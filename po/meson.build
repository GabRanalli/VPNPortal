# Compila cada po/<idioma>.po en un .mo (lo que lee gettext) y crea las
# órdenes para actualizar las traducciones:
#   meson compile -C build vpnportal-pot        (regenera la plantilla .pot)
#   meson compile -C build vpnportal-update-po  (añade los textos nuevos a los .po)
i18n.gettext(meson.project_name(),
  preset : 'glib',
  args : ['--msgid-bugs-address=https://github.com/GabRanalli/VPNPortal/issues'])
