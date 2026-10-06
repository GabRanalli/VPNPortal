/*
 * tray.h — el icono de la barra superior y su menú.
 *
 * En GNOME el icono lo pinta la extensión "AppIndicator" (en Ubuntu viene
 * activada). La app solo publica por D-Bus el icono, el título y el menú
 * (ver tray.c).
 */
#pragma once

#include <glib.h>

#include "vpn.h"

/* Lo que el menú necesita saber de cada VPN. */
typedef struct {
  const char *id;
  const char *name;
  VpnState    state;
} TrayItem;

/* Qué hacer cuando se elige algo en el menú. */
typedef struct {
  void (*toggle) (const char *id, gpointer user_data);  /* conectar/desconectar */
  /* mostrar la ventana; 'activation_token' (o NULL) es el permiso de
   * GNOME para traerla al frente (ver tray.c) */
  void (*show)   (const char *activation_token, gpointer user_data);
  void (*quit)   (gpointer user_data);                  /* salir */
} TrayCallbacks;

void     tray_init         (const char          *app_id,
                            const TrayCallbacks *callbacks,
                            gpointer             user_data);
void     tray_update       (const TrayItem *items, guint n_items);
void     tray_shutdown     (void);

/* ¿Hay en el escritorio alguien que pinte iconos de bandeja? */
gboolean tray_is_available (void);
