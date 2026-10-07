/*
 * login.h — el diálogo con el navegador embebido donde inicias sesión.
 */
#pragma once

#include <adwaita.h>

#include "auth.h"

/* Abre el login dentro de 'parent' y, al acabar (bien, mal o
 * cancelado), responde a 'request'. Devuelve el diálogo por si hay que
 * cerrarlo desde fuera (cerrarlo = cancelar). */
AdwDialog *login_dialog_run (GtkWidget   *parent,
                             const char  *title,
                             AuthRequest *request);

/* Borra las sesiones guardadas del navegador del login (cookies, etc.):
 * la próxima vez habrá que iniciar sesión desde cero. Es asíncrono: al
 * acabar llama a 'callback', que debe llamar a ..._finish. */
void       login_forget_sessions        (GAsyncReadyCallback callback,
                                         gpointer            user_data);
gboolean   login_forget_sessions_finish (GAsyncResult       *result,
                                         GError            **error);
