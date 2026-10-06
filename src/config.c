/*
 * config.c — leer y escribir ~/.config/vpnportal/vpns.ini con GKeyFile,
 * el lector de ficheros "estilo .ini" que ya trae GLib.
 */
#include "config.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>

static char *
config_path (void)
{
#ifdef CONFIG_FILE_PATH
  /* Ruta fija elegida al compilar (lo usa el modo demo, tools/demo.sh). */
  return g_strdup (CONFIG_FILE_PATH);
#else
  /* g_get_user_config_dir() es ~/.config (o lo que diga $XDG_CONFIG_HOME). */
  return g_build_filename (g_get_user_config_dir (), "vpnportal", "vpns.ini",
                           NULL);
#endif
}

VpnConfig *
vpn_config_new (void)
{
  VpnConfig *config = g_new0 (VpnConfig, 1);
  config->id = g_uuid_string_random ();
  /* Cadenas vacías en vez de NULL: así nadie tiene que comprobar NULL. */
  config->name = g_strdup ("");
  config->portal = g_strdup ("");
  config->gateway = g_strdup ("");
  config->user = g_strdup ("");
  return config;
}

void
vpn_config_free (VpnConfig *config)
{
  if (config == NULL)
    return;
  g_free (config->id);
  g_free (config->name);
  g_free (config->portal);
  g_free (config->gateway);
  g_free (config->user);
  g_free (config);
}

/* Nombre de host: letras, números, puntos y guiones, sin empezar ni
 * acabar en punto o guion. Que no pueda empezar por "-" importa: así
 * nadie puede colar una opción (como "-s script") haciéndola pasar
 * por un nombre de servidor. */
gboolean
vpn_config_host_is_valid (const char *host)
{
  gsize len = strlen (host);

  if (len == 0 || len > 253)
    return FALSE;
  if (host[0] == '-' || host[0] == '.' ||
      host[len - 1] == '-' || host[len - 1] == '.')
    return FALSE;
  for (const char *p = host; *p != '\0'; p++)
    if (!g_ascii_isalnum (*p) && *p != '.' && *p != '-')
      return FALSE;
  return TRUE;
}

/* Usuario: lo típico de un email o un login, sin empezar por "-". */
gboolean
vpn_config_user_is_valid (const char *user)
{
  gsize len = strlen (user);

  if (len == 0 || len > 128 || user[0] == '-')
    return FALSE;
  for (const char *p = user; *p != '\0'; p++)
    if (!g_ascii_isalnum (*p) && strchr ("._@+-", *p) == NULL)
      return FALSE;
  return TRUE;
}

/* Lee una clave como texto; si no está, "" (nunca NULL). */
static char *
get_string (GKeyFile *keyfile, const char *group, const char *key)
{
  char *value = g_key_file_get_string (keyfile, group, key, NULL);
  return value != NULL ? value : g_strdup ("");
}

GPtrArray *
vpn_config_load (GError **error)
{
  /* El array "sabe" liberar sus elementos: al destruirlo (o quitar uno)
   * llamará a vpn_config_free por nosotros. */
  GPtrArray *configs =
    g_ptr_array_new_with_free_func ((GDestroyNotify) vpn_config_free);

  g_autofree char *path = config_path ();
  g_autoptr (GKeyFile) keyfile = g_key_file_new ();
  g_autoptr (GError) local_error = NULL;

  if (!g_key_file_load_from_file (keyfile, path, G_KEY_FILE_NONE,
                                  &local_error)) {
    /* Que no exista el fichero es normal (primera vez): no es un error. */
    if (!g_error_matches (local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
      g_propagate_error (error, g_steal_pointer (&local_error));
    return configs;
  }

  /* Cada [grupo] del fichero es una VPN. */
  g_auto (GStrv) groups = g_key_file_get_groups (keyfile, NULL);
  for (char **group = groups; *group != NULL; group++) {
    VpnConfig *config = g_new0 (VpnConfig, 1);
    config->id = g_strdup (*group);
    config->name = get_string (keyfile, *group, "name");
    config->portal = get_string (keyfile, *group, "portal");
    config->gateway = get_string (keyfile, *group, "gateway");
    config->user = get_string (keyfile, *group, "user");
    config->hip = g_key_file_get_boolean (keyfile, *group, "hip", NULL);
    g_ptr_array_add (configs, config);
  }
  return configs;
}

gboolean
vpn_config_save (GPtrArray *configs, GError **error)
{
  g_autoptr (GKeyFile) keyfile = g_key_file_new ();

  for (guint i = 0; i < configs->len; i++) {
    VpnConfig *config = g_ptr_array_index (configs, i);
    g_key_file_set_string (keyfile, config->id, "name", config->name);
    g_key_file_set_string (keyfile, config->id, "portal", config->portal);
    g_key_file_set_string (keyfile, config->id, "gateway", config->gateway);
    g_key_file_set_string (keyfile, config->id, "user", config->user);
    g_key_file_set_boolean (keyfile, config->id, "hip", config->hip);
  }

  g_autofree char *path = config_path ();
  g_autofree char *dir = g_path_get_dirname (path);

  /* 0700: la carpeta solo la puedes ver tú. */
  if (g_mkdir_with_parents (dir, 0700) != 0) {
    int saved_errno = errno;
    g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved_errno),
                 "No se pudo crear %s: %s", dir, g_strerror (saved_errno));
    return FALSE;
  }

  gsize length;
  g_autofree char *data = g_key_file_to_data (keyfile, &length, NULL);

  /* CONSISTENT: escribe en un fichero temporal y luego lo renombra, así
   * un corte a mitad nunca deja el fichero a medias. 0600: solo tú. */
  return g_file_set_contents_full (path, data, (gssize) length,
                                   G_FILE_SET_CONTENTS_CONSISTENT, 0600,
                                   error);
}
