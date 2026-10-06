/*
 * gp-vpn — fase 1: el esqueleto mínimo de una app GTK4 + libadwaita.
 *
 * Idea clave: una app GTK NO es un programa que corre de arriba abajo.
 * main() crea un objeto "aplicación", le dice "cuando arranques, llama a
 * esta función mía" y le cede el control. A partir de ahí la app se queda
 * en un BUCLE DE EVENTOS (main loop) esperando cosas: clics, teclas,
 * temporizadores, datos de un proceso hijo... y por cada evento llama a
 * la función que hayamos "conectado" a él. Esas funciones se llaman
 * CALLBACKS, y en GTK se conectan a SEÑALES ("activate", "clicked"...).
 */

#include <adwaita.h>

/*
 * Callback de la señal "activate": GTK la emite cuando la app arranca
 * (y también si la lanzas otra vez estando ya abierta, que lo usaremos
 * más adelante para no abrir dos copias).
 *
 * Firma típica de un callback: (objeto que emite la señal, datos extra).
 * user_data es un puntero "comodín" que nosotros elegimos al conectar;
 * aquí no lo necesitamos.
 */
static void
on_activate (GtkApplication *app, gpointer user_data)
{
  (void) user_data;  /* evita el aviso de "parámetro sin usar" */

  /* Si ya hay ventana (segunda activación), la traemos al frente y listo. */
  GtkWindow *existing = gtk_application_get_active_window (app);
  if (existing != NULL) {
    gtk_window_present (existing);
    return;
  }

  /*
   * Estructura típica de una ventana libadwaita:
   *
   *   AdwApplicationWindow
   *   └── AdwToolbarView          (organiza barra superior + contenido)
   *       ├── AdwHeaderBar        (la barra de título con los botones)
   *       └── contenido           (de momento, una "página de estado")
   *
   * Cada gtk_*_new()/adw_*_new() devuelve un GtkWidget*. En C no hay
   * herencia "de verdad", así que GTK usa macros como ADW_TOOLBAR_VIEW(x)
   * para convertir el puntero al tipo concreto (y comprobarlo en tiempo
   * de ejecución) antes de llamar a funciones de ese tipo.
   */
  GtkWidget *window = adw_application_window_new (app);
  gtk_window_set_title (GTK_WINDOW (window), "GP VPN");
  gtk_window_set_default_size (GTK_WINDOW (window), 420, 520);

  GtkWidget *toolbar_view = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view),
                                adw_header_bar_new ());

  /* AdwStatusPage: icono grande + título + descripción, centrados. */
  GtkWidget *status = adw_status_page_new ();
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (status),
                                 "network-vpn-symbolic");
  adw_status_page_set_title (ADW_STATUS_PAGE (status), "GP VPN");
  adw_status_page_set_description (ADW_STATUS_PAGE (status),
                                   "Aquí irán las VPN de Empresa y Uni.");

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), status);
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (window),
                                      toolbar_view);

  /*
   * ¿Y la memoria? Los widgets van "colgando" unos de otros: el padre se
   * queda con la referencia del hijo. Al cerrar la ventana, GTK libera
   * todo el árbol. Por eso aquí no hay ningún free(): no somos dueños
   * de ninguno de estos punteros.
   */
  gtk_window_present (GTK_WINDOW (window));
}

int
main (int argc, char *argv[])
{
  /*
   * El "application id" es un nombre único en formato DNS al revés.
   * GNOME lo usa para identificar la app (icono, .desktop, ventana única,
   * ajustes guardados...). Como no tenemos dominio, uno inventado vale.
   */
  AdwApplication *app = adw_application_new ("es.gabran.GpVpn",
                                             G_APPLICATION_DEFAULT_FLAGS);

  /* "Cuando la app emita 'activate', llama a on_activate". */
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);

  /* Aquí entramos en el bucle de eventos; no volvemos hasta que la app
   * termina (al cerrar la última ventana, de momento). */
  int status = g_application_run (G_APPLICATION (app), argc, argv);

  /* Nosotros creamos 'app' con _new(), así que nosotros la soltamos. */
  g_object_unref (app);
  return status;
}
