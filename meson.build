# Meson es el "sistema de compilación": describe QUÉ compilar y CON QUÉ
# librerías. Él genera las órdenes reales (para ninja), así no escribimos
# a mano las líneas de gcc con todos los -I y -l de GTK.

project('gp-vpn', 'c',
  version : '0.1.0',
  default_options : ['c_std=c11', 'warning_level=2'])

# dependency() busca cada librería con pkg-config (los ficheros .pc que
# instalaron los paquetes -dev) y obtiene sus rutas de cabeceras y enlazado.
# De momento solo usamos libadwaita (que ya trae GTK4 consigo).
# WebKit y AppIndicator se añadirán en las fases que los necesiten.
adw_dep = dependency('libadwaita-1', version : '>= 1.6')

executable('gp-vpn',
  'src/main.c',
  'src/config.c',
  'src/vpn.c',
  dependencies : [adw_dep])
