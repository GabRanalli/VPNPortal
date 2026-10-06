/*
 * auth.c — publica en D-Bus el método Login(args) -> json y lo traduce
 * a una AuthRequest para la interfaz.
 *
 * D-Bus es el "buzón" del escritorio: cada app tiene un nombre (el app
 * id) y objetos con métodos que otros programas pueden llamar. Una
 * GApplication ya tiene su nombre registrado; aquí solo añadimos un
 * objeto más con nuestro método.
 */
#include "auth.h"

#include <string.h>

/* La "firma" del método, en el formato XML que entiende GDBus:
 * recibe un array de strings (as) y devuelve un string (s). */
static const char introspection_xml[] =
  "<node>"
  "  <interface name='" AUTH_INTERFACE "'>"
  "    <method name='Login'>"
  "      <arg type='as' name='args' direction='in'/>"
  "      <arg type='s' name='result' direction='out'/>"
  "    </method>"
  "  </interface>"
  "</node>";

struct _AuthRequest {
  GDBusMethodInvocation *invocation;  /* para responder a quien llamó */
  char     *server;
  char     *saml_request;
  char     *host_id;
  char     *client_version;
  gboolean  is_gateway;
  gboolean  ignore_tls;
};

static GDBusConnection *service_connection;
static guint            registration_id;
static AuthRequestFunc  request_func;
static gpointer         request_data;

void
auth_data_clear (AuthData *data)
{
  g_clear_pointer (&data->username, g_free);
  g_clear_pointer (&data->prelogin_cookie, g_free);
  g_clear_pointer (&data->portal_userauthcookie, g_free);
  g_clear_pointer (&data->token, g_free);
}

static void
auth_request_free (AuthRequest *request)
{
  g_object_unref (request->invocation);
  g_free (request->server);
  g_free (request->saml_request);
  g_free (request->host_id);
  g_free (request->client_version);
  g_free (request);
}

/* ---------------------------------------------------------------- */
/* JSON (a mano: son cuatro campos y no merece una librería)         */
/* ---------------------------------------------------------------- */

/* Añade "texto" con comillas, escapando lo que JSON exige escapar. */
static void
append_json_string (GString *json, const char *value)
{
  if (value == NULL) {
    g_string_append (json, "null");
    return;
  }

  g_string_append_c (json, '"');
  for (const char *p = value; *p != '\0'; p++) {
    switch (*p) {
    case '"':  g_string_append (json, "\\\""); break;
    case '\\': g_string_append (json, "\\\\"); break;
    case '\n': g_string_append (json, "\\n");  break;
    case '\r': g_string_append (json, "\\r");  break;
    case '\t': g_string_append (json, "\\t");  break;
    default:
      if ((unsigned char) *p < 0x20)
        g_string_append_printf (json, "\\u%04x", (unsigned char) *p);
      else
        g_string_append_c (json, *p);
    }
  }
  g_string_append_c (json, '"');
}

static void
reply (AuthRequest *request, GString *json)
{
  g_dbus_method_invocation_return_value (request->invocation,
                                         g_variant_new ("(s)", json->str));
  auth_request_free (request);
}

/* El mismo JSON que imprime gpauth al acertar:
 * {"success":{"username":"…","preloginCookie":"…",…}} */
void
auth_request_succeed (AuthRequest *request, const AuthData *data)
{
  g_autoptr (GString) json = g_string_new ("{\"success\":{\"username\":");
  append_json_string (json, data->username);
  g_string_append (json, ",\"preloginCookie\":");
  append_json_string (json, data->prelogin_cookie ? data->prelogin_cookie : "");
  g_string_append (json, ",\"portalUserauthcookie\":");
  append_json_string (json, data->portal_userauthcookie
                            ? data->portal_userauthcookie : "");
  g_string_append (json, ",\"token\":");
  append_json_string (json, data->token);
  if (request->host_id != NULL) {
    g_string_append (json, ",\"hostId\":");
    append_json_string (json, request->host_id);
  }
  g_string_append (json, "}}");
  reply (request, json);
}

/* Y al fallar: {"failure":"mensaje"} */
void
auth_request_fail (AuthRequest *request, const char *message)
{
  g_autoptr (GString) json = g_string_new ("{\"failure\":");
  append_json_string (json, message);
  g_string_append (json, "}");
  reply (request, json);
}

/* ---------------------------------------------------------------- */
/* El método Login                                                  */
/* ---------------------------------------------------------------- */

/* ¿Es una opción de gpauth que lleva valor detrás? */
static gboolean
option_takes_value (const char *option)
{
  static const char * const with_value[] = {
    "--saml-request", "--os", "--host-id", "--client-version",
    "--certificate", "-c", "--sslkey", "-k", "--key-password", "-p",
    NULL
  };
  return g_strv_contains (with_value, option);
}

/* Lee los argumentos tal y como gpclient se los pasaría a gpauth:
 *   <servidor> [--gateway] --saml-request <…> --host-id <…> … */
static AuthRequest *
parse_args (const char * const *args)
{
  AuthRequest *request = g_new0 (AuthRequest, 1);

  for (guint i = 0; args[i] != NULL; i++) {
    const char *arg = args[i];
    g_autofree char *option = NULL;
    const char *value = NULL;

    if (arg[0] != '-') {            /* lo que no es opción: el servidor */
      g_free (request->server);
      request->server = g_strdup (arg);
      continue;
    }

    /* Admitimos "--opcion=valor" y "--opcion valor". */
    const char *equals = strchr (arg, '=');
    if (equals != NULL) {
      option = g_strndup (arg, equals - arg);
      value = equals + 1;
    } else {
      option = g_strdup (arg);
      if (option_takes_value (option) && args[i + 1] != NULL)
        value = args[++i];
    }

    if (g_str_equal (option, "--gateway"))
      request->is_gateway = TRUE;
    else if (g_str_equal (option, "--ignore-tls-errors"))
      request->ignore_tls = TRUE;
    else if (g_str_equal (option, "--saml-request"))
      g_set_str (&request->saml_request, value);
    else if (g_str_equal (option, "--host-id"))
      g_set_str (&request->host_id, value);
    else if (g_str_equal (option, "--client-version"))
      g_set_str (&request->client_version, value);
    /* El resto (--os, --clean, --hidpi...) no nos hace falta. */
  }
  return request;
}

static void
handle_method_call (GDBusConnection       *connection,
                    const char            *sender,
                    const char            *object_path,
                    const char            *interface_name,
                    const char            *method_name,
                    GVariant              *parameters,
                    GDBusMethodInvocation *invocation,
                    gpointer               user_data)
{
  (void) connection; (void) sender; (void) object_path;
  (void) interface_name; (void) method_name; (void) user_data;

  g_autofree const char **args = NULL;
  g_variant_get (parameters, "(^a&s)", &args);

  AuthRequest *request = parse_args (args);
  /* Nos quedamos la invocación para responder MÁS TARDE, cuando el
   * usuario termine de iniciar sesión (puede tardar minutos). */
  request->invocation = g_object_ref (invocation);

  if (request->server == NULL || request->saml_request == NULL) {
    auth_request_fail (request, "Faltan el servidor o --saml-request");
    return;
  }

  request_func (request, request_data);
}

gboolean
auth_service_start (GDBusConnection *connection,
                    AuthRequestFunc  on_request,
                    gpointer         user_data,
                    GError         **error)
{
  /* Qué función atiende las llamadas a nuestros métodos. */
  static const GDBusInterfaceVTable vtable = {
    .method_call = handle_method_call,
  };
  g_autoptr (GDBusNodeInfo) info =
    g_dbus_node_info_new_for_xml (introspection_xml, error);
  if (info == NULL)
    return FALSE;

  registration_id =
    g_dbus_connection_register_object (connection, AUTH_OBJECT_PATH,
                                       info->interfaces[0], &vtable,
                                       NULL, NULL, error);
  if (registration_id == 0)
    return FALSE;

  service_connection = g_object_ref (connection);
  request_func = on_request;
  request_data = user_data;
  return TRUE;
}

void
auth_service_stop (void)
{
  if (registration_id != 0)
    g_dbus_connection_unregister_object (service_connection, registration_id);
  registration_id = 0;
  g_clear_object (&service_connection);
}

const char *auth_request_get_server (AuthRequest *r)         { return r->server; }
const char *auth_request_get_saml_request (AuthRequest *r)   { return r->saml_request; }
const char *auth_request_get_client_version (AuthRequest *r) { return r->client_version; }
gboolean    auth_request_get_is_gateway (AuthRequest *r)     { return r->is_gateway; }
gboolean    auth_request_get_ignore_tls (AuthRequest *r)     { return r->ignore_tls; }
