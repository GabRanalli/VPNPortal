/*
 * theme.c — aplicar un tema = elegir claro/oscuro en libadwaita y cargar
 * su CSS en un "proveedor de estilos" que GTK aplica a toda la app.
 *
 * Los .css viajan DENTRO del ejecutable como recursos de GLib (GResource,
 * ver data/vpnportal.gresource.xml): así no dependemos de dónde estén
 * instalados los ficheros.
 */
#include "theme.h"

#include "config.h"

#define THEMES_RESOURCE_PATH "/io/github/GabRanalli/VPNPortal/themes/"

static const ThemeInfo themes[] = {
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

static GtkCssProvider *provider;   /* el CSS del tema actual */
static const ThemeInfo *current;

const ThemeInfo *
theme_list (guint *n_themes)
{
  *n_themes = G_N_ELEMENTS (themes);
  return themes;
}

static const ThemeInfo *
find_theme (const char *id)
{
  for (guint i = 0; i < G_N_ELEMENTS (themes); i++)
    if (g_strcmp0 (themes[i].id, id) == 0)
      return &themes[i];
  return &themes[0];   /* desconocido: "Sistema" */
}

static void
apply (const ThemeInfo *theme)
{
  /* Claro / oscuro / el del sistema: lo hace libadwaita entero. */
  adw_style_manager_set_color_scheme (adw_style_manager_get_default (),
                                      theme->scheme);

  /* Y encima, el CSS del tema (o nada). Cambiar el contenido del
   * proveedor hace que GTK vuelva a pintar todo con los estilos nuevos. */
  if (theme->css != NULL) {
    g_autofree char *path = g_strconcat (THEMES_RESOURCE_PATH, theme->css,
                                         ".css", NULL);
    gtk_css_provider_load_from_resource (provider, path);
  } else {
    gtk_css_provider_load_from_string (provider, "");
  }
  current = theme;
}

void
theme_init (void)
{
  provider = gtk_css_provider_new ();

  /* Prioridad: por encima de libadwaita y del style.css de la app (que
   * tienen menos), para que el tema gane. */
  gtk_style_context_add_provider_for_display (gdk_display_get_default (),
                                              GTK_STYLE_PROVIDER (provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);

  g_autofree char *saved = app_settings_get_string ("theme");
  apply (find_theme (saved));
}

void
theme_set (const char *id)
{
  apply (find_theme (id));
  app_settings_set_string ("theme", current->id);
}

const char *
theme_get_current (void)
{
  return current != NULL ? current->id : themes[0].id;
}
