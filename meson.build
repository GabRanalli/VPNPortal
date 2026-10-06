# Meson es el "sistema de compilación": describe QUÉ compilar y CON QUÉ
# librerías. Él genera las órdenes reales (para ninja), así no escribimos
# a mano las líneas de gcc con todos los -I y -l de GTK.

project('vpnportal', 'c',
  version : '0.1.0',
  license : 'GPL-3.0-or-later',
  default_options : ['c_std=c11', 'warning_level=2'])

# dependency() busca cada librería con pkg-config (los ficheros .pc que
# instalaron los paquetes -dev) y obtiene sus rutas de cabeceras y enlazado.
adw_dep = dependency('libadwaita-1', version : '>= 1.6')
webkit_dep = dependency('webkitgtk-6.0')
indicator_dep = dependency('ayatana-appindicator-glib')
gio_dep = dependency('gio-2.0')

# La app.
executable('vpnportal',
  'src/main.c',
  'src/auth.c',
  'src/config.c',
  'src/login.c',
  'src/tray.c',
  'src/vpn.c',
  dependencies : [adw_dep, webkit_dep, indicator_dep])

# El sustituto de gpauth: un programa diminuto que solo necesita GIO.
executable('vpnportal-auth',
  'src/vpnportal-auth.c',
  dependencies : [gio_dep])
