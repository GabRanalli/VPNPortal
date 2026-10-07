# Meson es el "sistema de compilación": describe QUÉ compilar y CON QUÉ
# librerías. Él genera las órdenes reales (para ninja), así no escribimos
# a mano las líneas de gcc con todos los -I y -l de GTK.
#
# Uso:
#   meson setup build                 (configurar, una vez)
#   meson compile -C build            (compilar)
#   sudo meson install -C build       (instalar en /usr/local)
#   sudo ninja -C build uninstall     (desinstalar)

project('vpnportal', 'c',
  version : '0.1.0',
  license : 'GPL-3.0-or-later',
  meson_version : '>= 1.1',
  default_options : ['c_std=c11', 'warning_level=2'])

app_id = 'io.github.GabRanalli.VPNPortal'

# dependency() busca cada librería con pkg-config (los ficheros .pc que
# instalaron los paquetes -dev) y obtiene sus rutas de cabeceras y enlazado.
adw_dep = dependency('libadwaita-1', version : '>= 1.6')
webkit_dep = dependency('webkitgtk-6.0')
gio_dep = dependency('gio-2.0')

# Dónde se instala cada cosa (con el prefijo por defecto, /usr/local).
prefix = get_option('prefix')
bindir = prefix / get_option('bindir')
sbindir = prefix / get_option('sbindir')
libexecdir = prefix / get_option('libexecdir')
datadir = prefix / get_option('datadir')

# Traducciones (gettext): el nombre de los ficheros .mo y dónde se
# instalan, para que el código los encuentre.
i18n = import('i18n')
add_project_arguments(
  '-DGETTEXT_PACKAGE="@0@"'.format(meson.project_name()),
  '-DLOCALEDIR="@0@"'.format(prefix / get_option('localedir')),
  language : 'c')
subdir('po')

# Los ficheros que van dentro del ejecutable (CSS de los temas...). El
# módulo "gnome" de Meson llama a glib-compile-resources y genera un .c.
gnome = import('gnome')
resources = gnome.compile_resources('vpnportal-resources',
  'data/vpnportal.gresource.xml',
  source_dir : 'data',
  c_name : 'vpnportal')

# La app. HELPER_PATH: dónde quedará instalado el helper.
executable('vpnportal',
  'src/main.c',
  'src/auth.c',
  'src/config.c',
  'src/login.c',
  'src/theme.c',
  'src/tray.c',
  'src/vpn.c',
  resources,
  c_args : ['-DHELPER_PATH="@0@"'.format(sbindir / 'vpnportal-helper')],
  dependencies : [adw_dep, webkit_dep],
  install : true)

# El sustituto de gpauth: un programa diminuto que solo necesita GIO.
# Va en libexec (programas que no lanza una persona, sino otro programa).
executable('vpnportal-auth',
  'src/vpnportal-auth.c',
  dependencies : [gio_dep],
  install : true,
  install_dir : get_option('libexecdir'))

# El lanzador (.desktop), con su descripción traducida desde po/, y el icono.
i18n.merge_file(
  input : 'data/@0@.desktop.in'.format(app_id),
  output : '@0@.desktop'.format(app_id),
  type : 'desktop',
  po_dir : 'po',
  install : true,
  install_dir : get_option('datadir') / 'applications')

install_data('data/icons/@0@.svg'.format(app_id),
  install_dir : get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps')

# El helper y la regla de sudo: se rellenan las rutas (@SBINDIR@...) para
# que coincidan con dónde se instala todo.
if get_option('system_files')
  paths = configuration_data()
  paths.set('SBINDIR', sbindir)
  paths.set('LIBEXECDIR', libexecdir)
  paths.set('SUDO_GROUP', get_option('sudo_group'))

  helper = configure_file(
    input : 'system/vpnportal-helper.in',
    output : 'vpnportal-helper',
    configuration : paths)
  install_data(helper,
    install_dir : get_option('sbindir'),
    install_mode : 'rwxr-xr-x')

  sudoers = configure_file(
    input : 'system/vpnportal.sudoers.in',
    output : 'vpnportal.sudoers',
    configuration : paths)

  # Un fichero de sudo roto puede dejarte sin sudo: lo comprobamos YA, al
  # configurar, y si está mal paramos antes de instalar nada.
  visudo = find_program('visudo', dirs : ['/usr/sbin', '/sbin'], required : false)
  if visudo.found()
    check = run_command(visudo, '-cf', sudoers, check : false)
    if check.returncode() != 0
      error('La regla de sudo generada no es válida:\n' + check.stdout() + check.stderr())
    endif
  else
    warning('No se encontró visudo: no se ha podido comprobar la regla de sudo.')
  endif

  # /etc es la carpeta de configuración del sistema, también con /usr/local.
  install_data(sudoers,
    rename : 'vpnportal',
    install_dir : '/etc/sudoers.d',
    install_mode : 'r--r-----')
endif

# Después de instalar: avisar a GNOME de que hay icono y lanzador nuevos.
gnome.post_install(
  gtk_update_icon_cache : true,
  update_desktop_database : true)
