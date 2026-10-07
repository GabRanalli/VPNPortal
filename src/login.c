/*
 * login.c — carga la página de inicio de sesión (SAML) en un WebKitWebView
 * y vigila cada página que se carga hasta encontrar las "credenciales"
 * que el portal entrega al final. Es lo mismo que hace gpauth.
 *
 * Al terminar el login, el portal devuelve los datos de una de estas
 * tres formas (depende del portal):
 *   1. En CABECERAS HTTP de la respuesta: saml-auth-status: 1,
 *      saml-username, prelogin-cookie, portal-userauthcookie.
 *   2. En el HTML, normalmente dentro de un comentario:
 *      <saml-auth-status>1</saml-auth-status><prelogin-cookie>…
 *   3. En una URL especial globalprotectcallback:… (el navegador intenta
 *      abrirla al final).
 *
 * La sesión del navegador (cookies) se guarda en disco: la próxima vez,
 * si tu proveedor de identidad te recuerda, el login pasa solo y ni
 * llegas a ver la página.
 */
#include "login.h"

#include <string.h>
#include <webkit/webkit.h>

/* Si en este tiempo no ha terminado solo, enseñamos la página. */
#define REVEAL_DELAY_MS 2000

typedef struct {
  AdwDialog     *dialog;
  GtkWidget     *stack;        /* "waiting" (ruedecita) o "web" */
  WebKitWebView *web_view;
  AuthRequest   *request;      /* NULL en cuanto se ha respondido */
  GCancellable  *cancellable;
  guint          reveal_id;
} Login;

/* ---------------------------------------------------------------- */
/* La sesión del navegador, compartida y guardada en disco          */
/* ---------------------------------------------------------------- */

static WebKitNetworkSession *
get_network_session (void)
{
  static WebKitNetworkSession *session = NULL;

  if (session == NULL) {
#ifdef WEBKIT_DIR
    /* Carpeta fija elegida al compilar (la usa el modo demo, para no
     * mezclar sus sesiones con las tuyas). */
    g_autofree char *data_dir = g_build_filename (WEBKIT_DIR, "data", NULL);
    g_autofree char *cache_dir = g_build_filename (WEBKIT_DIR, "cache", NULL);
#else
    /* ~/.local/share/vpnportal/webkit y ~/.cache/vpnportal/webkit */
    g_autofree char *data_dir = g_build_filename (g_get_user_data_dir (),
                                                  "vpnportal", "webkit", NULL);
    g_autofree char *cache_dir = g_build_filename (g_get_user_cache_dir (),
                                                   "vpnportal", "webkit", NULL);
#endif
    g_autofree char *cookies = g_build_filename (data_dir, "cookies.sqlite",
                                                 NULL);
    session = webkit_network_session_new (data_dir, cache_dir);
    webkit_cookie_manager_set_persistent_storage (
      webkit_network_session_get_cookie_manager (session),
      cookies, WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
  }
  return session;
}

/* Olvidar sesiones: WebKit borra (en segundo plano) todo lo que guarda
 * esta sesión del navegador: cookies, almacenamiento local, caché... */
void
login_forget_sessions (GAsyncReadyCallback callback, gpointer user_data)
{
  WebKitWebsiteDataManager *manager =
    webkit_network_session_get_website_data_manager (get_network_session ());

  /* timespan 0 = "desde siempre" (no solo lo de las últimas horas). */
  webkit_website_data_manager_clear (manager, WEBKIT_WEBSITE_DATA_ALL, 0,
                                     NULL, callback, user_data);
}

gboolean
login_forget_sessions_finish (GAsyncResult *result, GError **error)
{
  WebKitWebsiteDataManager *manager =
    webkit_network_session_get_website_data_manager (get_network_session ());
  return webkit_website_data_manager_clear_finish (manager, result, error);
}

/* ---------------------------------------------------------------- */
/* Encontrar los datos                                              */
/* ---------------------------------------------------------------- */

/* Busca <tag>valor</tag> en 'text' y devuelve el valor (o NULL). */
static char *
find_tag (const char *text, const char *tag)
{
  g_autofree char *pattern = g_strdup_printf ("<%s>(.*?)</%s>", tag, tag);
  g_autoptr (GRegex) regex = g_regex_new (pattern, G_REGEX_DOTALL, 0, NULL);
  g_autoptr (GMatchInfo) match = NULL;

  if (!g_regex_match (regex, text, 0, &match))
    return NULL;
  return g_strstrip (g_match_info_fetch (match, 1));
}

/* ¿Tenemos ya lo suficiente? (Las mismas reglas que gpauth.) */
static gboolean
auth_data_is_valid (const AuthData *data)
{
  if (data->username == NULL || *data->username == '\0')
    return FALSE;
  return (data->prelogin_cookie && strlen (data->prelogin_cookie) > 5) ||
         (data->portal_userauthcookie &&
          strlen (data->portal_userauthcookie) > 5) ||
         (data->token && *data->token != '\0');
}

/* Forma 2: las etiquetas dentro del HTML (o de un texto cualquiera). */
static gboolean
parse_tags (const char *text, AuthData *data)
{
  g_autofree char *status = find_tag (text, "saml-auth-status");
  if (g_strcmp0 (status, "1") != 0)
    return FALSE;

  data->username = find_tag (text, "saml-username");
  data->prelogin_cookie = find_tag (text, "prelogin-cookie");
  data->portal_userauthcookie = find_tag (text, "portal-userauthcookie");
  return auth_data_is_valid (data);
}

/* Forma 3: globalprotectcallback:<datos>. Los datos vienen o bien como
 * "cas-as=1&un=usuario&token=…" o bien en base64 con las etiquetas. */
static gboolean
parse_callback (const char *uri, AuthData *data)
{
  const char *payload = uri + strlen ("globalprotectcallback:");
  while (*payload == '/')
    payload++;

  if (g_str_has_prefix (payload, "cas-as")) {
    g_autofree char *decoded = g_uri_unescape_string (payload, NULL);
    if (decoded == NULL)
      return FALSE;
    g_autoptr (GHashTable) params =
      g_uri_parse_params (decoded, -1, "&", G_URI_PARAMS_NONE, NULL);
    if (params == NULL)
      return FALSE;
    data->username = g_strdup (g_hash_table_lookup (params, "un"));
    data->token = g_strdup (g_hash_table_lookup (params, "token"));
    return auth_data_is_valid (data);
  }

  gsize length;
  g_autofree guchar *raw = g_base64_decode (payload, &length);
  g_autofree char *text = g_strndup ((const char *) raw, length);
  return parse_tags (text, data);
}

/* Forma 1: las cabeceras de la respuesta de la página principal. */
static gboolean
parse_headers (WebKitWebView *web_view, AuthData *data)
{
  WebKitWebResource *resource = webkit_web_view_get_main_resource (web_view);
  if (resource == NULL)
    return FALSE;

  WebKitURIResponse *response = webkit_web_resource_get_response (resource);
  if (response == NULL)
    return FALSE;

  SoupMessageHeaders *headers = webkit_uri_response_get_http_headers (response);
  if (headers == NULL ||
      g_strcmp0 (soup_message_headers_get_one (headers, "saml-auth-status"),
                 "1") != 0)
    return FALSE;

  data->username = g_strdup (soup_message_headers_get_one (headers,
                                                           "saml-username"));
  data->prelogin_cookie =
    g_strdup (soup_message_headers_get_one (headers, "prelogin-cookie"));
  data->portal_userauthcookie =
    g_strdup (soup_message_headers_get_one (headers, "portal-userauthcookie"));
  return auth_data_is_valid (data);
}

/* Algunas páginas escriben el callback con entidades HTML (&amp; …). */
static char *
decode_html_entities (const char *text)
{
  static const struct { const char *entity; char c; } entities[] = {
    { "&amp;", '&' }, { "&quot;", '"' }, { "&lt;", '<' }, { "&gt;", '>' },
    { "&#x3D;", '=' }, { "&#x3d;", '=' }, { "&#61;", '=' },
    { "&#x2F;", '/' }, { "&#x2f;", '/' }, { "&#47;", '/' },
  };
  GString *out = g_string_new (NULL);

  for (const char *p = text; *p != '\0';) {
    gboolean replaced = FALSE;
    for (guint i = 0; i < G_N_ELEMENTS (entities); i++) {
      if (g_str_has_prefix (p, entities[i].entity)) {
        g_string_append_c (out, entities[i].c);
        p += strlen (entities[i].entity);
        replaced = TRUE;
        break;
      }
    }
    if (!replaced)
      g_string_append_c (out, *p++);
  }
  return g_string_free (out, FALSE);
}

/* Forma 2 o 3 buscadas en el HTML de la página. */
static gboolean
parse_html (const char *html, AuthData *data)
{
  if (parse_tags (html, data))
    return TRUE;
  auth_data_clear (data);

  g_autoptr (GRegex) regex = g_regex_new ("globalprotectcallback:[^\"'<>\\s]+",
                                          0, 0, NULL);
  g_autoptr (GMatchInfo) match = NULL;
  if (!g_regex_match (regex, html, 0, &match))
    return FALSE;

  g_autofree char *found = g_match_info_fetch (match, 0);
  g_autofree char *uri = decode_html_entities (found);
  return parse_callback (uri, data);
}

/* ---------------------------------------------------------------- */
/* Terminar                                                         */
/* ---------------------------------------------------------------- */

/* Hemos encontrado los datos: respondemos y cerramos el diálogo. */
static void
login_succeed (Login *login, const AuthData *data)
{
  if (login->request == NULL)
    return;
  auth_request_succeed (g_steal_pointer (&login->request), data);
  adw_dialog_close (login->dialog);
}

/* El diálogo se ha cerrado (por nosotros o por el usuario). */
static void
on_dialog_closed (AdwDialog *dialog, gpointer user_data)
{
  (void) dialog;
  Login *login = user_data;

  if (login->request != NULL)
    auth_request_fail (g_steal_pointer (&login->request),
                       "Authentication cancelled");

  g_cancellable_cancel (login->cancellable);
  g_clear_handle_id (&login->reveal_id, g_source_remove);
  webkit_web_view_stop_loading (login->web_view);
}

static void
login_free (Login *login)
{
  g_clear_object (&login->cancellable);
  g_free (login);
}

/* ---------------------------------------------------------------- */
/* Vigilar el navegador                                             */
/* ---------------------------------------------------------------- */

static gboolean
reveal_web_view (gpointer user_data)
{
  Login *login = user_data;
  login->reveal_id = 0;
  gtk_stack_set_visible_child_name (GTK_STACK (login->stack), "web");
  return G_SOURCE_REMOVE;
}

/* Ha llegado el HTML de la página que acaba de cargar. */
static void
on_html_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr (GError) error = NULL;
  g_autofree guchar *bytes = NULL;
  gsize length = 0;

  bytes = webkit_web_resource_get_data_finish (WEBKIT_WEB_RESOURCE (source),
                                               result, &length, &error);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;   /* diálogo cerrado: 'login' puede no existir ya */
  if (bytes == NULL)
    return;

  Login *login = user_data;
  g_autofree char *html = g_strndup ((const char *) bytes, length);
  AuthData data = { 0 };
  if (parse_html (html, &data))
    login_succeed (login, &data);
  auth_data_clear (&data);
}

/* Cada vez que una página termina de cargar, miramos cabeceras y HTML. */
static void
on_load_changed (WebKitWebView *web_view, WebKitLoadEvent event,
                 gpointer user_data)
{
  Login *login = user_data;

  if (event != WEBKIT_LOAD_FINISHED || login->request == NULL)
    return;

  AuthData data = { 0 };
  gboolean found = parse_headers (web_view, &data);
  if (found)
    login_succeed (login, &data);
  auth_data_clear (&data);
  if (found)
    return;

  WebKitWebResource *resource = webkit_web_view_get_main_resource (web_view);
  if (resource != NULL)
    webkit_web_resource_get_data (resource, login->cancellable,
                                  on_html_ready, login);
}

/* Antes de navegar a una URL: si es globalprotectcallback:, no se
 * navega (no es una web de verdad); leemos sus datos y listo. */
static gboolean
on_decide_policy (WebKitWebView *web_view, WebKitPolicyDecision *decision,
                  WebKitPolicyDecisionType type, gpointer user_data)
{
  (void) web_view;
  Login *login = user_data;

  if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION &&
      type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION)
    return FALSE;   /* FALSE = que WebKit decida como siempre */

  WebKitNavigationAction *action =
    webkit_navigation_policy_decision_get_navigation_action (
      WEBKIT_NAVIGATION_POLICY_DECISION (decision));
  const char *uri =
    webkit_uri_request_get_uri (webkit_navigation_action_get_request (action));

  if (!g_str_has_prefix (uri, "globalprotectcallback:"))
    return FALSE;

  webkit_policy_decision_ignore (decision);
  AuthData data = { 0 };
  if (parse_callback (uri, &data))
    login_succeed (login, &data);
  auth_data_clear (&data);
  return TRUE;   /* TRUE = ya lo hemos decidido nosotros */
}

/* ---------------------------------------------------------------- */
/* Montar el diálogo                                                */
/* ---------------------------------------------------------------- */

/* El portal espera el "User-Agent" del cliente oficial, como gpauth:
 * "PAN GlobalProtect/<versión> (Linux <distro>) " + el de WebKit con
 * "Version/x" cambiado por "PanGPUI Version/10.0". */
static void
set_user_agent (WebKitWebView *web_view, const char *client_version)
{
  WebKitSettings *settings = webkit_web_view_get_settings (web_view);
  g_autofree char *distro = g_get_os_info (G_OS_INFO_KEY_PRETTY_NAME);
  g_autoptr (GRegex) regex = g_regex_new ("Version/\\S+", 0, 0, NULL);
  g_autofree char *base =
    g_regex_replace_literal (regex, webkit_settings_get_user_agent (settings),
                             -1, 0, "PanGPUI Version/10.0", 0, NULL);
  g_autofree char *agent =
    g_strdup_printf ("PAN GlobalProtect/%s (Linux %s) %s",
                     client_version ? client_version : "6.3.3-619",
                     distro ? distro : "", base);
  webkit_settings_set_user_agent (settings, agent);
}

static GtkWidget *
build_waiting_page (void)
{
  GtkWidget *status = adw_status_page_new ();
  adw_status_page_set_title (ADW_STATUS_PAGE (status), "Iniciando sesión…");
  /* El "paintable" necesita saber en qué widget está para animarse. */
  g_autoptr (AdwSpinnerPaintable) spinner = adw_spinner_paintable_new (status);
  adw_status_page_set_paintable (ADW_STATUS_PAGE (status),
                                 GDK_PAINTABLE (spinner));
  return status;
}

AdwDialog *
login_dialog_run (GtkWidget *parent, const char *title, AuthRequest *request)
{
  Login *login = g_new0 (Login, 1);
  login->request = request;
  login->cancellable = g_cancellable_new ();

  WebKitNetworkSession *session = get_network_session ();
  webkit_network_session_set_tls_errors_policy (
    session, auth_request_get_ignore_tls (request)
             ? WEBKIT_TLS_ERRORS_POLICY_IGNORE
             : WEBKIT_TLS_ERRORS_POLICY_FAIL);

  /* "network-session" solo se puede poner al crear el WebView. */
  login->web_view = g_object_new (WEBKIT_TYPE_WEB_VIEW,
                                  "network-session", session, NULL);
  gtk_widget_set_vexpand (GTK_WIDGET (login->web_view), TRUE);
  set_user_agent (login->web_view,
                  auth_request_get_client_version (request));
  g_signal_connect (login->web_view, "load-changed",
                    G_CALLBACK (on_load_changed), login);
  g_signal_connect (login->web_view, "decide-policy",
                    G_CALLBACK (on_decide_policy), login);

  login->stack = gtk_stack_new ();
  gtk_stack_add_named (GTK_STACK (login->stack), build_waiting_page (),
                       "waiting");
  gtk_stack_add_named (GTK_STACK (login->stack),
                       GTK_WIDGET (login->web_view), "web");

  GtkWidget *toolbar_view = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view),
                                adw_header_bar_new ());
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), login->stack);

  login->dialog = adw_dialog_new ();
  g_autofree char *dialog_title = g_strdup_printf ("Iniciar sesión · %s",
                                                   title);
  adw_dialog_set_title (login->dialog, dialog_title);
  adw_dialog_set_content_width (login->dialog, 560);
  adw_dialog_set_content_height (login->dialog, 680);
  adw_dialog_set_child (login->dialog, toolbar_view);
  g_signal_connect (login->dialog, "closed",
                    G_CALLBACK (on_dialog_closed), login);
  g_object_set_data_full (G_OBJECT (login->dialog), "login", login,
                          (GDestroyNotify) login_free);

  /* La petición SAML es o una URL o una página HTML con un formulario
   * que se envía solo (método POST). */
  const char *saml = auth_request_get_saml_request (request);
  if (g_str_has_prefix (saml, "http"))
    webkit_web_view_load_uri (login->web_view, saml);
  else
    webkit_web_view_load_html (login->web_view, saml, NULL);

  login->reveal_id = g_timeout_add (REVEAL_DELAY_MS, reveal_web_view, login);
  adw_dialog_present (login->dialog, parent);
  return login->dialog;
}
