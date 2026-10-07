/*
 * snapshot.c — SOLO PARA PRUEBAS (no entra en la app normal): al recibir
 * la señal SIGUSR2, guarda una captura PNG de la ventana de la app en la
 * ruta de $SNAPSHOT_PATH. Sirve para ver cómo queda la interfaz sin mirar
 * la pantalla (lo usa tools/screenshots.py):
 *
 *   kill -USR2 <pid>   ->   $SNAPSHOT_PATH
 *
 * SIGUSR2 y no SIGUSR1: el motor de JavaScript de WebKit (el del login) se
 * queda SIGUSR1 para sí mismo en cuanto arranca.
 *
 * Se engancha solo: el "constructor" se ejecuta antes que main().
 */
#include <glib-unix.h>
#include <gtk/gtk.h>

/* Un GtkWidgetPaintable "sigue" a un widget: cada vez que el widget se
 * pinta, guarda una imagen de cómo ha quedado. Recién creado aún no tiene
 * ninguna, así que primero pedimos que la ventana se repinte y hacemos la
 * captura un poco después (save_snapshot). */
static GdkPaintable *paintable;

static GtkWindow *
find_window (void)
{
  GApplication *app = g_application_get_default ();
  if (app == NULL)
    return NULL;

  GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (app));
  if (window == NULL) {   /* si ninguna está "activa", vale la primera */
    GList *windows = gtk_application_get_windows (GTK_APPLICATION (app));
    window = windows != NULL ? windows->data : NULL;
  }
  return window;
}

static gboolean
save_snapshot (gpointer user_data)
{
  GtkWindow *window = user_data;
  const char *path = g_getenv ("SNAPSHOT_PATH");
  int width = gtk_widget_get_width (GTK_WIDGET (window));
  int height = gtk_widget_get_height (GTK_WIDGET (window));

  /* Pintar la imagen en un "snapshot", convertirlo en textura con el mismo
   * renderizador que usa GTK y guardarla. */
  GtkSnapshot *snapshot = gtk_snapshot_new ();
  gdk_paintable_snapshot (paintable, snapshot, width, height);
  g_autoptr (GskRenderNode) node = gtk_snapshot_free_to_node (snapshot);
  g_clear_object (&paintable);

  if (node == NULL) {
    g_printerr ("snapshot: nothing drawn\n");
    return G_SOURCE_REMOVE;
  }

  GskRenderer *renderer = gtk_native_get_renderer (GTK_NATIVE (window));
  graphene_rect_t area = GRAPHENE_RECT_INIT (0, 0, width, height);
  g_autoptr (GdkTexture) texture = gsk_renderer_render_texture (renderer, node, &area);
  if (texture == NULL || !gdk_texture_save_to_png (texture, path))
    g_printerr ("snapshot: could not save %s\n", path);
  return G_SOURCE_REMOVE;
}

static gboolean
take_snapshot (gpointer user_data)
{
  (void) user_data;
  GtkWindow *window = find_window ();

  if (window == NULL || g_getenv ("SNAPSHOT_PATH") == NULL) {
    g_printerr ("snapshot: no window or no SNAPSHOT_PATH\n");
    return G_SOURCE_CONTINUE;
  }
  if (paintable != NULL)
    return G_SOURCE_CONTINUE;   /* ya hay una captura en marcha */

  paintable = gtk_widget_paintable_new (GTK_WIDGET (window));
  gtk_widget_queue_draw (GTK_WIDGET (window));
  g_timeout_add (300, save_snapshot, window);
  return G_SOURCE_CONTINUE;
}

__attribute__ ((constructor)) static void
snapshot_setup (void)
{
  g_unix_signal_add (SIGUSR2, take_snapshot, NULL);
}
