/*
 * theme.h — los temas de la app: los que trae (Sistema, Blanco, Negro,
 * Azul neón, Rojo, Frutiger Aero) y los "propios" que añada cada uno.
 *
 * Cada tema es dos cosas:
 *   - un "esquema de color" de libadwaita: claro, oscuro, o el del sistema;
 *   - un fichero CSS que cambia las variables de color de libadwaita y, si
 *     hace falta, añade degradados, brillos...
 *
 * Los propios son ficheros .css en ~/.config/vpnportal/themes/ (ver
 * docs/THEMES.md).
 */
#pragma once

#include <adwaita.h>

typedef struct {
  char           *id;            /* lo que se guarda: "aero", "user:mio" */
  char           *name;          /* lo que se ve: "Frutiger Aero" */
  char           *description;
  AdwColorScheme  scheme;
  char           *resource;      /* tema de serie: data/themes/<x>.css */
  char           *file;          /* tema propio: ruta de su .css */
  char           *swatch_class;  /* clase CSS que pinta su muestra */
} ThemeInfo;

/* Para avisar de problemas (p. ej. errores en el CSS de un tema propio). */
typedef void (*ThemeMessageFunc) (const char *message, gpointer user_data);

/* Pone el tema guardado (o "Sistema"). Llamar al arrancar. */
void        theme_init               (ThemeMessageFunc on_problem,
                                      gpointer         user_data);

/* Todos los temas (ThemeInfo*), de serie primero. Es "prestado": no lo
 * liberes, y puede cambiar al llamar a theme_reload_user_themes. */
GPtrArray  *theme_list               (void);

/* Vuelve a leer la carpeta de temas propios. */
void        theme_reload_user_themes (void);

/* Cambia al tema 'id' y lo recuerda para la próxima vez. */
void        theme_set                (const char *id);
const char *theme_get_current        (void);

/* La carpeta de temas propios (la crea si no existe). Hay que liberarla. */
char       *theme_user_dir           (void);

/* Copia un .css a la carpeta de temas propios. Devuelve el id del tema
 * nuevo (hay que liberarlo), o NULL con 'error'. */
char       *theme_install            (GFile *css, GError **error);

/* Borra un tema propio. Si era el actual, se vuelve a "Sistema". */
gboolean    theme_remove             (const char *id, GError **error);
