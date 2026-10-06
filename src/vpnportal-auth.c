/*
 * vpnportal-auth — sustituto de gpauth. gpclient lo lanza (como tu usuario,
 * no como root) cuando necesita un login SAML, con los mismos argumentos
 * que le pasaría a gpauth. Este programa no enseña nada: le pasa los
 * argumentos a la app por D-Bus, espera la respuesta (lo que tardes en
 * iniciar sesión) e imprime el JSON resultante, que es lo que gpclient
 * lee de su salida.
 *
 * Si la app no está abierta, imprime un fallo y gpclient se detiene.
 */
#include <gio/gio.h>

#include "auth.h"

int
main (int argc, char *argv[])
{
  (void) argc;
  g_autoptr (GError) error = NULL;

  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SESSION,
                                                    NULL, &error);
  if (bus == NULL) {
    g_printerr ("vpnportal-auth: no hay bus de sesión: %s\n", error->message);
    g_print ("{\"failure\":\"No session bus\"}\n");
    return 1;
  }

  /* argv[1..] tal cual: "(^as)" convierte un char** terminado en NULL en
   * el array de strings que espera el método. */
  g_autoptr (GVariant) result =
    g_dbus_connection_call_sync (bus, VPNPORTAL_APP_ID, AUTH_OBJECT_PATH,
                                 AUTH_INTERFACE, "Login",
                                 g_variant_new ("(^as)", argv + 1),
                                 G_VARIANT_TYPE ("(s)"),
                                 G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                 G_MAXINT,   /* sin límite: el login espera */
                                 NULL, &error);
  if (result == NULL) {
    g_printerr ("vpnportal-auth: la app no respondió: %s\n", error->message);
    g_print ("{\"failure\":\"VPN Portal app is not running\"}\n");
    return 1;
  }

  const char *json;
  g_variant_get (result, "(&s)", &json);
  g_print ("%s\n", json);
  return 0;
}
