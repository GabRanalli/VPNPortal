/*
 * auth.h — el "servicio de login" que la app ofrece por D-Bus.
 *
 * Cómo encaja todo:
 *
 *   gpclient (root) ──lanza──> vpnportal-auth (tu usuario)
 *                                  │ D-Bus: Login(args)
 *                                  v
 *                              la app (este módulo) ──> diálogo de login
 *                                  │ devuelve el JSON
 *                                  v
 *   gpclient <──── stdout ──── vpnportal-auth
 *
 * gpclient cree que está hablando con gpauth (su programa de login): le
 * pasamos los mismos argumentos y le devolvemos el mismo JSON.
 */
#pragma once

#include <gio/gio.h>

/* El nombre de la app en el bus de sesión (GApplication lo registra
 * solo) y dónde está nuestro objeto de login. vpnportal-auth usa lo mismo. */
#ifndef VPNPORTAL_APP_ID
#define VPNPORTAL_APP_ID "io.github.GabRanalli.VPNPortal"
#endif
#define AUTH_OBJECT_PATH "/io/github/GabRanalli/VPNPortal/Auth"
#define AUTH_INTERFACE   "io.github.GabRanalli.VPNPortal.Auth"

/* Lo que se consigue al iniciar sesión (lo que gpauth imprimiría). */
typedef struct {
  char *username;
  char *prelogin_cookie;
  char *portal_userauthcookie;
  char *token;
} AuthData;

void auth_data_clear (AuthData *data);

/* Una petición de login en curso. Hay que responderla SIEMPRE una vez,
 * con auth_request_succeed o auth_request_fail (que además la liberan). */
typedef struct _AuthRequest AuthRequest;

typedef void (*AuthRequestFunc) (AuthRequest *request, gpointer user_data);

gboolean    auth_service_start (GDBusConnection *connection,
                                AuthRequestFunc  on_request,
                                gpointer         user_data,
                                GError         **error);
void        auth_service_stop  (void);

const char *auth_request_get_server         (AuthRequest *request);
const char *auth_request_get_saml_request   (AuthRequest *request);
const char *auth_request_get_client_version (AuthRequest *request);
gboolean    auth_request_get_is_gateway     (AuthRequest *request);
gboolean    auth_request_get_ignore_tls     (AuthRequest *request);

void        auth_request_succeed (AuthRequest *request, const AuthData *data);
void        auth_request_fail    (AuthRequest *request, const char *message);
