# Pendiente

## En curso
- [ ] Login SAML dentro de la app (WebKitGTK) en vez de la ventana de gpauth,
      recordando la sesión para no tener que teclear cada vez.

## Siguiente
- [ ] Icono en la barra superior de GNOME (AppIndicator) con el estado y
      conectar/desconectar desde ahí.
- [ ] Al cerrar la ventana, seguir en segundo plano sin desconectar.
- [ ] Registro escueto con los pasos ("Autenticando…", "Esperando…",
      "Conectada"…) y un botón para ver el registro completo de gpclient.
- [ ] Temas: claro, oscuro y algunas variantes de color, en Preferencias.
- [ ] Interfaz en inglés (y quizá traducción al español con gettext).
- [ ] Opción para olvidar la sesión de login guardada (como `gpauth --clean`).

## Para publicar
- [x] Cambiar el id de la app a io.github.GabRanalli.VPNPortal.
- [ ] README con capturas, requisitos e instalación.
- [ ] Elegir licencia.
- [ ] Instalación con `meson install` (binario, .desktop, icono, helper).
