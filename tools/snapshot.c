/*
 * snapshot.c — SOLO PARA PRUEBAS (no entra en la app normal): al recibir
 * la señal SIGUSR1, guarda una captura PNG de la ventana activa en la
 * ruta de $SNAPSHOT_PATH. Sirve para ver cómo queda un tema sin tener
 * que mirar la pantalla:
 *
 *   kill -USR1 <pid>   ->   $SNAPSHOT_PATH
 *
 * Se engancha solo: el "constructor" se ejecuta antes que main().
 */
#include <glib-unix.h>
#include <gtk/gtk.h>

static gboolean
take_snapshot (gpointer user_data)
{
  (void) user_data;
  GApplication *app = g_application_get_default ();
  GtkWindow *window = app != NULL
    ? gtk_application_get_active_window (GTK_APPLICATION (app)) : NULL;
  const char *path = g_getenv ("SNAPSHOT_PATH");

  if (window == NULL || path == NULL)
    return G_SOURCE_CONTINUE;

  int width = gtk_widget_get_width (GTK_WIDGET (window));
  int height = gtk_widget_get_height (GTK_WIDGET (window));

  /* Pintar la ventana en un "snapshot", convertirlo en textura con el
   * mismo renderizador que usa GTK y guardarla. */
  g_autoptr (GdkPaintable) paintable = gtk_widget_paintable_new (GTK_WIDGET (window));
  GtkSnapshot *snapshot = gtk_snapshot_new ();
  gdk_paintable_snapshot (paintable, snapshot, width, height);
  g_autoptr (GskRenderNode) node = gtk_snapshot_free_to_node (snapshot);
  if (node == NULL)
    return G_SOURCE_CONTINUE;

  GskRenderer *renderer = gtk_native_get_renderer (GTK_NATIVE (window));
  graphene_rect_t area = GRAPHENE_RECT_INIT (0, 0, width, height);
  g_autoptr (GdkTexture) texture = gsk_renderer_render_texture (renderer, node, &area);
  gdk_texture_save_to_png (texture, path);
  return G_SOURCE_CONTINUE;
}

__attribute__ ((constructor)) static void
snapshot_setup (void)
{
  g_unix_signal_add (SIGUSR1, take_snapshot, NULL);
}
