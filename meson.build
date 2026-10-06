# Meson es el "sistema de compilación": describe QUÉ compilar y CON QUÉ
# librerías. Él genera las órdenes reales (para ninja), así no escribimos
# a mano las líneas de gcc con todos los -I y -l de GTK.

project('gp-vpn', 'c',
  version : '0.1.0',
  default_options : ['c_std=c11', 'warning_level=2'])

# dependency() busca cada librería con pkg-config (los ficheros .pc que
# instalaron los paquetes -dev) y obtiene sus rutas de cabeceras y enlazado.
adw_dep = dependency('libadwaita-1', version : '>= 1.6')
webkit_dep = dependency('webkitgtk-6.0')
gio_dep = dependency('gio-2.0')

# La app.
executable('gp-vpn',
  'src/main.c',
  'src/auth.c',
  'src/config.c',
  'src/login.c',
  'src/vpn.c',
  dependencies : [adw_dep, webkit_dep])

# El sustituto de gpauth: un programa diminuto que solo necesita GIO.
executable('gp-vpn-auth',
  'src/gp-vpn-auth.c',
  dependencies : [gio_dep])
