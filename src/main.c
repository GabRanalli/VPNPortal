/*
 * vpnportal — la ventana: lista de VPN del usuario, diálogo para añadir /
 * editar / borrar, y registro con la salida de gpclient.
 *
 * Recordatorio: una app GTK vive en un BUCLE DE EVENTOS. main() prepara
 * la app y le cede el control; a partir de ahí GTK llama a nuestras
 * funciones (CALLBACKS) cuando ocurre algo (SEÑALES).
 *
 * Reparto del trabajo:
 *   - config.c  guarda y carga las VPN (~/.config/vpnportal/vpns.ini).
 *   - vpn.c     sabe lanzar/parar gpclient y nos avisa de lo que pasa.
 *   - auth.c    atiende las peticiones de login de gpclient (por D-Bus).
 *   - login.c   el diálogo con el navegador donde inicias sesión.
 *   - tray.c    el icono de la barra superior y su menú.
 *   - main.c    (este) pinta la ventana y reacciona a esos avisos.
 */

#include <adwaita.h>
#include <glib-unix.h>

#include "auth.h"
#include "config.h"
#include "login.h"
#include "tray.h"
#include "vpn.h"

/* Todo lo de una VPN de la lista: sus datos, su conexión y su fila. */
typedef struct {
  VpnConfig *config;    /* nuestro: se libera junto con la fila */
  Vpn       *vpn;
  GtkWidget *row;       /* AdwActionRow: nombre + estado */
  GtkWidget *spinner;   /* ruedecita mientras conecta/desconecta */
  GtkWidget *edit;      /* botón del lápiz */
  GtkWidget *button;    /* Conectar / Desconectar */
} VpnRow;

/*
 * Variables globales (static = solo visibles en este fichero). En una app
 * pequeña con una sola ventana es lo más sencillo.
 */
static GPtrArray     *rows;          /* VpnRow*, en el orden de la lista */
static GtkWindow     *main_window;
static GtkWidget     *stack;         /* página "empty" o "list" */
static GtkWidget     *vpn_group;     /* la tarjeta con las filas */
static GtkTextBuffer *log_buffer;
static GtkWidget     *log_view;
static char          *startup_error; /* error al cargar, para el registro */
static AdwDialog     *login_dialog;  /* el login abierto, o NULL */
static gboolean       quitting;      /* saliendo: esperando a desconectar */

static void open_edit_dialog (VpnRow *editing);

/* ---------------------------------------------------------------- */
/* Registro                                                         */
/* ---------------------------------------------------------------- */

/* Añade "[origen] línea" al final del registro y baja hasta ella. */
static void
append_log (const char *origin, const char *line)
{
  if (log_buffer == NULL)
    return;

  GtkTextIter end;
  gtk_text_buffer_get_end_iter (log_buffer, &end);
  g_autofree char *text = g_strdup_printf ("[%s] %s\n", origin, line);
  gtk_text_buffer_insert (log_buffer, &end, text, -1);

  /* Un "mark" es una posición del texto que se mantiene aunque el texto
   * cambie. Usamos uno al final para que la vista lo siga. */
  GtkTextMark *mark = gtk_text_buffer_get_mark (log_buffer, "end");
  gtk_text_view_scroll_mark_onscreen (GTK_TEXT_VIEW (log_view), mark);
}

/* ---------------------------------------------------------------- */
/* Guardar                                                          */
/* ---------------------------------------------------------------- */

static void
save_configs (void)
{
  /* Un array "prestado": apunta a los VpnConfig de las filas pero no
   * es su dueño (sin función de liberar), así que al destruirlo no
   * se libera ningún VpnConfig. */
  g_autoptr (GPtrArray) configs = g_ptr_array_new ();
  for (guint i = 0; i < rows->len; i++) {
    VpnRow *r = g_ptr_array_index (rows, i);
    g_ptr_array_add (configs, r->config);
  }

  g_autoptr (GError) error = NULL;
  if (!vpn_config_save (configs, &error))
    append_log ("App", error->message);
}

/* ---------------------------------------------------------------- */
/* Actualizar la interfaz                                           */
/* ---------------------------------------------------------------- */

/* Le pasa al icono de la barra el estado de todas las VPN. */
static void
refresh_tray (void)
{
  g_autofree TrayItem *items = g_new0 (TrayItem, rows->len);
  for (guint i = 0; i < rows->len; i++) {
    VpnRow *r = g_ptr_array_index (rows, i);
    items[i].id = r->config->id;
    items[i].name = r->config->name;
    items[i].state = vpn_get_state (r->vpn);
  }
  tray_update (items, rows->len);
}

/* Pone cada fila (y el icono de la barra) de acuerdo con el estado de su
 * VPN. Se llama cada vez que algo cambia: es el ÚNICO sitio que decide
 * cómo se ve todo. */
static void
refresh_rows (void)
{
  refresh_tray ();

  if (stack == NULL)
    return;   /* la ventana aún no existe */

  gtk_stack_set_visible_child_name (GTK_STACK (stack),
                                    rows->len > 0 ? "list" : "empty");

  /* gpclient solo permite una conexión a la vez: si alguna está en uso,
   * las demás no pueden conectar. */
  gboolean any_busy = FALSE;
  for (guint i = 0; i < rows->len; i++) {
    VpnRow *r = g_ptr_array_index (rows, i);
    if (vpn_get_state (r->vpn) != VPN_DISCONNECTED)
      any_busy = TRUE;
  }

  for (guint i = 0; i < rows->len; i++) {
    VpnRow *r = g_ptr_array_index (rows, i);
    VpnState state = vpn_get_state (r->vpn);
    GtkButton *button = GTK_BUTTON (r->button);

    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (r->row),
                                   r->config->name);
    adw_action_row_set_subtitle (ADW_ACTION_ROW (r->row),
                                 vpn_state_to_string (state));
    gtk_widget_set_visible (r->spinner, state == VPN_CONNECTING ||
                                        state == VPN_DISCONNECTING);

    /* Solo se puede editar si está desconectada. */
    gtk_widget_set_sensitive (r->edit, state == VPN_DISCONNECTED);

    /* Las clases CSS "suggested-action" (azul) y "destructive-action"
     * (rojo) son estilos que ya trae libadwaita. */
    gtk_widget_remove_css_class (r->button, "suggested-action");
    gtk_widget_remove_css_class (r->button, "destructive-action");

    switch (state) {
    case VPN_DISCONNECTED:
      gtk_button_set_label (button, "Conectar");
      gtk_widget_add_css_class (r->button, "suggested-action");
      gtk_widget_set_sensitive (r->button, !any_busy);
      break;
    case VPN_CONNECTING:
    case VPN_CONNECTED:
      gtk_button_set_label (button, "Desconectar");
      gtk_widget_add_css_class (r->button, "destructive-action");
      gtk_widget_set_sensitive (r->button, TRUE);
      break;
    case VPN_DISCONNECTING:
      gtk_button_set_label (button, "Desconectar");
      gtk_widget_set_sensitive (r->button, FALSE);
      break;
    }
  }
}

/* ---------------------------------------------------------------- */
/* Avisos que llegan desde vpn.c                                    */
/* ---------------------------------------------------------------- */

/* La fila cuya VPN está en uso (solo puede haber una), o NULL. */
static VpnRow *
find_active_row (void)
{
  for (guint i = 0; i < rows->len; i++) {
    VpnRow *r = g_ptr_array_index (rows, i);
    if (vpn_get_state (r->vpn) != VPN_DISCONNECTED)
      return r;
  }
  return NULL;
}

/*
 * Salir de verdad. Si hay una VPN en uso, primero se desconecta y se
 * ESPERA a que gpclient termine: al desconectar, gpclient deja las rutas
 * y el DNS como estaban, y si la app se cerrase antes podría quedarse a
 * medias. on_vpn_state hará el g_application_quit al acabar.
 */
static void
request_quit (void)
{
  VpnRow *active = find_active_row ();

  if (active == NULL) {
    g_application_quit (g_application_get_default ());
    return;
  }

  quitting = TRUE;
  if (main_window != NULL)
    gtk_widget_set_visible (GTK_WIDGET (main_window), FALSE);
  vpn_disconnect (active->vpn);
}

static void
on_vpn_state (Vpn *vpn, VpnState state, gpointer user_data)
{
  (void) vpn; (void) state; (void) user_data;

  if (quitting && find_active_row () == NULL) {
    g_application_quit (g_application_get_default ());
    return;
  }

  /* Si la VPN se ha parado con el login abierto, ya no sirve: fuera. */
  if (login_dialog != NULL && find_active_row () == NULL)
    adw_dialog_close (login_dialog);

  refresh_rows ();
}

/* gpclient necesita un login (nos llega desde vpnportal-auth por D-Bus). */
static void
on_auth_request (AuthRequest *request, gpointer user_data)
{
  (void) user_data;
  VpnRow *active = find_active_row ();

  /* Solo atendemos logins de una conexión que hayamos lanzado nosotros:
   * así ningún otro programa puede abrir páginas en nuestra ventana. */
  if (active == NULL || main_window == NULL) {
    auth_request_fail (request, "No connection in progress in VPN Portal");
    return;
  }
  if (login_dialog != NULL) {
    auth_request_fail (request, "Another login is already in progress");
    return;
  }

  append_log (active->config->name,
              auth_request_get_is_gateway (request)
              ? "» Iniciando sesión en la gateway…"
              : "» Iniciando sesión…");
  gtk_window_present (main_window);
  login_dialog = login_dialog_run (GTK_WIDGET (main_window),
                                   active->config->name, request);
  /* "Puntero débil": GTK lo pondrá a NULL solo cuando el diálogo se
   * destruya, así nunca apunta a un diálogo que ya no existe. */
  g_object_add_weak_pointer (G_OBJECT (login_dialog),
                             (gpointer *) &login_dialog);
}

static void
on_vpn_line (Vpn *vpn, const char *line, gpointer user_data)
{
  (void) user_data;
  append_log (vpn_get_config (vpn)->name, line);
}

/* ---------------------------------------------------------------- */
/* Filas de la lista                                                */
/* ---------------------------------------------------------------- */

/* Clic en el botón de una fila. user_data es el VpnRow de esa fila:
 * así un único callback sirve para todos los botones. */
static void
on_button_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  VpnRow *r = user_data;

  if (vpn_get_state (r->vpn) == VPN_DISCONNECTED)
    vpn_connect (r->vpn);
  else
    vpn_disconnect (r->vpn);
}

/* Clic en el lápiz: editar. */
static void
on_edit_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  VpnRow *r = user_data;

  if (vpn_get_state (r->vpn) == VPN_DISCONNECTED)
    open_edit_dialog (r);
}

/* Crea los widgets de una fila y la mete en la tarjeta. */
static void
vpn_row_build (VpnRow *r)
{
  r->row = adw_action_row_new ();
  adw_action_row_add_prefix (ADW_ACTION_ROW (r->row),
                             gtk_image_new_from_icon_name ("network-vpn-symbolic"));

  r->spinner = adw_spinner_new ();
  adw_action_row_add_suffix (ADW_ACTION_ROW (r->row), r->spinner);

  /* "flat" = botón sin fondo, solo el icono. Un botón que solo tiene
   * icono necesita un nombre para los lectores de pantalla. */
  r->edit = gtk_button_new_from_icon_name ("document-edit-symbolic");
  gtk_widget_set_valign (r->edit, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (r->edit, "flat");
  gtk_widget_set_tooltip_text (r->edit, "Editar");
  gtk_accessible_update_property (GTK_ACCESSIBLE (r->edit),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL, "Editar",
                                  -1);
  g_signal_connect (r->edit, "clicked", G_CALLBACK (on_edit_clicked), r);
  adw_action_row_add_suffix (ADW_ACTION_ROW (r->row), r->edit);

  r->button = gtk_button_new_with_label ("Conectar");
  gtk_widget_set_valign (r->button, GTK_ALIGN_CENTER);
  g_signal_connect (r->button, "clicked", G_CALLBACK (on_button_clicked), r);
  adw_action_row_add_suffix (ADW_ACTION_ROW (r->row), r->button);

  adw_preferences_group_add (ADW_PREFERENCES_GROUP (vpn_group), r->row);
}

/* Nueva fila para 'config' (la fila pasa a ser su dueña). */
static VpnRow *
vpn_row_new (VpnConfig *config)
{
  VpnRow *r = g_new0 (VpnRow, 1);
  r->config = config;
  r->vpn = vpn_new (config, on_vpn_state, on_vpn_line, NULL);
  g_ptr_array_add (rows, r);

  if (vpn_group != NULL)   /* si la ventana ya existe, se ve al momento */
    vpn_row_build (r);
  return r;
}

/* El orden importa: el Vpn usa el config, así que el Vpn va primero. */
static void
vpn_row_free (VpnRow *r)
{
  vpn_free (r->vpn);
  vpn_config_free (r->config);
  g_free (r);
}

/* ---------------------------------------------------------------- */
/* Diálogo de añadir / editar                                       */
/* ---------------------------------------------------------------- */

typedef struct {
  AdwDialog *dialog;
  VpnRow    *editing;     /* NULL = VPN nueva */
  GtkWidget *name;
  GtkWidget *portal;
  GtkWidget *gateway;
  GtkWidget *user;
  GtkWidget *hip;
  GtkWidget *save;
} EditDialog;

/* Texto de un campo sin espacios al principio ni al final (hay que
 * liberarlo: g_strstrip trabaja sobre una copia nuestra). */
static char *
entry_text (GtkWidget *entry)
{
  return g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (entry))));
}

/* Marca un campo en rojo (clase CSS "error") si 'bad' es TRUE. */
static void
mark_error (GtkWidget *entry, gboolean bad)
{
  if (bad)
    gtk_widget_add_css_class (entry, "error");
  else
    gtk_widget_remove_css_class (entry, "error");
}

/* Se llama con cada tecla: decide si se puede pulsar Guardar. */
static void
edit_dialog_validate (EditDialog *d)
{
  g_autofree char *name = entry_text (d->name);
  g_autofree char *portal = entry_text (d->portal);
  g_autofree char *gateway = entry_text (d->gateway);
  g_autofree char *user = entry_text (d->user);

  /* Los campos vacíos no se marcan en rojo (aún no has escrito nada). */
  gboolean portal_ok = vpn_config_host_is_valid (portal);
  gboolean gateway_ok = *gateway == '\0' || vpn_config_host_is_valid (gateway);
  gboolean user_ok = *user == '\0' || vpn_config_user_is_valid (user);

  mark_error (d->portal, *portal != '\0' && !portal_ok);
  mark_error (d->gateway, !gateway_ok);
  mark_error (d->user, !user_ok);

  gtk_widget_set_sensitive (d->save, *name != '\0' && portal_ok &&
                                     gateway_ok && user_ok);
}

/* Cambia el texto de un campo de VpnConfig liberando el anterior. */
static void
replace_string (char **field, char *value)
{
  g_free (*field);
  *field = value;
}

static void
on_save_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  EditDialog *d = user_data;

  /* Por si algo la conectó mientras el diálogo estaba abierto. */
  if (d->editing != NULL &&
      vpn_get_state (d->editing->vpn) != VPN_DISCONNECTED)
    return;

  VpnConfig *config = d->editing != NULL ? d->editing->config
                                         : vpn_config_new ();

  replace_string (&config->name, entry_text (d->name));
  replace_string (&config->portal, entry_text (d->portal));
  replace_string (&config->gateway, entry_text (d->gateway));
  replace_string (&config->user, entry_text (d->user));
  config->hip = adw_switch_row_get_active (ADW_SWITCH_ROW (d->hip));

  if (d->editing == NULL)
    vpn_row_new (config);

  save_configs ();
  refresh_rows ();
  adw_dialog_close (d->dialog);
}

static void
on_delete_confirmed (AdwAlertDialog *alert, const char *response,
                     gpointer user_data)
{
  (void) alert; (void) response;
  EditDialog *d = user_data;
  VpnRow *r = d->editing;

  adw_preferences_group_remove (ADW_PREFERENCES_GROUP (vpn_group), r->row);
  /* El array tiene vpn_row_free como función de liberar: quitarla del
   * array ya la libera. */
  g_ptr_array_remove (rows, r);

  save_configs ();
  refresh_rows ();
  adw_dialog_close (d->dialog);
}

static void
on_delete_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  EditDialog *d = user_data;

  /* AdwAlertDialog: la típica pregunta de confirmación de GNOME. */
  AdwDialog *alert = adw_alert_dialog_new ("¿Eliminar esta VPN?", NULL);
  adw_alert_dialog_format_body (ADW_ALERT_DIALOG (alert),
                                "Se borrará «%s» de la lista.",
                                d->editing->config->name);
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (alert),
                                  "cancel", "Cancelar",
                                  "delete", "Eliminar",
                                  NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (alert), "delete",
                                            ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (alert), "cancel");

  /* "response::delete" = la señal "response", pero solo cuando la
   * respuesta es "delete" (a esto se le llama "detalle" de la señal). */
  g_signal_connect (alert, "response::delete",
                    G_CALLBACK (on_delete_confirmed), d);
  adw_dialog_present (alert, GTK_WIDGET (d->dialog));
}

static GtkWidget *
add_entry (GtkWidget *group, const char *title, const char *text,
           EditDialog *d)
{
  GtkWidget *entry = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (entry), title);
  gtk_editable_set_text (GTK_EDITABLE (entry), text);
  /* _swapped: llama a edit_dialog_validate(d) en vez de (entry, d). */
  g_signal_connect_swapped (entry, "changed",
                            G_CALLBACK (edit_dialog_validate), d);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), entry);
  return entry;
}

static void
open_edit_dialog (VpnRow *editing)
{
  EditDialog *d = g_new0 (EditDialog, 1);
  const VpnConfig *config = editing != NULL ? editing->config : NULL;

  d->editing = editing;
  d->dialog = adw_dialog_new ();
  adw_dialog_set_title (d->dialog, editing != NULL ? "Editar VPN"
                                                   : "Nueva VPN");
  adw_dialog_set_content_width (d->dialog, 460);
  /* Guardamos 'd' dentro del diálogo: cuando el diálogo se destruya,
   * GTK llamará a g_free(d) por nosotros. */
  g_object_set_data_full (G_OBJECT (d->dialog), "edit-dialog", d, g_free);

  /* Barra de título: Cancelar a la izquierda, Guardar a la derecha. */
  GtkWidget *header = adw_header_bar_new ();
  adw_header_bar_set_show_start_title_buttons (ADW_HEADER_BAR (header), FALSE);
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);

  GtkWidget *cancel = gtk_button_new_with_label ("Cancelar");
  g_signal_connect_swapped (cancel, "clicked",
                            G_CALLBACK (adw_dialog_close), d->dialog);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), cancel);

  d->save = gtk_button_new_with_label ("Guardar");
  gtk_widget_add_css_class (d->save, "suggested-action");
  g_signal_connect (d->save, "clicked", G_CALLBACK (on_save_clicked), d);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), d->save);

  /* Los campos. */
  GtkWidget *group = adw_preferences_group_new ();
  adw_preferences_group_set_description (ADW_PREFERENCES_GROUP (group),
    "La primera vez que conectes a un servidor nuevo te pedirá la "
    "contraseña de administrador para aprobarlo.");
  d->name = add_entry (group, "Nombre", config ? config->name : "", d);
  d->portal = add_entry (group, "Portal", config ? config->portal : "", d);
  d->gateway = add_entry (group, "Gateway (vacío = automática)",
                          config ? config->gateway : "", d);
  d->user = add_entry (group, "Usuario (opcional)",
                       config ? config->user : "", d);

  d->hip = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (d->hip),
                                 "Enviar informe HIP");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (d->hip),
                               "Solo si tu organización lo exige");
  adw_switch_row_set_active (ADW_SWITCH_ROW (d->hip),
                             config != NULL && config->hip);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), d->hip);

  GtkWidget *page = adw_preferences_page_new ();
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            ADW_PREFERENCES_GROUP (group));

  /* Al editar, un botón rojo para eliminar, debajo de los campos. */
  if (editing != NULL) {
    GtkWidget *delete_button = gtk_button_new_with_label ("Eliminar VPN");
    gtk_widget_set_halign (delete_button, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class (delete_button, "pill");
    gtk_widget_add_css_class (delete_button, "destructive-action");
    g_signal_connect (delete_button, "clicked",
                      G_CALLBACK (on_delete_clicked), d);

    GtkWidget *delete_group = adw_preferences_group_new ();
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (delete_group),
                               delete_button);
    adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                              ADW_PREFERENCES_GROUP (delete_group));
  }

  GtkWidget *toolbar_view = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), header);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), page);
  adw_dialog_set_child (d->dialog, toolbar_view);

  edit_dialog_validate (d);
  adw_dialog_present (d->dialog, GTK_WIDGET (main_window));
}

/* ---------------------------------------------------------------- */
/* Construir la ventana                                             */
/* ---------------------------------------------------------------- */

static void
on_add_clicked (GtkButton *button, gpointer user_data)
{
  (void) button; (void) user_data;
  open_edit_dialog (NULL);
}

/* Lo que se ve cuando aún no hay ninguna VPN. */
static GtkWidget *
build_empty_page (void)
{
  GtkWidget *status = adw_status_page_new ();
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (status),
                                 "network-vpn-symbolic");
  adw_status_page_set_title (ADW_STATUS_PAGE (status), "Sin VPN");
  adw_status_page_set_description (ADW_STATUS_PAGE (status),
                                   "Añade tu primera VPN de GlobalProtect.");

  GtkWidget *add = gtk_button_new_with_label ("Añadir VPN");
  gtk_widget_set_halign (add, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (add, "pill");
  gtk_widget_add_css_class (add, "suggested-action");
  g_signal_connect (add, "clicked", G_CALLBACK (on_add_clicked), NULL);
  adw_status_page_set_child (ADW_STATUS_PAGE (status), add);
  return status;
}

static GtkWidget *
build_log_group (void)
{
  GtkWidget *group = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (group),
                                   "Registro");

  /* GtkTextView muestra un GtkTextBuffer: la vista y el texto van por
   * separado (el mismo texto podría verse en dos sitios a la vez). */
  log_view = gtk_text_view_new ();
  gtk_text_view_set_editable (GTK_TEXT_VIEW (log_view), FALSE);
  gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (log_view), FALSE);
  gtk_text_view_set_monospace (GTK_TEXT_VIEW (log_view), TRUE);
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (log_view), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_left_margin (GTK_TEXT_VIEW (log_view), 8);
  gtk_text_view_set_top_margin (GTK_TEXT_VIEW (log_view), 8);
  log_buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (log_view));

  GtkTextIter end;
  gtk_text_buffer_get_end_iter (log_buffer, &end);
  gtk_text_buffer_create_mark (log_buffer, "end", &end, FALSE);

  /* La vista de texto va dentro de una ventana con barra de scroll. */
  GtkWidget *scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), log_view);
  gtk_scrolled_window_set_min_content_height (GTK_SCROLLED_WINDOW (scroller),
                                              220);
  gtk_widget_add_css_class (scroller, "card");   /* bordes redondeados */

  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), scroller);
  return group;
}

/* La lista de VPN + el registro. */
static GtkWidget *
build_list_page (void)
{
  vpn_group = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (vpn_group),
                                   "Conexiones");
  for (guint i = 0; i < rows->len; i++)
    vpn_row_build (g_ptr_array_index (rows, i));

  /* AdwPreferencesPage ya trae scroll y márgenes bonitos. */
  GtkWidget *page = adw_preferences_page_new ();
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            ADW_PREFERENCES_GROUP (vpn_group));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            ADW_PREFERENCES_GROUP (build_log_group ()));
  return page;
}

/* Al pulsar la X de la ventana. Devolver TRUE significa "ya me encargo
 * yo, no la destruyas": la ocultamos y la app sigue en la barra. */
static gboolean
on_close_request (GtkWindow *window, gpointer user_data)
{
  GApplication *app = user_data;

  if (tray_is_available ()) {
    gtk_widget_set_visible (GTK_WIDGET (window), FALSE);
    return TRUE;
  }

  /* Sin icono en la barra no habría forma de volver: salimos. */
  (void) app;
  request_quit ();
  return TRUE;
}

static void
on_activate (GtkApplication *app, gpointer user_data)
{
  (void) user_data;

  if (main_window != NULL) {
    gtk_window_present (main_window);
    return;
  }

  GtkWidget *window = adw_application_window_new (app);
  main_window = GTK_WINDOW (window);
  gtk_window_set_title (main_window, "VPN Portal");
  gtk_window_set_default_size (main_window, 520, 600);
  g_signal_connect (window, "close-request",
                    G_CALLBACK (on_close_request), app);

  /* Botón "+" en la barra de título. */
  GtkWidget *header = adw_header_bar_new ();
  GtkWidget *add = gtk_button_new_from_icon_name ("list-add-symbolic");
  gtk_widget_set_tooltip_text (add, "Añadir VPN");
  g_signal_connect (add, "clicked", G_CALLBACK (on_add_clicked), NULL);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), add);

  /* GtkStack: varias páginas apiladas, se ve una cada vez. */
  stack = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (stack),
                                 GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_add_named (GTK_STACK (stack), build_empty_page (), "empty");
  gtk_stack_add_named (GTK_STACK (stack), build_list_page (), "list");

  GtkWidget *toolbar_view = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), header);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), stack);
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (window),
                                      toolbar_view);

  if (startup_error != NULL)
    append_log ("App", startup_error);

  refresh_rows ();
  gtk_window_present (main_window);
}

/* ---------------------------------------------------------------- */
/* Lo que se pide desde el menú de la barra                         */
/* ---------------------------------------------------------------- */

static void
on_tray_toggle (const char *id, gpointer user_data)
{
  (void) user_data;
  for (guint i = 0; i < rows->len; i++) {
    VpnRow *r = g_ptr_array_index (rows, i);
    if (!g_str_equal (r->config->id, id))
      continue;
    if (vpn_get_state (r->vpn) == VPN_DISCONNECTED)
      vpn_connect (r->vpn);
    else
      vpn_disconnect (r->vpn);
    return;
  }
}

static void
on_tray_show (const char *activation_token, gpointer user_data)
{
  /* El token es el permiso de GNOME para poner la ventana delante (en
   * Wayland una app no puede hacerlo por su cuenta). GTK lo usa en el
   * siguiente gtk_window_present. */
  if (main_window != NULL && activation_token != NULL)
    gtk_window_set_startup_id (main_window, activation_token);

  /* "activate" es lo mismo que abrir la app desde el lanzador: si la
   * ventana existe, on_activate la vuelve a mostrar. */
  g_application_activate (G_APPLICATION (user_data));
}

static void
on_tray_quit (gpointer user_data)
{
  (void) user_data;
  request_quit ();
}

/* La acción "app.quit" (Ctrl+Q). */
static void
on_quit_action (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  (void) action; (void) parameter; (void) user_data;
  request_quit ();
}

/* Señales del sistema (SIGTERM al cerrar sesión, SIGINT con Ctrl+C en la
 * terminal): salir igual de limpio. Si insisten, salir ya. */
static gboolean
on_unix_signal (gpointer user_data)
{
  (void) user_data;
  if (quitting)
    g_application_quit (g_application_get_default ());
  else
    request_quit ();
  return G_SOURCE_CONTINUE;
}

/* ---------------------------------------------------------------- */
/* Arranque y cierre                                                */
/* ---------------------------------------------------------------- */

/*
 * "startup" se emite UNA vez al arrancar (antes de "activate"), y
 * "shutdown" una vez al terminar. Las VPN viven toda la vida de la app,
 * así que se cargan y se liberan aquí, no con la ventana.
 */
static void
on_startup (GApplication *app, gpointer user_data)
{
  (void) user_data;
  g_autoptr (GError) error = NULL;

  rows = g_ptr_array_new_with_free_func ((GDestroyNotify) vpn_row_free);

  /* "hold": que la app siga viva aunque no tenga ninguna ventana
   * visible (por defecto GTK sale al cerrar la última ventana). */
  g_application_hold (app);

  static const TrayCallbacks tray_callbacks = {
    .toggle = on_tray_toggle,
    .show = on_tray_show,
    .quit = on_tray_quit,
  };
  tray_init (VPNPORTAL_APP_ID, &tray_callbacks, app);

  g_unix_signal_add (SIGTERM, on_unix_signal, NULL);
  g_unix_signal_add (SIGINT, on_unix_signal, NULL);

  /* Ctrl+Q para salir del todo (desconectando). */
  static const GActionEntry app_actions[] = {
    { .name = "quit", .activate = on_quit_action },
  };
  g_action_map_add_action_entries (G_ACTION_MAP (app), app_actions,
                                   G_N_ELEMENTS (app_actions), app);
  gtk_application_set_accels_for_action (GTK_APPLICATION (app), "app.quit",
                                         (const char *[]) { "<Control>q",
                                                            NULL });

  g_autoptr (GPtrArray) configs = vpn_config_load (&error);
  if (error != NULL)
    startup_error = g_strdup_printf ("No se pudo leer la configuración: %s",
                                     error->message);

  /* Traspaso de dueño: a partir de aquí cada VpnConfig es de su fila,
   * así que el array 'configs' ya no debe liberarlos al destruirse. */
  g_ptr_array_set_free_func (configs, NULL);
  for (guint i = 0; i < configs->len; i++)
    vpn_row_new (g_ptr_array_index (configs, i));
  refresh_tray ();

  /* Publicamos el servicio de login en el bus de sesión. */
  GDBusConnection *bus = g_application_get_dbus_connection (app);
  g_clear_error (&error);
  if (bus == NULL)
    startup_error = g_strdup ("Sin bus de sesión: el login no funcionará");
  else if (!auth_service_start (bus, on_auth_request, NULL, &error))
    startup_error = g_strdup_printf ("No se pudo publicar el login: %s",
                                     error->message);
}

static void
on_shutdown (GApplication *app, gpointer user_data)
{
  (void) app; (void) user_data;
  /* La ventana ya no existe: que nadie la toque. */
  main_window = NULL;
  stack = NULL;
  vpn_group = NULL;
  log_view = NULL;
  log_buffer = NULL;
  auth_service_stop ();
  tray_shutdown ();
  g_clear_pointer (&rows, g_ptr_array_unref);
  g_clear_pointer (&startup_error, g_free);
}

int
main (int argc, char *argv[])
{
  AdwApplication *app = adw_application_new (VPNPORTAL_APP_ID,
                                             G_APPLICATION_DEFAULT_FLAGS);

  g_signal_connect (app, "startup",  G_CALLBACK (on_startup),  NULL);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  g_signal_connect (app, "shutdown", G_CALLBACK (on_shutdown), NULL);

  int status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);
  return status;
}
