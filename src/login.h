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
