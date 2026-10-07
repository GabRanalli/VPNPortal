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
#include <errno.h>
#include <glib-unix.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <locale.h>
#include <unistd.h>

#include "auth.h"
#include "config.h"
#include "login.h"
#include "theme.h"
#include "tray.h"
#include "vpn.h"

/* gettext (traducciones): el "dominio" (nombre de los ficheros .mo) y
 * dónde están instalados. Meson los define; estos son por si se compila
 * a mano (la demo). */
#ifndef GETTEXT_PACKAGE
#define GETTEXT_PACKAGE "vpnportal"
#endif
#ifndef LOCALEDIR
#define LOCALEDIR "/usr/local/share/locale"
#endif

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
static GtkWidget     *toast_overlay; /* donde salen los avisos breves */
static GtkWidget     *vpn_group;     /* la tarjeta con las filas */
static GtkTextBuffer *log_buffer;    /* el registro completo (texto) */
static GtkWidget     *log_view;      /* su vista, si el diálogo está abierto */
static char          *startup_error; /* error al cargar, para el registro */
static AdwDialog     *login_dialog;  /* el login abierto, o NULL */
static gboolean       quitting;      /* saliendo: esperando a desconectar */
static gboolean       restarting;    /* al salir, volver a arrancar */
static VpnRow        *pending_switch; /* VPN a conectar en cuanto se
                                       * desconecte la actual */

/* El resumen de _("Activity"): los pasos del último intento de conexión. */
typedef enum {
  STEP_ICON_NONE,
  STEP_ICON_DONE,      /* ✓ */
  STEP_ICON_RUNNING,   /* ruedecita */
  STEP_ICON_ERROR,     /* ⚠ */
} StepIcon;

typedef struct {
  VpnStepKind  kind;
  char        *text;
  char        *time;       /* "12:41:03" */
  GtkWidget   *row;        /* su fila en la lista (si la ventana existe) */
  GtkWidget   *icon;       /* el icono que tiene puesto ahora */
  StepIcon     icon_kind;
} Step;

#define MAX_STEPS 50

static GPtrArray     *steps;          /* de Step*, el más antiguo primero */
static Vpn           *steps_vpn;      /* la VPN de esos pasos */
static GtkWidget     *activity_group;
static GtkWidget     *activity_list;  /* GtkListBox con un paso por fila */
static gboolean       activity_follow = TRUE;  /* ¿vista pegada al final? */

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
   * cambie. Usamos uno al final para que la vista (si está abierta) lo
   * siga. */
  if (log_view != NULL) {
    GtkTextMark *mark = gtk_text_buffer_get_mark (log_buffer, "end");
    gtk_text_view_scroll_mark_onscreen (GTK_TEXT_VIEW (log_view), mark);
  }
}

/* ---------------------------------------------------------------- */
/* Actividad (el resumen)                                           */
/* ---------------------------------------------------------------- */

static void
step_free (Step *step)
{
  g_free (step->text);
  g_free (step->time);
  g_free (step);
}

/* Qué icono le toca a un paso: error, "en curso" (solo el último, y
 * solo mientras la VPN está ocupada) o hecho. */
static StepIcon
step_wanted_icon (const Step *step, gboolean last, gboolean busy)
{
  if (step->kind == VPN_STEP_ERROR)
    return STEP_ICON_ERROR;
  if (last && busy && step->kind != VPN_STEP_DONE)
    return STEP_ICON_RUNNING;
  return STEP_ICON_DONE;
}

/* Cambia el icono de la fila solo si hace falta (cambiarlo siempre
 * reiniciaría la animación de la ruedecita). */
static void
step_update_icon (Step *step, gboolean last, gboolean busy)
{
  StepIcon wanted = step_wanted_icon (step, last, busy);

  if (step->row == NULL || wanted == step->icon_kind)
    return;

  if (step->icon != NULL)
    adw_action_row_remove (ADW_ACTION_ROW (step->row), step->icon);

  switch (wanted) {
  case STEP_ICON_ERROR:
    step->icon = gtk_image_new_from_icon_name ("dialog-error-symbolic");
    gtk_widget_add_css_class (step->icon, "error");
    break;
  case STEP_ICON_RUNNING:
    step->icon = adw_spinner_new ();
    break;
  default:
    step->icon = gtk_image_new_from_icon_name ("object-select-symbolic");
    gtk_widget_add_css_class (step->icon, "dim-label");
    break;
  }
  adw_action_row_add_prefix (ADW_ACTION_ROW (step->row), step->icon);
  step->icon_kind = wanted;
}

/* Crea la fila de un paso al final de la lista (el icono lo pone
 * refresh_activity). */
static void
step_build_row (Step *step)
{
  step->row = adw_action_row_new ();
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (step->row), FALSE);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (step->row), step->text);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (step->row), step->time);
  step->icon = NULL;
  step->icon_kind = STEP_ICON_NONE;
  gtk_list_box_append (GTK_LIST_BOX (activity_list), step->row);
}

/* Pone al día el título y los iconos. Las filas no se rehacen: si se
 * rehiciesen, el scroll saltaría cada vez. */
static void
refresh_activity (void)
{
  if (activity_list == NULL)
    return;

  /* El título de una tarjeta admite "markup" (<b>, &amp;...): el nombre
   * de la VPN hay que escaparlo, o un "&" lo rompería. */
  g_autofree char *title = NULL;
  if (steps_vpn != NULL) {
    g_autofree char *name =
      g_markup_escape_text (vpn_get_config (steps_vpn)->name, -1);
    title = g_strdup_printf (_("Activity · %s"), name);
  }
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (activity_group),
                                   title != NULL ? title : _("Activity"));

  gboolean busy = steps_vpn != NULL &&
                  vpn_get_state (steps_vpn) != VPN_DISCONNECTED;

  for (guint i = 0; i < steps->len; i++)
    step_update_icon (g_ptr_array_index (steps, i), i == steps->len - 1, busy);
}

/* Llega un paso nuevo desde vpn.c. */
static void
on_vpn_step (Vpn *vpn, VpnStepKind kind, const char *text, gpointer user_data)
{
  (void) user_data;

  /* Un intento nuevo (o de otra VPN): lista limpia. */
  if (kind == VPN_STEP_BEGIN || vpn != steps_vpn) {
    g_ptr_array_set_size (steps, 0);
    if (activity_list != NULL)
      gtk_list_box_remove_all (GTK_LIST_BOX (activity_list));
    steps_vpn = vpn;
    activity_follow = TRUE;
  }

  /* gpclient a veces repite un mensaje: no lo contamos dos veces. */
  if (steps->len > 0) {
    Step *previous = g_ptr_array_index (steps, steps->len - 1);
    if (g_str_equal (previous->text, text))
      return;
  }

  Step *step = g_new0 (Step, 1);
  step->kind = kind;
  step->text = g_strdup (text);
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  step->time = g_date_time_format (now, "%H:%M:%S");
  g_ptr_array_add (steps, step);
  if (activity_list != NULL)
    step_build_row (step);

  if (steps->len > MAX_STEPS) {           /* fuera el más antiguo */
    Step *oldest = g_ptr_array_index (steps, 0);
    if (oldest->row != NULL && activity_list != NULL)
      gtk_list_box_remove (GTK_LIST_BOX (activity_list), oldest->row);
    g_ptr_array_remove_index (steps, 0);
  }

  refresh_activity ();
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
  refresh_activity ();   /* la ruedecita depende del estado */

  if (stack == NULL)
    return;   /* la ventana aún no existe */

  gtk_stack_set_visible_child_name (GTK_STACK (stack),
                                    rows->len > 0 ? "list" : "empty");

  /* gpclient solo permite una conexión a la vez: si alguna está en uso,
   * el botón de las demás es _("Switch") (desconecta una y conecta otra). */
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
      gtk_button_set_label (button, any_busy ? _("Switch") : _("Connect"));
      gtk_widget_add_css_class (r->button, "suggested-action");
      gtk_widget_set_sensitive (r->button, TRUE);
      break;
    case VPN_CONNECTING:
    case VPN_CONNECTED:
      gtk_button_set_label (button, _("Disconnect"));
      gtk_widget_add_css_class (r->button, "destructive-action");
      gtk_widget_set_sensitive (r->button, TRUE);
      break;
    case VPN_DISCONNECTING:
      gtk_button_set_label (button, _("Disconnect"));
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

  /* Cambio de VPN: la anterior ya está desconectada del todo, ahora sí
   * podemos lanzar la nueva (gpclient no admite dos a la vez). */
  if (pending_switch != NULL && find_active_row () == NULL) {
    VpnRow *next = g_steal_pointer (&pending_switch);
    vpn_connect (next->vpn);
  }

  refresh_rows ();
}

/* Cambiar a 'target' (ya confirmado): desconectar la actual y, cuando
 * termine, on_vpn_state conecta 'target'. */
static void
switch_to (VpnRow *target)
{
  VpnRow *active = find_active_row ();

  if (active == NULL) {   /* mientras preguntábamos, ya se desconectó */
    vpn_connect (target->vpn);
    return;
  }
  if (active == target)
    return;

  pending_switch = target;
  g_autofree char *note = g_strdup_printf (_("» Switching to %s: this one is "
                                             "disconnected first…"),
                                           target->config->name);
  append_log (active->config->name, note);
  vpn_disconnect (active->vpn);
}

static void
on_switch_confirmed (AdwAlertDialog *alert, const char *response,
                     gpointer user_data)
{
  (void) alert; (void) response;
  switch_to (user_data);
}

/* Conectar 'target'. Si ya hay otra VPN en uso, primero se pregunta si
 * se quiere cambiar (desconectando la actual). */
static void
request_connect (VpnRow *target)
{
  VpnRow *active = find_active_row ();

  if (active == NULL) {
    vpn_connect (target->vpn);
    return;
  }
  if (active == target || main_window == NULL)
    return;

  gtk_window_present (main_window);

  AdwDialog *alert = adw_alert_dialog_new (NULL, NULL);
  adw_alert_dialog_format_heading (ADW_ALERT_DIALOG (alert),
                                   _("Switch to %s?"), target->config->name);
  adw_alert_dialog_format_body (ADW_ALERT_DIALOG (alert),
                                _("%s will be disconnected and then %s will be connected."),
                                active->config->name, target->config->name);
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (alert),
                                  "cancel", _("Cancel"),
                                  "switch", _("Switch"),
                                  NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (alert), "switch",
                                            ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (alert), "switch");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (alert), "cancel");
  g_signal_connect (alert, "response::switch",
                    G_CALLBACK (on_switch_confirmed), target);
  adw_dialog_present (alert, GTK_WIDGET (main_window));
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
              ? _("» Signing in to the gateway…")
              : _("» Signing in…"));
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
    request_connect (r);
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
  /* El título será el nombre de la VPN, texto tal cual (sin "markup"). */
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (r->row), FALSE);
  adw_action_row_add_prefix (ADW_ACTION_ROW (r->row),
                             gtk_image_new_from_icon_name ("network-vpn-symbolic"));

  r->spinner = adw_spinner_new ();
  adw_action_row_add_suffix (ADW_ACTION_ROW (r->row), r->spinner);

  /* "flat" = botón sin fondo, solo el icono. Un botón que solo tiene
   * icono necesita un nombre para los lectores de pantalla. */
  r->edit = gtk_button_new_from_icon_name ("document-edit-symbolic");
  gtk_widget_set_valign (r->edit, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (r->edit, "flat");
  gtk_widget_set_tooltip_text (r->edit, _("Edit"));
  gtk_accessible_update_property (GTK_ACCESSIBLE (r->edit),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL, _("Edit"),
                                  -1);
  g_signal_connect (r->edit, "clicked", G_CALLBACK (on_edit_clicked), r);
  adw_action_row_add_suffix (ADW_ACTION_ROW (r->row), r->edit);

  r->button = gtk_button_new_with_label (_("Connect"));
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
  r->vpn = vpn_new (config, on_vpn_state, on_vpn_line, on_vpn_step, NULL);
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
  if (pending_switch == r)
    pending_switch = NULL;
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
  AdwDialog *alert = adw_alert_dialog_new (_("Delete this VPN?"), NULL);
  adw_alert_dialog_format_body (ADW_ALERT_DIALOG (alert),
                                _("“%s” will be removed from the list."),
                                d->editing->config->name);
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (alert),
                                  "cancel", _("Cancel"),
                                  "delete", _("Delete"),
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
  adw_dialog_set_title (d->dialog, editing != NULL ? _("Edit VPN")
                                                   : _("New VPN"));
  adw_dialog_set_content_width (d->dialog, 460);
  /* Guardamos 'd' dentro del diálogo: cuando el diálogo se destruya,
   * GTK llamará a g_free(d) por nosotros. */
  g_object_set_data_full (G_OBJECT (d->dialog), "edit-dialog", d, g_free);

  /* Barra de título: Cancelar a la izquierda, Guardar a la derecha. */
  GtkWidget *header = adw_header_bar_new ();
  adw_header_bar_set_show_start_title_buttons (ADW_HEADER_BAR (header), FALSE);
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);

  GtkWidget *cancel = gtk_button_new_with_label (_("Cancel"));
  g_signal_connect_swapped (cancel, "clicked",
                            G_CALLBACK (adw_dialog_close), d->dialog);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), cancel);

  d->save = gtk_button_new_with_label (_("Save"));
  gtk_widget_add_css_class (d->save, "suggested-action");
  g_signal_connect (d->save, "clicked", G_CALLBACK (on_save_clicked), d);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), d->save);

  /* Los campos. */
  GtkWidget *group = adw_preferences_group_new ();
  adw_preferences_group_set_description (ADW_PREFERENCES_GROUP (group),
    _("The first time you connect to a new server, you will be asked for "
      "the administrator password to approve it."));
  d->name = add_entry (group, _("Name"), config ? config->name : "", d);
  d->portal = add_entry (group, _("Portal"), config ? config->portal : "", d);
  d->gateway = add_entry (group, _("Gateway (empty = automatic)"),
                          config ? config->gateway : "", d);
  d->user = add_entry (group, _("User (optional)"),
                       config ? config->user : "", d);

  d->hip = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (d->hip),
                                 _("Send HIP report"));
  adw_action_row_set_subtitle (ADW_ACTION_ROW (d->hip),
                               _("Only if your organization requires it"));
  adw_switch_row_set_active (ADW_SWITCH_ROW (d->hip),
                             config != NULL && config->hip);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), d->hip);

  GtkWidget *page = adw_preferences_page_new ();
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            ADW_PREFERENCES_GROUP (group));

  /* Al editar, un botón rojo para eliminar, debajo de los campos. */
  if (editing != NULL) {
    GtkWidget *delete_button = gtk_button_new_with_label (_("Delete VPN"));
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
  adw_status_page_set_title (ADW_STATUS_PAGE (status), _("No VPNs"));
  adw_status_page_set_description (ADW_STATUS_PAGE (status),
                                   _("Add your first GlobalProtect VPN."));

  GtkWidget *add = gtk_button_new_with_label (_("Add VPN"));
  gtk_widget_set_halign (add, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (add, "pill");
  gtk_widget_add_css_class (add, "suggested-action");
  g_signal_connect (add, "clicked", G_CALLBACK (on_add_clicked), NULL);
  adw_status_page_set_child (ADW_STATUS_PAGE (status), add);
  return status;
}

/* "Copiar" en el diálogo del registro: todo el texto al portapapeles. */
static void
on_copy_log_clicked (GtkButton *button, gpointer user_data)
{
  AdwToastOverlay *overlay = user_data;
  GtkTextIter start, end;

  gtk_text_buffer_get_bounds (log_buffer, &start, &end);
  g_autofree char *text = gtk_text_buffer_get_text (log_buffer, &start, &end,
                                                    FALSE);
  gdk_clipboard_set_text (gtk_widget_get_clipboard (GTK_WIDGET (button)), text);

  /* Un "toast": el mensajito que aparece abajo unos segundos. */
  adw_toast_overlay_add_toast (overlay, adw_toast_new (_("Log copied")));
}

/* El registro completo, en un diálogo aparte. */
static void
on_show_log_clicked (GtkButton *button, gpointer user_data)
{
  (void) button; (void) user_data;

  /* GtkTextView muestra un GtkTextBuffer: la vista y el texto van por
   * separado. El texto vive toda la app; la vista, lo que dure el
   * diálogo. */
  GtkWidget *view = gtk_text_view_new_with_buffer (log_buffer);
  gtk_text_view_set_editable (GTK_TEXT_VIEW (view), FALSE);
  gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (view), FALSE);
  gtk_text_view_set_monospace (GTK_TEXT_VIEW (view), TRUE);
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (view), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_left_margin (GTK_TEXT_VIEW (view), 12);
  gtk_text_view_set_right_margin (GTK_TEXT_VIEW (view), 12);
  gtk_text_view_set_top_margin (GTK_TEXT_VIEW (view), 12);
  gtk_text_view_set_bottom_margin (GTK_TEXT_VIEW (view), 12);

  GtkWidget *scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), view);
  gtk_widget_set_vexpand (scroller, TRUE);

  GtkWidget *overlay = adw_toast_overlay_new ();
  adw_toast_overlay_set_child (ADW_TOAST_OVERLAY (overlay), scroller);

  GtkWidget *copy = gtk_button_new_from_icon_name ("edit-copy-symbolic");
  gtk_widget_set_tooltip_text (copy, _("Copy all"));
  gtk_accessible_update_property (GTK_ACCESSIBLE (copy),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL, _("Copy all"),
                                  -1);
  g_signal_connect (copy, "clicked", G_CALLBACK (on_copy_log_clicked), overlay);

  GtkWidget *header = adw_header_bar_new ();
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), copy);

  GtkWidget *toolbar_view = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), header);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), overlay);

  AdwDialog *dialog = adw_dialog_new ();
  adw_dialog_set_title (dialog, _("Full log"));
  adw_dialog_set_content_width (dialog, 760);
  adw_dialog_set_content_height (dialog, 520);
  adw_dialog_set_child (dialog, toolbar_view);

  /* Mientras el diálogo exista, append_log hará que la vista siga el
   * final; el puntero débil se pone a NULL solo al cerrarlo. */
  log_view = view;
  g_object_add_weak_pointer (G_OBJECT (view), (gpointer *) &log_view);

  adw_dialog_present (dialog, GTK_WIDGET (main_window));
  gtk_text_view_scroll_to_mark (GTK_TEXT_VIEW (view),
                                gtk_text_buffer_get_mark (log_buffer, "end"),
                                0, FALSE, 0, 0);
}

/* El usuario (o nosotros) ha movido el scroll: ¿está abajo del todo? */
static void
on_activity_scrolled (GtkAdjustment *adjustment, gpointer user_data)
{
  (void) user_data;
  activity_follow = gtk_adjustment_get_value (adjustment) +
                    gtk_adjustment_get_page_size (adjustment) >=
                    gtk_adjustment_get_upper (adjustment) - 1;
}

static guint activity_scroll_id;   /* bajada pendiente (ver abajo) */

static gboolean
scroll_activity_to_bottom (gpointer user_data)
{
  GtkAdjustment *adjustment = user_data;

  activity_scroll_id = 0;
  gtk_adjustment_set_value (adjustment,
                            gtk_adjustment_get_upper (adjustment) -
                            gtk_adjustment_get_page_size (adjustment));
  return G_SOURCE_REMOVE;
}

/* Ha cambiado el tamaño del contenido (p. ej. un paso nuevo). Como en una
 * consola: si estabas abajo del todo, seguimos bajando; si habías subido
 * a mirar algo, no te movemos.
 *
 * OJO: "changed" llega mientras GTK está colocando los widgets; si
 * movemos el scroll aquí mismo, el contenido ya se colocó con el valor
 * anterior y se queda desfasado. Por eso lo dejamos para justo después
 * (g_idle_add = "cuando el bucle de eventos esté libre"). */
static void
on_activity_resized (GtkAdjustment *adjustment, gpointer user_data)
{
  (void) user_data;
  if (activity_follow && activity_scroll_id == 0)
    activity_scroll_id = g_idle_add (scroll_activity_to_bottom, adjustment);
}

/* La tarjeta _("Activity"): los pasos + el botón del registro completo. */
static GtkWidget *
build_activity_group (void)
{
  activity_group = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (activity_group),
                                   _("Activity"));

  GtkWidget *show_log = gtk_button_new_with_label (_("Full log"));
  gtk_widget_add_css_class (show_log, "flat");
  gtk_widget_set_valign (show_log, GTK_ALIGN_CENTER);
  g_signal_connect (show_log, "clicked", G_CALLBACK (on_show_log_clicked),
                    NULL);
  adw_preferences_group_set_header_suffix (ADW_PREFERENCES_GROUP (activity_group),
                                           show_log);

  activity_list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (activity_list),
                                   GTK_SELECTION_NONE);
  gtk_list_box_set_show_separators (GTK_LIST_BOX (activity_list), TRUE);

  /* Lo que se ve cuando la lista está vacía. */
  GtkWidget *placeholder = gtk_label_new (_("No activity yet"));
  gtk_widget_add_css_class (placeholder, "dim-label");
  gtk_widget_set_margin_top (placeholder, 18);
  gtk_widget_set_margin_bottom (placeholder, 18);
  gtk_list_box_set_placeholder (GTK_LIST_BOX (activity_list), placeholder);

  /* Pasos que hubiera de antes de crear la ventana. */
  for (guint i = 0; i < steps->len; i++)
    step_build_row (g_ptr_array_index (steps, i));

  /* La lista va dentro de una zona con scroll de altura fija: la ventana
   * no crece, y para ver pasos anteriores se sube, como en una consola.
   * "card" le da el borde redondeado; overflow HIDDEN recorta lo que
   * asome por las esquinas al hacer scroll. */
  GtkWidget *scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height (GTK_SCROLLED_WINDOW (scroller),
                                              240);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), activity_list);
  gtk_widget_add_css_class (scroller, "card");
  gtk_widget_set_overflow (scroller, GTK_OVERFLOW_HIDDEN);

  /* La "adjustment" es el modelo del scroll: posición (value), tamaño de
   * lo visible (page-size) y tamaño total (upper). */
  GtkAdjustment *adjustment =
    gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (scroller));
  g_signal_connect (adjustment, "value-changed",
                    G_CALLBACK (on_activity_scrolled), NULL);
  g_signal_connect (adjustment, "changed",
                    G_CALLBACK (on_activity_resized), NULL);

  adw_preferences_group_add (ADW_PREFERENCES_GROUP (activity_group), scroller);
  refresh_activity ();
  return activity_group;
}

/* La lista de VPN + la actividad. */
static GtkWidget *
build_list_page (void)
{
  vpn_group = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (vpn_group),
                                   _("Connections"));
  for (guint i = 0; i < rows->len; i++)
    vpn_row_build (g_ptr_array_index (rows, i));

  /* AdwPreferencesPage ya trae scroll y márgenes bonitos. */
  GtkWidget *page = adw_preferences_page_new ();
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            ADW_PREFERENCES_GROUP (vpn_group));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            ADW_PREFERENCES_GROUP (build_activity_group ()));
  return page;
}

/* ---------------------------------------------------------------- */
/* Aviso la primera vez que se cierra la ventana                    */
/* ---------------------------------------------------------------- */

static gboolean close_hint_open;   /* el aviso está a la vista */

static void
on_close_hint_response (AdwAlertDialog *alert, const char *response,
                        gpointer user_data)
{
  (void) alert; (void) user_data;

  close_hint_open = FALSE;
  app_settings_set_bool ("close-hint-shown", TRUE);   /* no volver a avisar */

  if (g_str_equal (response, "quit"))
    request_quit ();
  else
    gtk_widget_set_visible (GTK_WIDGET (main_window), FALSE);
}

static void
show_close_hint (void)
{
  if (close_hint_open)
    return;   /* ya está a la vista (p. ej. pulsaste la X dos veces) */

  AdwDialog *alert = adw_alert_dialog_new (
    _("VPN Portal keeps running"),
    _("When you close the window, the app stays in the top bar so you can "
      "connect and disconnect from its icon. To close it completely, use "
      "“Quit” in that menu or press Ctrl+Q."));
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (alert),
                                  "quit", _("Quit completely"),
                                  "hide", _("Got it"),
                                  NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (alert), "hide",
                                            ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (alert), "hide");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (alert), "hide");
  g_signal_connect (alert, "response",
                    G_CALLBACK (on_close_hint_response), NULL);
  close_hint_open = TRUE;
  adw_dialog_present (alert, GTK_WIDGET (main_window));
}

/* Al pulsar la X de la ventana. Devolver TRUE significa "ya me encargo
 * yo, no la destruyas": la ocultamos y la app sigue en la barra. */
static gboolean
on_close_request (GtkWindow *window, gpointer user_data)
{
  (void) user_data;

  if (tray_is_available ()) {
    if (!app_settings_get_bool ("close-hint-shown"))
      show_close_hint ();   /* la primera vez, explicamos qué pasa */
    else
      gtk_widget_set_visible (GTK_WIDGET (window), FALSE);
    return TRUE;
  }

  /* Sin icono en la barra no habría forma de volver: salimos. */
  request_quit ();
  return TRUE;
}

/* ---------------------------------------------------------------- */
/* Preferencias (temas)                                             */
/* ---------------------------------------------------------------- */

static void show_toast (const char *message);

static AdwDialog *prefs_dialog;   /* Preferencias, si está abierto */
static GtkWidget *themes_group;   /* su grupo "Tema" */
static GPtrArray *theme_rows;     /* las filas de ese grupo (sin "ref") */

/* Un aviso breve: dentro de Preferencias si está abierto (si no, quedaría
 * tapado por el diálogo); si no, en la ventana. */
static void
prefs_toast (const char *message)
{
  if (prefs_dialog != NULL)
    adw_preferences_dialog_add_toast (ADW_PREFERENCES_DIALOG (prefs_dialog),
                                      adw_toast_new (message));
  else
    show_toast (message);
}

/* Errores en el CSS de un tema propio (nos avisa theme.c). */
static void
on_theme_problem (const char *message, gpointer user_data)
{
  (void) user_data;
  append_log (_("Theme"), message);
  prefs_toast (_("The theme has CSS errors (details in the log)"));
}

/* La acción "app.theme": su ESTADO es el id del tema actual ("aero"...).
 * Pedir otro estado = cambiar de tema. Los botones de radio de
 * Preferencias están enganchados a ella: GTK marca solo el que coincide
 * con el estado, y al pulsar uno pide el cambio. */
static void
on_theme_change_state (GSimpleAction *action, GVariant *value,
                       gpointer user_data)
{
  (void) user_data;
  theme_set (g_variant_get_string (value, NULL));
  g_simple_action_set_state (action, g_variant_new_string (theme_get_current ()));
}

/* Cambiar de tema "desde el código" (y que los radios se enteren). */
static void
select_theme (const char *id)
{
  g_action_group_change_action_state (G_ACTION_GROUP (g_application_get_default ()),
                                      "theme", g_variant_new_string (id));
}

static void fill_theme_rows (void);

static void
on_remove_theme_confirmed (AdwAlertDialog *alert, const char *response,
                           gpointer user_data)
{
  (void) alert; (void) response;
  const char *id = user_data;
  g_autoptr (GError) error = NULL;

  if (!theme_remove (id, &error)) {
    prefs_toast (error->message);
    return;
  }
  select_theme (theme_get_current ());   /* por si era el actual */
  fill_theme_rows ();
  prefs_toast (_("Theme removed"));
}

static void
on_remove_theme_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  const ThemeInfo *theme = user_data;

  AdwDialog *alert = adw_alert_dialog_new (NULL, NULL);
  adw_alert_dialog_format_heading (ADW_ALERT_DIALOG (alert),
                                   _("Remove the theme “%s”?"), theme->name);
  adw_alert_dialog_format_body (ADW_ALERT_DIALOG (alert),
                                _("Its file will be deleted from the themes "
                                  "folder."));
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (alert),
                                  "cancel", _("Cancel"),
                                  "remove", _("Remove"),
                                  NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (alert), "remove",
                                            ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (alert), "cancel");
  /* El id se copia y se libera con el diálogo: la lista de temas puede
   * rehacerse mientras tanto. */
  char *id = g_strdup (theme->id);
  g_object_set_data_full (G_OBJECT (alert), "theme-id", id, g_free);
  g_signal_connect (alert, "response::remove",
                    G_CALLBACK (on_remove_theme_confirmed), id);
  adw_dialog_present (alert, GTK_WIDGET (prefs_dialog));
}

/* (Re)hace las filas del grupo "Tema": los de serie y los propios. */
static void
fill_theme_rows (void)
{
  if (themes_group == NULL)
    return;

  for (guint i = 0; i < theme_rows->len; i++)
    adw_preferences_group_remove (ADW_PREFERENCES_GROUP (themes_group),
                                  g_ptr_array_index (theme_rows, i));
  g_ptr_array_set_size (theme_rows, 0);

  theme_reload_user_themes ();   /* por si has copiado alguno a mano */
  GPtrArray *themes = theme_list ();
  GtkWidget *first_check = NULL;

  for (guint i = 0; i < themes->len; i++) {
    const ThemeInfo *theme = g_ptr_array_index (themes, i);

    GtkWidget *row = adw_action_row_new ();
    adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), theme->name);
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), theme->description);

    /* La muestra de color: una caja vacía pintada por CSS con las clases
     * "theme-swatch" y la propia del tema. */
    GtkWidget *swatch = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class (swatch, "theme-swatch");
    gtk_widget_add_css_class (swatch, theme->swatch_class);
    gtk_widget_set_valign (swatch, GTK_ALIGN_CENTER);
    adw_action_row_add_prefix (ADW_ACTION_ROW (row), swatch);

    /* Los temas propios llevan una papelera para quitarlos. */
    if (theme->file != NULL) {
      GtkWidget *remove = gtk_button_new_from_icon_name ("user-trash-symbolic");
      gtk_widget_add_css_class (remove, "flat");
      gtk_widget_set_valign (remove, GTK_ALIGN_CENTER);
      gtk_widget_set_tooltip_text (remove, _("Remove theme"));
      gtk_accessible_update_property (GTK_ACCESSIBLE (remove),
                                      GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      _("Remove theme"), -1);
      g_signal_connect (remove, "clicked",
                        G_CALLBACK (on_remove_theme_clicked), (gpointer) theme);
      adw_action_row_add_suffix (ADW_ACTION_ROW (row), remove);
    }

    /* Botón de radio enganchado a "app.theme" con su id como "target":
     * se ve marcado si el estado de la acción es ese id. Van en un mismo
     * grupo para que se dibujen como radios (círculo) y no como casillas. */
    GtkWidget *check = gtk_check_button_new ();
    gtk_widget_set_valign (check, GTK_ALIGN_CENTER);
    gtk_actionable_set_action_name (GTK_ACTIONABLE (check), "app.theme");
    gtk_actionable_set_action_target_value (GTK_ACTIONABLE (check),
                                            g_variant_new_string (theme->id));
    if (first_check != NULL)
      gtk_check_button_set_group (GTK_CHECK_BUTTON (check),
                                  GTK_CHECK_BUTTON (first_check));
    else
      first_check = check;
    adw_action_row_add_suffix (ADW_ACTION_ROW (row), check);

    /* Pulsar en cualquier parte de la fila = pulsar el botón de radio. */
    adw_action_row_set_activatable_widget (ADW_ACTION_ROW (row), check);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (themes_group), row);
    g_ptr_array_add (theme_rows, row);
  }
}

/* Elegiste un .css en el selector de ficheros. */
static void
on_theme_file_chosen (GObject *source, GAsyncResult *result,
                      gpointer user_data)
{
  (void) user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source),
                                                        result, &error);
  if (file == NULL)
    return;   /* cancelado */

  g_autofree char *id = theme_install (file, &error);
  if (id == NULL) {
    prefs_toast (error->message);
    return;
  }
  fill_theme_rows ();
  select_theme (id);
  prefs_toast (_("Theme added"));
}

static void
on_add_theme_clicked (GtkButton *button, gpointer user_data)
{
  (void) button; (void) user_data;

  /* El selector de ficheros de GNOME, mostrando solo los .css. */
  g_autoptr (GtkFileFilter) filter = gtk_file_filter_new ();
  gtk_file_filter_set_name (filter, _("Themes (.css)"));
  gtk_file_filter_add_suffix (filter, "css");
  g_autoptr (GListStore) filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
  g_list_store_append (filters, filter);

  g_autoptr (GtkFileDialog) chooser = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (chooser, _("Add theme"));
  gtk_file_dialog_set_filters (chooser, G_LIST_MODEL (filters));
  gtk_file_dialog_open (chooser, main_window, NULL, on_theme_file_chosen, NULL);
}

static void
on_open_themes_folder_clicked (GtkButton *button, gpointer user_data)
{
  (void) button; (void) user_data;
  g_autofree char *dir = theme_user_dir ();
  g_autoptr (GFile) folder = g_file_new_for_path (dir);

  /* GtkFileLauncher abre la carpeta con la app de ficheros de GNOME. */
  g_autoptr (GtkFileLauncher) launcher = gtk_file_launcher_new (folder);
  gtk_file_launcher_launch (launcher, main_window, NULL, NULL, NULL);
}

/* El grupo "Temas propios": explicación + botones. */
static GtkWidget *
build_user_themes_group (void)
{
  GtkWidget *group = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (group),
                                   _("Your themes"));
  adw_preferences_group_set_description (
    ADW_PREFERENCES_GROUP (group),
    _("A theme is a .css file. Add yours and it will show up above; if you "
      "edit the one in use, it reloads by itself when you save. "
      "<a href=\"https://github.com/GabRanalli/VPNPortal/blob/main/docs/THEMES.md\">"
      "How to make a theme</a>"));

  GtkWidget *folder = gtk_button_new_from_icon_name ("folder-open-symbolic");
  gtk_widget_add_css_class (folder, "flat");
  gtk_widget_set_tooltip_text (folder, _("Open the themes folder"));
  gtk_accessible_update_property (GTK_ACCESSIBLE (folder),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _("Open the themes folder"), -1);
  g_signal_connect (folder, "clicked",
                    G_CALLBACK (on_open_themes_folder_clicked), NULL);

  GtkWidget *add = gtk_button_new_with_label (_("Add theme…"));
  g_signal_connect (add, "clicked", G_CALLBACK (on_add_theme_clicked), NULL);

  GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_valign (buttons, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (buttons), folder);
  gtk_box_append (GTK_BOX (buttons), add);
  adw_preferences_group_set_header_suffix (ADW_PREFERENCES_GROUP (group),
                                           buttons);
  return group;
}

/* ---------------------------------------------------------------- */
/* Idioma y reiniciar                                               */
/* ---------------------------------------------------------------- */

/* Los idiomas del selector: "system" (el del sistema) y los traducidos.
 * Cada idioma se escribe en su propio idioma, como es costumbre. */
static const char * const language_ids[] = { "system", "en", "es" };

static void
restart_now (void)
{
  restarting = TRUE;
  request_quit ();   /* sale limpio (desconectando) y main() relanza */
}

static void
on_restart_confirmed (AdwAlertDialog *alert, const char *response,
                      gpointer user_data)
{
  (void) alert; (void) response; (void) user_data;
  restart_now ();
}

/* La acción "app.restart". Si hay una VPN en uso, avisamos antes:
 * reiniciar la desconecta. */
static void
on_restart_action (GSimpleAction *action, GVariant *parameter,
                   gpointer user_data)
{
  (void) action; (void) parameter; (void) user_data;
  VpnRow *active = find_active_row ();

  if (active == NULL) {
    restart_now ();
    return;
  }

  AdwDialog *alert = adw_alert_dialog_new (_("Restart now?"), NULL);
  adw_alert_dialog_format_body (ADW_ALERT_DIALOG (alert),
                                _("Restarting will disconnect %s."),
                                active->config->name);
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (alert),
                                  "cancel", _("Cancel"),
                                  "restart", _("Restart"),
                                  NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (alert),
                                            "restart",
                                            ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (alert), "cancel");
  g_signal_connect (alert, "response::restart",
                    G_CALLBACK (on_restart_confirmed), NULL);
  adw_dialog_present (alert, prefs_dialog != NULL
                             ? GTK_WIDGET (prefs_dialog)
                             : GTK_WIDGET (main_window));
}

/* Cambiaste el idioma en el desplegable: se guarda, y se ofrece
 * reiniciar (los textos ya pintados no se pueden cambiar en caliente). */
static void
on_language_selected (AdwComboRow *row, GParamSpec *pspec, gpointer user_data)
{
  (void) pspec; (void) user_data;
  guint selected = adw_combo_row_get_selected (row);
  if (selected >= G_N_ELEMENTS (language_ids))
    return;

  app_settings_set_string ("language", language_ids[selected]);

  AdwToast *toast = adw_toast_new (_("The language will change when the app "
                                     "restarts"));
  adw_toast_set_button_label (toast, _("Restart"));
  adw_toast_set_action_name (toast, "app.restart");
  adw_toast_set_timeout (toast, 0);   /* hasta que lo cierres */
  adw_preferences_dialog_add_toast (ADW_PREFERENCES_DIALOG (prefs_dialog),
                                    toast);
}

static GtkWidget *
build_language_group (void)
{
  GtkWidget *group = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (group),
                                   _("Language"));

  /* AdwComboRow: una fila con desplegable. Las opciones van en un
   * GtkStringList (una lista de textos). */
  g_autoptr (GtkStringList) options = gtk_string_list_new (NULL);
  gtk_string_list_append (options, _("Automatic (system language)"));
  gtk_string_list_append (options, "English");
  gtk_string_list_append (options, "Español");

  GtkWidget *row = adw_combo_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), _("Language"));
  adw_combo_row_set_model (ADW_COMBO_ROW (row), G_LIST_MODEL (options));

  g_autofree char *saved = app_settings_get_string ("language");
  for (guint i = 0; i < G_N_ELEMENTS (language_ids); i++)
    if (g_strcmp0 (saved, language_ids[i]) == 0)
      adw_combo_row_set_selected (ADW_COMBO_ROW (row), i);

  /* "notify::selected" = "ha cambiado la propiedad selected". Se conecta
   * después de poner el valor guardado, para no dispararla al abrir. */
  g_signal_connect (row, "notify::selected",
                    G_CALLBACK (on_language_selected), NULL);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), row);
  return group;
}

static void
on_preferences_action (GSimpleAction *action, GVariant *parameter,
                       gpointer user_data)
{
  (void) action; (void) parameter; (void) user_data;

  if (prefs_dialog != NULL)
    return;   /* ya está abierto */

  themes_group = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (themes_group),
                                   _("Theme"));
  adw_preferences_group_set_description (ADW_PREFERENCES_GROUP (themes_group),
                                         _("Applied right away."));
  g_object_add_weak_pointer (G_OBJECT (themes_group),
                             (gpointer *) &themes_group);
  if (theme_rows == NULL)
    theme_rows = g_ptr_array_new ();
  g_ptr_array_set_size (theme_rows, 0);
  fill_theme_rows ();

  GtkWidget *page = adw_preferences_page_new ();
  adw_preferences_page_set_title (ADW_PREFERENCES_PAGE (page), _("Appearance"));
  adw_preferences_page_set_icon_name (ADW_PREFERENCES_PAGE (page),
                                      "applications-graphics-symbolic");
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            ADW_PREFERENCES_GROUP (themes_group));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            ADW_PREFERENCES_GROUP (build_user_themes_group ()));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            ADW_PREFERENCES_GROUP (build_language_group ()));

  prefs_dialog = adw_preferences_dialog_new ();
  g_object_add_weak_pointer (G_OBJECT (prefs_dialog),
                             (gpointer *) &prefs_dialog);
  adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (prefs_dialog),
                              ADW_PREFERENCES_PAGE (page));
  adw_dialog_present (prefs_dialog, GTK_WIDGET (main_window));
}

/* ---------------------------------------------------------------- */
/* Olvidar las sesiones guardadas del login                         */
/* ---------------------------------------------------------------- */

/* Un "toast": el mensajito que aparece abajo unos segundos. */
static void
show_toast (const char *message)
{
  if (toast_overlay != NULL)
    adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (toast_overlay),
                                 adw_toast_new (message));
}

static void
on_sessions_forgotten (GObject *source, GAsyncResult *result,
                       gpointer user_data)
{
  (void) source; (void) user_data;
  g_autoptr (GError) error = NULL;

  if (!login_forget_sessions_finish (result, &error)) {
    append_log ("App", error->message);
    show_toast (_("Could not forget the sessions"));
    return;
  }
  append_log ("App", _("» Saved sign-in sessions forgotten"));
  show_toast (_("Sessions forgotten"));
}

static void
on_forget_confirmed (AdwAlertDialog *alert, const char *response,
                     gpointer user_data)
{
  (void) alert; (void) response; (void) user_data;
  login_forget_sessions (on_sessions_forgotten, NULL);
}

static void
on_forget_sessions_action (GSimpleAction *action, GVariant *parameter,
                           gpointer user_data)
{
  (void) action; (void) parameter; (void) user_data;

  /* Borrar las cookies a mitad de un login lo rompería. */
  if (login_dialog != NULL) {
    show_toast (_("Finish or cancel the sign-in first"));
    return;
  }

  AdwDialog *alert = adw_alert_dialog_new (
    _("Forget saved sessions?"),
    _("Next time you connect you will have to sign in again (and enter "
      "the code from your phone, if you use one) on every VPN. Your VPNs "
      "are not deleted."));
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (alert),
                                  "cancel", _("Cancel"),
                                  "forget", _("Forget"),
                                  NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (alert), "forget",
                                            ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (alert), "cancel");
  g_signal_connect (alert, "response::forget",
                    G_CALLBACK (on_forget_confirmed), NULL);
  adw_dialog_present (alert, GTK_WIDGET (main_window));
}

/* ---------------------------------------------------------------- */
/* Arrancar al iniciar sesión                                       */
/* ---------------------------------------------------------------- */

/*
 * Los escritorios de Linux lanzan al iniciar sesión cada fichero .desktop
 * que haya en ~/.config/autostart. Activar la opción = crear el nuestro;
 * desactivarla = borrarlo. No hace falta nada más.
 */
static char *
autostart_path (void)
{
  g_autofree char *name = g_strconcat (VPNPORTAL_APP_ID, ".desktop", NULL);
  return g_build_filename (g_get_user_config_dir (), "autostart", name, NULL);
}

/* En la línea Exec de un .desktop, una ruta con espacios va entre
 * comillas dobles, y dentro se escapan  " ` $ \  con una barra. */
static char *
quote_exec_arg (const char *arg)
{
  GString *quoted = g_string_new ("\"");
  for (const char *p = arg; *p != '\0'; p++) {
    if (strchr ("\"`$\\", *p) != NULL)
      g_string_append_c (quoted, '\\');
    g_string_append_c (quoted, *p);
  }
  g_string_append_c (quoted, '"');
  return g_string_free (quoted, FALSE);
}

static gboolean
autostart_write (GError **error)
{
  /* /proc/self/exe apunta al ejecutable que estamos corriendo ahora. */
  g_autofree char *exe = g_file_read_link ("/proc/self/exe", error);
  if (exe == NULL)
    return FALSE;

  g_autofree char *quoted = quote_exec_arg (exe);
  g_autofree char *exec = g_strdup_printf ("%s --background", quoted);

  g_autoptr (GKeyFile) desktop = g_key_file_new ();
  const char *group = G_KEY_FILE_DESKTOP_GROUP;
  g_key_file_set_string (desktop, group, G_KEY_FILE_DESKTOP_KEY_TYPE,
                         "Application");
  g_key_file_set_string (desktop, group, G_KEY_FILE_DESKTOP_KEY_NAME,
                         "VPN Portal");
  g_key_file_set_string (desktop, group, G_KEY_FILE_DESKTOP_KEY_COMMENT,
                         "GlobalProtect VPN manager");
  g_key_file_set_string (desktop, group, G_KEY_FILE_DESKTOP_KEY_ICON,
                         "network-vpn");
  g_key_file_set_string (desktop, group, G_KEY_FILE_DESKTOP_KEY_EXEC, exec);
  g_key_file_set_boolean (desktop, group, "X-GNOME-Autostart-enabled", TRUE);

  g_autofree char *path = autostart_path ();
  g_autofree char *dir = g_path_get_dirname (path);
  g_mkdir_with_parents (dir, 0700);
  return g_key_file_save_to_file (desktop, path, error);
}

/* La casilla del menú. Es una acción "con estado" (TRUE/FALSE): GTK la
 * pinta como casilla y, al pulsarla, nos pide cambiar el estado. Solo lo
 * cambiamos si de verdad se ha podido crear/borrar el fichero. */
static void
on_autostart_change_state (GSimpleAction *action, GVariant *value,
                           gpointer user_data)
{
  (void) user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *path = autostart_path ();

  if (g_variant_get_boolean (value)) {
    if (!autostart_write (&error)) {
      append_log ("App", error->message);
      return;
    }
  } else if (g_unlink (path) != 0 && errno != ENOENT) {
    append_log ("App", g_strerror (errno));
    return;
  }
  g_simple_action_set_state (action, value);
}

/* ---------------------------------------------------------------- */
/* Arrancar oculta (--background)                                   */
/* ---------------------------------------------------------------- */

static gboolean start_hidden;   /* nos lanzaron con --background */

/* Se ejecuta antes de nada con las opciones de la línea de órdenes. */
static int
on_handle_local_options (GApplication *app, GVariantDict *options,
                         gpointer user_data)
{
  (void) app; (void) user_data;
  if (g_variant_dict_contains (options, "background"))
    start_hidden = TRUE;
  return -1;   /* -1 = "sigue arrancando con normalidad" */
}

/* Red de seguridad: si arrancamos ocultos pero nadie pinta iconos en la
 * barra (p. ej. sin la extensión), enseñamos la ventana: si no, la app
 * estaría en marcha sin forma de llegar a ella. */
static gboolean
show_window_if_no_tray (gpointer user_data)
{
  (void) user_data;
  if (main_window != NULL && !tray_is_available ())
    gtk_window_present (main_window);
  return G_SOURCE_REMOVE;
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
  /* Una clase CSS propia, para que los temas puedan decorar justo esta
   * ventana (p. ej. el cielo de Frutiger Aero). */
  gtk_widget_add_css_class (window, "vpnportal-main");
  gtk_window_set_title (main_window, "VPN Portal");
  gtk_window_set_default_size (main_window, 520, 600);
  g_signal_connect (window, "close-request",
                    G_CALLBACK (on_close_request), app);

  /* Botón "+" en la barra de título. */
  GtkWidget *header = adw_header_bar_new ();
  GtkWidget *add = gtk_button_new_from_icon_name ("list-add-symbolic");
  gtk_widget_set_tooltip_text (add, _("Add VPN"));
  g_signal_connect (add, "clicked", G_CALLBACK (on_add_clicked), NULL);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), add);

  /* El menú principal (☰). Cada entrada apunta a una acción "app.…";
   * "app.autostart" tiene estado TRUE/FALSE y por eso sale como casilla. */
  g_autoptr (GMenu) menu = g_menu_new ();
  g_autoptr (GMenu) prefs_section = g_menu_new ();
  g_menu_append (prefs_section, _("Preferences"), "app.preferences");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (prefs_section));
  g_autoptr (GMenu) options_section = g_menu_new ();
  g_menu_append (options_section, _("Start when I log in"),
                 "app.autostart");
  g_menu_append (options_section, _("Forget saved sessions…"),
                 "app.forget-sessions");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (options_section));
  g_autoptr (GMenu) quit_section = g_menu_new ();
  g_menu_append (quit_section, _("Quit"), "app.quit");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (quit_section));

  GtkWidget *menu_button = gtk_menu_button_new ();
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (menu_button),
                                 "open-menu-symbolic");
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (menu_button),
                                  G_MENU_MODEL (menu));
  gtk_menu_button_set_primary (GTK_MENU_BUTTON (menu_button), TRUE);  /* F10 */
  gtk_widget_set_tooltip_text (menu_button, _("Main menu"));
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), menu_button);

  /* GtkStack: varias páginas apiladas, se ve una cada vez. */
  stack = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (stack),
                                 GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_add_named (GTK_STACK (stack), build_empty_page (), "empty");
  gtk_stack_add_named (GTK_STACK (stack), build_list_page (), "list");

  GtkWidget *toolbar_view = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), header);
  /* El contenido va dentro de un AdwToastOverlay: así los "toasts" (avisos
   * breves) pueden salir encima de lo que haya. */
  toast_overlay = adw_toast_overlay_new ();
  adw_toast_overlay_set_child (ADW_TOAST_OVERLAY (toast_overlay), stack);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view),
                                toast_overlay);
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (window),
                                      toolbar_view);

  if (startup_error != NULL)
    append_log ("App", startup_error);

  refresh_rows ();

  /* Con --background (al iniciar sesión) la ventana se crea pero no se
   * enseña: la app queda solo en la barra. */
  if (start_hidden) {
    start_hidden = FALSE;   /* solo la primera vez */
    g_timeout_add_seconds (10, show_window_if_no_tray, NULL);
    return;
  }
  gtk_window_present (main_window);
}

/* ---------------------------------------------------------------- */
/* Lo que se pide desde el menú de la barra                         */
/* ---------------------------------------------------------------- */

static void
on_tray_toggle (const char *id, const char *activation_token,
                gpointer user_data)
{
  (void) user_data;
  for (guint i = 0; i < rows->len; i++) {
    VpnRow *r = g_ptr_array_index (rows, i);
    if (!g_str_equal (r->config->id, id))
      continue;

    /* Por si hay que enseñar la ventana para preguntar (ver on_tray_show). */
    if (main_window != NULL && activation_token != NULL)
      gtk_window_set_startup_id (main_window, activation_token);

    if (vpn_get_state (r->vpn) == VPN_DISCONNECTED)
      request_connect (r);
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
  steps = g_ptr_array_new_with_free_func ((GDestroyNotify) step_free);

  /* El texto del registro completo vive toda la app (aunque la ventana o
   * el diálogo no existan), con un "mark" al final para poder seguirlo. */
  log_buffer = gtk_text_buffer_new (NULL);
  GtkTextIter end;
  gtk_text_buffer_get_end_iter (log_buffer, &end);
  gtk_text_buffer_create_mark (log_buffer, "end", &end, FALSE);

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
    { .name = "forget-sessions", .activate = on_forget_sessions_action },
    { .name = "preferences", .activate = on_preferences_action },
    { .name = "restart", .activate = on_restart_action },
    /* Con parámetro y estado de texto ("s") y sin .activate: activarla
     * con un id llama a .change_state con ese id. */
    { .name = "theme", .parameter_type = "s", .state = "'system'",
      .change_state = on_theme_change_state },
    /* Sin .activate y con estado sí/no: GLib la convierte en un
     * interruptor que llama a .change_state con el valor contrario. */
    { .name = "autostart", .state = "false",
      .change_state = on_autostart_change_state },
  };
  g_action_map_add_action_entries (G_ACTION_MAP (app), app_actions,
                                   G_N_ELEMENTS (app_actions), app);

  /* La casilla refleja si el fichero de autoarranque existe. Si existe,
   * lo reescribimos para que apunte al ejecutable actual (por si lo has
   * movido o recompilado en otra carpeta). */
  g_autofree char *autostart = autostart_path ();
  gboolean autostart_on = g_file_test (autostart, G_FILE_TEST_EXISTS);
  if (autostart_on)
    autostart_write (NULL);
  g_simple_action_set_state (
    G_SIMPLE_ACTION (g_action_map_lookup_action (G_ACTION_MAP (app),
                                                 "autostart")),
    g_variant_new_boolean (autostart_on));
  gtk_application_set_accels_for_action (GTK_APPLICATION (app), "app.quit",
                                         (const char *[]) { "<Control>q",
                                                            NULL });
  gtk_application_set_accels_for_action (GTK_APPLICATION (app),
                                         "app.preferences",
                                         (const char *[]) { "<Control>comma",
                                                            NULL });

  /* El tema guardado (o "Sistema"), y la acción reflejándolo. */
  theme_init (on_theme_problem, NULL);
  g_simple_action_set_state (
    G_SIMPLE_ACTION (g_action_map_lookup_action (G_ACTION_MAP (app), "theme")),
    g_variant_new_string (theme_get_current ()));

  g_autoptr (GPtrArray) configs = vpn_config_load (&error);
  if (error != NULL)
    startup_error = g_strdup_printf (_("Could not read the configuration: %s"),
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
    startup_error = g_strdup (_("No session bus: signing in will not work"));
  else if (!auth_service_start (bus, on_auth_request, NULL, &error))
    startup_error = g_strdup_printf (_("Could not publish the sign-in service: %s"),
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
  activity_group = NULL;
  activity_list = NULL;
  toast_overlay = NULL;
  g_clear_handle_id (&activity_scroll_id, g_source_remove);
  auth_service_stop ();
  tray_shutdown ();
  g_clear_pointer (&rows, g_ptr_array_unref);
  g_clear_pointer (&steps, g_ptr_array_unref);
  steps_vpn = NULL;
  g_clear_object (&log_buffer);
  g_clear_pointer (&startup_error, g_free);
}

/*
 * Traducciones con gettext. En el código los textos están en inglés,
 * envueltos en _("..."); gettext busca su traducción en los ficheros .mo
 * (compilados desde po/es.po) del idioma que toque.
 */
static void
setup_i18n (void)
{
  /* El idioma elegido en Preferencias manda sobre el del sistema.
   * LANGUAGE es la variable que gettext mira antes que ninguna. */
  g_autofree char *language = app_settings_get_string ("language");
  if (language != NULL && g_strcmp0 (language, "system") != 0)
    g_setenv ("LANGUAGE", language, TRUE);

  /* "Usa el idioma y formatos del sistema" (sin esto, todo en inglés). */
  setlocale (LC_ALL, "");

  /* ¿Dónde están los .mo? Si ejecutamos desde la carpeta de compilación
   * (build/), Meson los deja al lado, en build/po. Si la app está
   * instalada, en LOCALEDIR. */
  g_autofree char *exe = g_file_read_link ("/proc/self/exe", NULL);
  g_autofree char *exe_dir = exe != NULL ? g_path_get_dirname (exe) : NULL;
  g_autofree char *local = exe_dir != NULL
    ? g_build_filename (exe_dir, "po", NULL) : NULL;
  const char *dir = local != NULL && g_file_test (local, G_FILE_TEST_IS_DIR)
    ? local : LOCALEDIR;

  bindtextdomain (GETTEXT_PACKAGE, dir);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);
}

int
main (int argc, char *argv[])
{
  setup_i18n ();   /* lo primero: antes de crear ningún texto */

  AdwApplication *app = adw_application_new (VPNPORTAL_APP_ID,
                                             G_APPLICATION_DEFAULT_FLAGS);

  /* Dónde buscar los recursos (style.css...). Por defecto se deduce del id
   * de la app; lo fijamos para que la versión demo (otro id) los encuentre. */
  g_application_set_resource_base_path (G_APPLICATION (app),
                                        "/io/github/GabRanalli/VPNPortal");

  /* La opción --background (y su explicación en --help). */
  g_application_add_main_option (G_APPLICATION (app), "background", 'b',
                                 G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,
                                 _("Start hidden, with just the top bar "
                                   "icon"), NULL);
  g_signal_connect (app, "handle-local-options",
                    G_CALLBACK (on_handle_local_options), NULL);
  g_signal_connect (app, "startup",  G_CALLBACK (on_startup),  NULL);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  g_signal_connect (app, "shutdown", G_CALLBACK (on_shutdown), NULL);

  int status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);

  /* "Reiniciar" (p. ej. al cambiar de idioma): la app ya se ha cerrado
   * del todo (VPN desconectada incluida); execv sustituye este proceso por
   * uno nuevo del mismo programa, sin --background para que se vea. */
  if (restarting) {
    g_autofree char *exe = g_file_read_link ("/proc/self/exe", NULL);
    if (exe != NULL)
      execl (exe, exe, (char *) NULL);
  }
  return status;
}
