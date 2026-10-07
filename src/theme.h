/*
 * theme.h — los temas de la app (Sistema, Blanco, Negro, Azul neón,
 * Rojo, Frutiger Aero).
 *
 * Cada tema es dos cosas:
 *   - un "esquema de color" de libadwaita: claro, oscuro, o el del sistema;
 *   - un fichero CSS (en data/themes/) que cambia las variables de color
 *     de libadwaita y, si hace falta, añade degradados, brillos...
 */
#pragma once

#include <adwaita.h>

typedef struct {
  const char     *id;           /* lo que se guarda en settings.ini */
  const char     *name;         /* lo que se ve: "Frutiger Aero" */
  const char     *description;
  AdwColorScheme  scheme;
  const char     *css;          /* data/themes/<css>.css, o NULL */
} ThemeInfo;

/* Todos los temas, en el orden en que se muestran. */
const ThemeInfo *theme_list        (guint *n_themes);

/* Pone el tema guardado (o "Sistema"). Llamar al arrancar. */
void             theme_init        (void);

/* Cambia al tema 'id' y lo recuerda para la próxima vez. */
void             theme_set         (const char *id);

const char      *theme_get_current (void);
