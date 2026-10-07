# Pendiente

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
- [x] Olvidar las sesiones de login guardadas (menú ☰).
- [x] Temas en Preferencias: Sistema, Blanco, Negro, Azul neón, Rojo y
      Frutiger Aero.
- [x] Temas propios (.css en ~/.config/vpnportal/themes), con recarga en
      vivo y guía en docs/THEMES.md.
- [x] Inglés y español (gettext), elegible en Preferencias.
- [x] Nota en el README sobre la ayuda de Claude Opus 5.5.
- [x] Capturas en el README (se regeneran con tools/screenshots.py).
- [x] Instalación con `sudo meson install` (app, lanzador, icono,
      traducciones, helper y regla de sudo comprobada con visudo).
- [x] Registro escueto ("Actividad") con los pasos y un botón para ver y
      copiar el registro completo de gpclient.
- [x] Id de la app io.github.GabRanalli.VPNPortal, licencia GPL-3.0 y README
      en inglés y español.
