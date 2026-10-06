# Pendiente

## Siguiente
- [ ] Temas: claro, oscuro y algunas variantes de color, en Preferencias.
- [ ] Interfaz en inglés (y quizá traducción al español con gettext).
- [ ] Opción para olvidar la sesión de login guardada (como `gpauth --clean`).

## Para publicar mejor
- [ ] Capturas en el README.
- [ ] Instalación con `meson install` (binario, .desktop, icono, helper).

## Hecho
- [x] VPN configurables (añadir, editar, borrar) guardadas en ~/.config.
- [x] Helper con lista de servidores aprobados (sudo sin contraseña seguro).
- [x] Login SAML dentro de la app (WebKitGTK) en vez de la ventana de gpauth,
      recordando la sesión para no tener que teclear cada vez.
- [x] Icono en la barra superior de GNOME (AppIndicator) con el estado y
      conectar/desconectar desde ahí.
- [x] Al cerrar la ventana, seguir en segundo plano sin desconectar.
- [x] Cambiar de VPN desde el menú o la ventana, con aviso.
- [x] Modo demo (tools/demo.sh) para probar sin VPN ni login.
- [x] Arrancar sola al iniciar sesión, oculta en la barra (--background).
- [x] Aviso la primera vez que se cierra la ventana.
- [x] Registro escueto ("Actividad") con los pasos y un botón para ver y
      copiar el registro completo de gpclient.
- [x] Id de la app io.github.GabRanalli.VPNPortal, licencia GPL-3.0 y README
      en inglés y español.
