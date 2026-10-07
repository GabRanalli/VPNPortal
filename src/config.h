/*
 * config.h — las VPN que ha añadido el usuario y cómo se guardan.
 *
 * Se guardan en ~/.config/vpnportal/vpns.ini, un fichero de texto que se
 * puede leer (y editar) a mano:
 *
 *   [3f2a9c1e-...]          <- identificador interno de cada VPN
 *   name=Trabajo
 *   portal=vpn.empresa.example
 *   gateway=gw1.empresa.example
 *   user=yo@empresa.example
 *   hip=true
 */
#pragma once

#include <glib.h>

typedef struct {
  char     *id;       /* identificador único (no se muestra) */
  char     *name;     /* lo que ve el usuario: "Trabajo", "Uni"... */
  char     *portal;   /* servidor del portal GlobalProtect */
  char     *gateway;  /* "" = elegir automáticamente (--auto-gateway) */
  char     *user;     /* "" = sin usuario prefijado */
  gboolean  hip;      /* enviar informe HIP (--hip) */
} VpnConfig;

VpnConfig *vpn_config_new           (void);
void       vpn_config_free          (VpnConfig *config);

/* Las mismas reglas que comprueba el helper (system/vpnportal-helper). */
gboolean   vpn_config_host_is_valid (const char *host);
gboolean   vpn_config_user_is_valid (const char *user);

/* Ajustes sueltos de la app (sí/no), en settings.ini junto a vpns.ini.
 * Si no existe el ajuste, devuelve FALSE. */
gboolean   app_settings_get_bool    (const char *key);
void       app_settings_set_bool    (const char *key, gboolean value);
/* Lo mismo con texto. get devuelve NULL si no existe (hay que liberarlo). */
char      *app_settings_get_string  (const char *key);
void       app_settings_set_string  (const char *key, const char *value);

/* Devuelve un array de VpnConfig* (vacío si aún no hay fichero). */
GPtrArray *vpn_config_load          (GError **error);
gboolean   vpn_config_save          (GPtrArray *configs, GError **error);
