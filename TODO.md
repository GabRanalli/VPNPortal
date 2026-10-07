# Pendiente

## Ideas: pequeñas y muy útiles
- [ ] Notificaciones al conectar/desconectar y, sobre todo, un aviso antes de
      que caduque la sesión (gpclient dice cuánto dura en el registro).
- [ ] Detalles de la conexión: IP asignada, gateway, tiempo conectado y
      tráfico (de /sys/class/net/tun0 y del registro de gpclient).
- [ ] Reconectar sola tras suspender o si se cae la red (opcional por VPN).

## Ideas: medianas
- [ ] Línea de órdenes: `vpnportal --connect <VPN>`, `--disconnect`,
      `--status` (para scripts y atajos de teclado).
- [ ] Conectar automáticamente según la red (p. ej. fuera de la Wi-Fi de
      casa, conectar Trabajo).
- [ ] Exportar e importar VPN a un fichero, para compartirlas.
- [ ] Elegir la gateway de una lista (las que ofrece el portal).

## Ideas: grandes, para llegar a más gente
- [ ] Paquetes: .deb/PPA, AUR (Arch), COPR (Fedora). Flatpak lo veo difícil:
      la app necesita lanzar sudo y gpclient fuera del sandbox.
- [ ] Pruebas automáticas y GitHub Actions (compilar y probar cada cambio).
- [ ] Otras VPN con openconnect: Cisco AnyConnect, Fortinet, Pulse...
- [ ] Integración en los Ajustes rápidos de GNOME (extensión de GNOME Shell,
      en JavaScript).
- [ ] Windows. Sería casi otra app: gpclient y WebKitGTK no existen allí
      (habría que usar openconnect y WebView2), y la bandeja, los permisos
      (UAC en vez de sudo) y el helper funcionan distinto. GTK/libadwaita sí
      se pueden usar en Windows. Ojo: allí sí funciona el cliente oficial de
      GlobalProtect, así que el hueco que cubre la app es menor.

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
