/*
 * theme.c — aplicar un tema = elegir claro/oscuro en libadwaita y cargar
 * su CSS en un "proveedor de estilos" que GTK aplica a toda la app.
 *
 * Los temas de serie viajan DENTRO del ejecutable como recursos de GLib
 * (GResource, ver data/vpnportal.gresource.xml). Los propios se leen de
 * los ficheros .css de ~/.config/vpnportal/themes/.
 */
#include "theme.h"

#include <string.h>

#include "config.h"

#define THEMES_RESOURCE_PATH "/io/github/GabRanalli/VPNPortal/themes/"
#define USER_PREFIX "user:"

/* Los temas de serie: id, nombre, descripción, esquema, recurso. */
static const struct {
  const char     *id;
  const char     *name;
  const char     *description;
  AdwColorScheme  scheme;
  const char     *resource;
} builtin_themes[] = {
  { "system", "Sistema", "Claro u oscuro, como tengas GNOME",
    ADW_COLOR_SCHEME_DEFAULT, NULL },
  { "white", "Blanco", "Claro y limpio",
    ADW_COLOR_SCHEME_FORCE_LIGHT, "white" },
  { "black", "Negro", "Oscuro, de negro puro",
    ADW_COLOR_SCHEME_FORCE_DARK, "black" },
  { "neon", "Azul neón", "Oscuro, con bordes y botones cian que brillan",
    ADW_COLOR_SCHEME_FORCE_DARK, "neon" },
  { "red", "Rojo", "Oscuro, en tonos granate y rojo",
    ADW_COLOR_SCHEME_FORCE_DARK, "red" },
  { "aero", "Frutiger Aero", "Cielo, cristal y botones con brillo",
    ADW_COLOR_SCHEME_FORCE_LIGHT, "aero" },
};

static GPtrArray        *themes;           /* ThemeInfo* (de serie + propios) */
static char             *current_id;
static GtkCssProvider   *provider;         /* el CSS del tema actual */
static GtkCssProvider   *swatch_provider;  /* muestras de los temas propios */
static GFileMonitor     *monitor;          /* vigila el .css del tema propio */
static ThemeMessageFunc  problem_func;
static gpointer          problem_data;

/* ---------------------------------------------------------------- */
/* La lista de temas                                                */
/* ---------------------------------------------------------------- */

static void
theme_info_free (ThemeInfo *theme)
{
  g_free (theme->id);
  g_free (theme->name);
  g_free (theme->description);
  g_free (theme->resource);
  g_free (theme->file);
  g_free (theme->swatch_class);
  g_free (theme);
}

static ThemeInfo *
find_theme (const char *id)
{
  for (guint i = 0; i < themes->len; i++) {
    ThemeInfo *theme = g_ptr_array_index (themes, i);
    if (g_strcmp0 (theme->id, id) == 0)
      return theme;
  }
  return NULL;
}

/*
 * Los datos de un tema propio van en el primer comentario del .css:
 *
 *   / * name: Atardecer
 *     * description: Naranjas y morados
 *     * base: dark
 *     * swatch: #2b1a3a #ff8a3d
 *     * /
 *
 * Leemos cada línea "clave: valor" (ignorando los asteriscos del margen).
 * Devuelve los dos colores de la muestra en swatch[0..1] (o NULL).
 */
static void
parse_metadata (const char *contents, ThemeInfo *theme, char **swatch)
{
  const char *start = strstr (contents, "/*");
  const char *end = start != NULL ? strstr (start + 2, "*/") : NULL;
  if (end == NULL)
    return;

  g_autofree char *comment = g_strndup (start + 2, end - start - 2);
  g_auto (GStrv) lines = g_strsplit (comment, "\n", -1);

  for (char **line = lines; *line != NULL; line++) {
    char *text = g_strstrip (*line);
    while (*text == '*')
      text++;
    char *colon = strchr (text, ':');
    if (colon == NULL)
      continue;

    *colon = '\0';
    g_autofree char *key = g_ascii_strdown (g_strstrip (text), -1);
    const char *value = g_strstrip (colon + 1);
    if (*value == '\0')
      continue;

    if (g_str_equal (key, "name")) {
      g_set_str (&theme->name, value);
    } else if (g_str_equal (key, "description")) {
      g_set_str (&theme->description, value);
    } else if (g_str_equal (key, "base")) {
      if (g_ascii_strcasecmp (value, "dark") == 0)
        theme->scheme = ADW_COLOR_SCHEME_FORCE_DARK;
      else if (g_ascii_strcasecmp (value, "light") == 0)
        theme->scheme = ADW_COLOR_SCHEME_FORCE_LIGHT;
    } else if (g_str_equal (key, "swatch")) {
      /* Solo aceptamos colores de verdad (lo que entienda GdkRGBA), así
       * el texto no puede colar CSS raro en la hoja de las muestras. */
      g_auto (GStrv) colors = g_strsplit_set (value, " ,", -1);
      int n = 0;
      for (char **c = colors; *c != NULL && n < 2; c++) {
        GdkRGBA rgba;
        if (**c != '\0' && gdk_rgba_parse (&rgba, *c))
          swatch[n++] = gdk_rgba_to_string (&rgba);
      }
    }
  }
}

/* Lee la carpeta de temas propios y rehace la lista + sus muestras. */
void
theme_reload_user_themes (void)
{
  /* Quitamos los propios que hubiera (van detrás de los de serie). */
  g_ptr_array_set_size (themes, G_N_ELEMENTS (builtin_themes));

  g_autofree char *dir_path = theme_user_dir ();
  g_autoptr (GDir) dir = g_dir_open (dir_path, 0, NULL);
  g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
  const char *entry;
  while (dir != NULL && (entry = g_dir_read_name (dir)) != NULL)
    if (g_str_has_suffix (entry, ".css"))
      g_ptr_array_add (names, g_strdup (entry));
  g_ptr_array_sort_values (names, (GCompareFunc) g_strcmp0);

  g_autoptr (GString) swatch_css = g_string_new (NULL);

  for (guint i = 0; i < names->len; i++) {
    const char *filename = g_ptr_array_index (names, i);
    g_autofree char *base = g_strndup (filename, strlen (filename) - 4);
    ThemeInfo *theme = g_new0 (ThemeInfo, 1);

    theme->id = g_strconcat (USER_PREFIX, base, NULL);
    theme->name = g_strdup (base);
    theme->description = g_strdup ("Tema propio");
    theme->scheme = ADW_COLOR_SCHEME_DEFAULT;
    theme->file = g_build_filename (dir_path, filename, NULL);
    theme->swatch_class = g_strdup_printf ("user-%u", i);

    g_autofree char *contents = NULL;
    char *swatch[2] = { NULL, NULL };
    if (g_file_get_contents (theme->file, &contents, NULL, NULL))
      parse_metadata (contents, theme, swatch);

    /* La muestra: dos colores en diagonal (o un gris neutro si el tema
     * no dice nada). */
    g_string_append_printf (swatch_css,
                            ".theme-swatch.%s { background-image: "
                            "linear-gradient(135deg, %s 50%%, %s 50%%); }\n",
                            theme->swatch_class,
                            swatch[0] ? swatch[0] : "#9a9996",
                            swatch[1] ? swatch[1]
                                      : (swatch[0] ? swatch[0] : "#5e5c64"));
    g_free (swatch[0]);
    g_free (swatch[1]);
    g_ptr_array_add (themes, theme);
  }

  gtk_css_provider_load_from_string (swatch_provider, swatch_css->str);
}

/* ---------------------------------------------------------------- */
/* Aplicar                                                          */
/* ---------------------------------------------------------------- */

/* GTK nos avisa de cada error que encuentra al leer el CSS. */
static void
on_parsing_error (GtkCssProvider *css_provider, GtkCssSection *section,
                  const GError *error, gpointer user_data)
{
  (void) css_provider; (void) user_data;
  const GtkCssLocation *location = gtk_css_section_get_start_location (section);
  g_autofree char *message = g_strdup_printf ("Error en el CSS del tema, "
                                              "línea %zu: %s",
                                              location->lines + 1,
                                              error->message);
  if (problem_func != NULL)
    problem_func (message, problem_data);
}

static void apply (ThemeInfo *theme);

/* El .css del tema propio actual ha cambiado (alguien lo está editando):
 * lo volvemos a cargar para ver los cambios al momento. */
static void
on_theme_file_changed (GFileMonitor *file_monitor, GFile *file, GFile *other,
                       GFileMonitorEvent event, gpointer user_data)
{
  (void) file_monitor; (void) file; (void) other; (void) user_data;
  if (event != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT &&
      event != G_FILE_MONITOR_EVENT_CREATED)
    return;

  ThemeInfo *theme = find_theme (current_id);
  if (theme != NULL && theme->file != NULL)
    gtk_css_provider_load_from_path (provider, theme->file);
}

static void
apply (ThemeInfo *theme)
{
  g_set_str (&current_id, theme->id);
  g_clear_object (&monitor);

  /* Claro / oscuro / el del sistema: lo hace libadwaita entero. */
  adw_style_manager_set_color_scheme (adw_style_manager_get_default (),
                                      theme->scheme);

  /* Y encima, el CSS del tema (o nada). Cambiar el contenido del
   * proveedor hace que GTK vuelva a pintar todo con los estilos nuevos. */
  if (theme->resource != NULL) {
    g_autofree char *path = g_strconcat (THEMES_RESOURCE_PATH,
                                         theme->resource, ".css", NULL);
    gtk_css_provider_load_from_resource (provider, path);
  } else if (theme->file != NULL) {
    gtk_css_provider_load_from_path (provider, theme->file);

    /* Recarga en vivo: vigilamos el fichero mientras sea el tema actual. */
    g_autoptr (GFile) file = g_file_new_for_path (theme->file);
    monitor = g_file_monitor_file (file, G_FILE_MONITOR_NONE, NULL, NULL);
    if (monitor != NULL)
      g_signal_connect (monitor, "changed",
                        G_CALLBACK (on_theme_file_changed), NULL);
  } else {
    gtk_css_provider_load_from_string (provider, "");
  }
}

/* ---------------------------------------------------------------- */
/* API                                                              */
/* ---------------------------------------------------------------- */

void
theme_init (ThemeMessageFunc on_problem, gpointer user_data)
{
  problem_func = on_problem;
  problem_data = user_data;

  themes = g_ptr_array_new_with_free_func ((GDestroyNotify) theme_info_free);
  for (guint i = 0; i < G_N_ELEMENTS (builtin_themes); i++) {
    ThemeInfo *theme = g_new0 (ThemeInfo, 1);
    theme->id = g_strdup (builtin_themes[i].id);
    theme->name = g_strdup (builtin_themes[i].name);
    theme->description = g_strdup (builtin_themes[i].description);
    theme->scheme = builtin_themes[i].scheme;
    theme->resource = g_strdup (builtin_themes[i].resource);
    theme->swatch_class = g_strdup (builtin_themes[i].id);
    g_ptr_array_add (themes, theme);
  }

  GdkDisplay *display = gdk_display_get_default ();

  /* Prioridad del tema: por encima de libadwaita y del style.css de la
   * app (que tienen menos), para que el tema gane. */
  provider = gtk_css_provider_new ();
  g_signal_connect (provider, "parsing-error",
                    G_CALLBACK (on_parsing_error), NULL);
  gtk_style_context_add_provider_for_display (display,
                                              GTK_STYLE_PROVIDER (provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);

  swatch_provider = gtk_css_provider_new ();
  gtk_style_context_add_provider_for_display (display,
                                              GTK_STYLE_PROVIDER (swatch_provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  theme_reload_user_themes ();

  g_autofree char *saved = app_settings_get_string ("theme");
  ThemeInfo *theme = find_theme (saved);
  apply (theme != NULL ? theme : g_ptr_array_index (themes, 0));
}

GPtrArray *
theme_list (void)
{
  return themes;
}

void
theme_set (const char *id)
{
  ThemeInfo *theme = find_theme (id);
  apply (theme != NULL ? theme : g_ptr_array_index (themes, 0));
  app_settings_set_string ("theme", current_id);
}

const char *
theme_get_current (void)
{
  return current_id != NULL ? current_id : builtin_themes[0].id;
}

char *
theme_user_dir (void)
{
  /* Junto a la configuración (así la demo, que la cambia de sitio,
   * también tiene su propia carpeta de temas). */
  g_autofree char *settings_dir = app_settings_dir ();
  char *dir = g_build_filename (settings_dir, "themes", NULL);
  g_mkdir_with_parents (dir, 0700);
  return dir;
}

char *
theme_install (GFile *css, GError **error)
{
  g_autofree char *basename = g_file_get_basename (css);
  if (basename == NULL || !g_str_has_suffix (basename, ".css")) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME,
                 "Un tema tiene que ser un fichero .css");
    return NULL;
  }

  g_autofree char *dir = theme_user_dir ();
  g_autofree char *target_path = g_build_filename (dir, basename, NULL);
  g_autoptr (GFile) target = g_file_new_for_path (target_path);

  /* OVERWRITE: si ya existía un tema con ese nombre, es una versión nueva. */
  if (!g_file_copy (css, target, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL,
                    error))
    return NULL;

  theme_reload_user_themes ();
  g_autofree char *base = g_strndup (basename, strlen (basename) - 4);
  return g_strconcat (USER_PREFIX, base, NULL);
}

gboolean
theme_remove (const char *id, GError **error)
{
  ThemeInfo *theme = find_theme (id);
  if (theme == NULL || theme->file == NULL) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                 "Ese tema no es un tema propio");
    return FALSE;
  }

  g_autoptr (GFile) file = g_file_new_for_path (theme->file);
  if (!g_file_delete (file, NULL, error))
    return FALSE;

  gboolean was_current = g_strcmp0 (id, current_id) == 0;
  theme_reload_user_themes ();
  if (was_current)
    theme_set (builtin_themes[0].id);
  return TRUE;
}
