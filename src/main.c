/*
 * gp-vpn — fase 2: dos VPN con botón de conectar/desconectar y registro.
 *
 * Recordatorio de la fase 1: una app GTK vive en un BUCLE DE EVENTOS.
 * main() prepara la app y le cede el control; a partir de ahí GTK llama
 * a nuestras funciones (CALLBACKS) cuando ocurre algo (SEÑALES).
 *
 * Reparto del trabajo:
 *   - vpn.c  sabe lanzar/parar gpclient y nos avisa de lo que pasa.
 *   - main.c (este) solo pinta la ventana y reacciona a esos avisos.
 */

#include <adwaita.h>

#include "vpn.h"

/* Las VPN que conocemos. El 'id' debe coincidir con el del helper
 * (system/gp-vpn-helper), que es quien sabe los argumentos reales. */
static const VpnInfo vpn_infos[] = {
  { "empresa", "Empresa", "vpn.empresa.example" },
  { "uni",     "Uni",     "vpn.uni.example" },
};
/* Truco clásico de C: tamaño del array / tamaño de un elemento. */
#define N_VPNS G_N_ELEMENTS (vpn_infos)

/* Lo que necesitamos recordar de cada fila de la ventana. */
typedef struct {
  Vpn       *vpn;
  GtkWidget *row;       /* AdwActionRow: título + subtítulo (estado) */
  GtkWidget *spinner;   /* ruedecita mientras conecta/desconecta */
  GtkWidget *button;    /* Conectar / Desconectar */
} VpnRow;

/*
 * Variables globales (static = solo visibles en este fichero). En una app
 * pequeña con una sola ventana es lo más sencillo. Si creciera, las
 * meteríamos en un struct propio y lo pasaríamos como user_data.
 */
static VpnRow         rows[N_VPNS];
static GtkTextBuffer *log_buffer;   /* el texto del registro */
static GtkWidget     *log_view;

/* ---------------------------------------------------------------- */
/* Actualizar la interfaz                                           */
/* ---------------------------------------------------------------- */

/* Pone cada fila de acuerdo con el estado de su VPN. Se llama cada vez
 * que CUALQUIER VPN cambia, porque una ocupada bloquea a la otra. */
static void
refresh_rows (void)
{
  if (log_view == NULL)
    return;   /* la ventana aún no existe */

  /* gpclient solo permite una conexión a la vez: si alguna está en uso,
   * las demás no pueden conectar. */
  gboolean any_busy = FALSE;
  for (guint i = 0; i < N_VPNS; i++)
    if (vpn_get_state (rows[i].vpn) != VPN_DISCONNECTED)
      any_busy = TRUE;

  for (guint i = 0; i < N_VPNS; i++) {
    VpnRow *r = &rows[i];
    VpnState state = vpn_get_state (r->vpn);
    GtkButton *button = GTK_BUTTON (r->button);

    adw_action_row_set_subtitle (ADW_ACTION_ROW (r->row),
                                 vpn_state_to_string (state));
    gtk_widget_set_visible (r->spinner, state == VPN_CONNECTING ||
                                        state == VPN_DISCONNECTING);

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

static void
on_vpn_state (Vpn *vpn, VpnState state, gpointer user_data)
{
  (void) vpn; (void) state; (void) user_data;
  refresh_rows ();
}

/* Añade "[Empresa] línea" al final del registro y baja hasta ella. */
static void
on_vpn_line (Vpn *vpn, const char *line, gpointer user_data)
{
  (void) user_data;

  if (log_buffer == NULL)
    return;

  GtkTextIter end;
  gtk_text_buffer_get_end_iter (log_buffer, &end);
  g_autofree char *text = g_strdup_printf ("[%s] %s\n",
                                           vpn_get_info (vpn)->title, line);
  gtk_text_buffer_insert (log_buffer, &end, text, -1);

  /* Un "mark" es una posición del texto que se mantiene aunque el texto
   * cambie. Usamos uno al final para que la vista lo siga. */
  GtkTextMark *mark = gtk_text_buffer_get_mark (log_buffer, "end");
  gtk_text_view_scroll_mark_onscreen (GTK_TEXT_VIEW (log_view), mark);
}

/* ---------------------------------------------------------------- */
/* Construir la ventana                                             */
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

static GtkWidget *
build_vpn_group (void)
{
  /* AdwPreferencesGroup: una "tarjeta" con título y filas dentro. */
  GtkWidget *group = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (group),
                                   "Conexiones");

  for (guint i = 0; i < N_VPNS; i++) {
    VpnRow *r = &rows[i];
    const VpnInfo *info = vpn_get_info (r->vpn);

    r->row = adw_action_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (r->row), info->title);
    adw_action_row_add_prefix (ADW_ACTION_ROW (r->row),
                               gtk_image_new_from_icon_name ("network-vpn-symbolic"));
    gtk_widget_set_tooltip_text (r->row, info->subtitle);

    r->spinner = adw_spinner_new ();
    adw_action_row_add_suffix (ADW_ACTION_ROW (r->row), r->spinner);

    r->button = gtk_button_new_with_label ("Conectar");
    gtk_widget_set_valign (r->button, GTK_ALIGN_CENTER);
    g_signal_connect (r->button, "clicked",
                      G_CALLBACK (on_button_clicked), r);
    adw_action_row_add_suffix (ADW_ACTION_ROW (r->row), r->button);

    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), r->row);
  }
  return group;
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

static void
on_activate (GtkApplication *app, gpointer user_data)
{
  (void) user_data;

  GtkWindow *existing = gtk_application_get_active_window (app);
  if (existing != NULL) {
    gtk_window_present (existing);
    return;
  }

  GtkWidget *window = adw_application_window_new (app);
  gtk_window_set_title (GTK_WINDOW (window), "GP VPN");
  gtk_window_set_default_size (GTK_WINDOW (window), 520, 600);

  /* AdwPreferencesPage ya trae scroll y márgenes bonitos. */
  GtkWidget *page = adw_preferences_page_new ();
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            ADW_PREFERENCES_GROUP (build_vpn_group ()));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            ADW_PREFERENCES_GROUP (build_log_group ()));

  GtkWidget *toolbar_view = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view),
                                adw_header_bar_new ());
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), page);
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (window),
                                      toolbar_view);

  refresh_rows ();
  gtk_window_present (GTK_WINDOW (window));
}

/* ---------------------------------------------------------------- */
/* Arranque y cierre                                                */
/* ---------------------------------------------------------------- */

/*
 * "startup" se emite UNA vez al arrancar (antes de "activate"), y
 * "shutdown" una vez al terminar. Los objetos Vpn viven toda la vida de
 * la app, así que se crean y destruyen aquí, no con la ventana.
 */
static void
on_startup (GApplication *app, gpointer user_data)
{
  (void) app; (void) user_data;
  for (guint i = 0; i < N_VPNS; i++)
    rows[i].vpn = vpn_new (&vpn_infos[i], on_vpn_state, on_vpn_line, NULL);
}

static void
on_shutdown (GApplication *app, gpointer user_data)
{
  (void) app; (void) user_data;
  log_view = NULL;   /* la ventana ya no existe: que nadie la toque */
  log_buffer = NULL;
  for (guint i = 0; i < N_VPNS; i++)
    g_clear_pointer (&rows[i].vpn, vpn_free);
}

int
main (int argc, char *argv[])
{
  AdwApplication *app = adw_application_new ("es.gabran.GpVpn",
                                             G_APPLICATION_DEFAULT_FLAGS);

  g_signal_connect (app, "startup",  G_CALLBACK (on_startup),  NULL);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  g_signal_connect (app, "shutdown", G_CALLBACK (on_shutdown), NULL);

  int status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);
  return status;
}
