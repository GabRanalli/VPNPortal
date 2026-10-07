#!/bin/bash
# demo.sh — arranca VPN Portal en "modo demo": dos VPN inventadas que
# conectan de mentira, sin red, sin login y sin GlobalProtect instalado.
# Sirve para probar la interfaz (conectar, cambiar de VPN, el icono de la
# barra...) sin tener que iniciar sesión de verdad.
#
# Cómo funciona: se compila una versión aparte de la app (con otro id, para
# que conviva con la de verdad) que en vez de /usr/local/sbin/vpnportal-helper
# usa una copia del helper REAL (mismas validaciones) que lanza un gpclient
# falso. sudo y pkexec también son de mentira. Todo vive en build-demo/:
# tu configuración y tus VPN reales no se tocan.
#
# OJO: no se cambian XDG_CONFIG_HOME / XDG_DATA_HOME para aislarla: GTK
# también busca ahí tu tema de iconos y tus ajustes, y la demo se vería
# distinta (sin iconos). Las rutas de la demo se fijan al compilar.
#
# Uso: tools/demo.sh

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/build-demo"

rm -rf "$DEMO"
mkdir -p "$DEMO/bin" "$DEMO/etc"

# --- gpclient de mentira: imita los pasos y crea un "túnel" (un fichero) ---
# Escribe las mismas frases clave que el gpclient real ("Portal prelogin",
# "Connected to VPN"...), que son las que la app usa para la "Actividad".
cat > "$DEMO/gpclient" <<EOF
#!/bin/bash
TUN="$DEMO/tun0"
portal="\${!#}"   # el último argumento es el portal
log() { echo "[demo \$1 gpclient] \$2"; }
trap 'log INFO "Disconnecting from \$portal"; sleep 1; rm -f "\$TUN"; exit 0' INT TERM
log INFO "gpclient (demo) \$*"
log INFO "Portal prelogin: \$portal"; sleep 1
if [[ "\$portal" == *falla* ]]; then
  log ERROR "Failed to connect portal: demo error (this VPN always fails)"
  exit 1
fi
log INFO "SAML auth launch (demo: no real login)"; sleep 1
log INFO "Retrieve the portal config"; sleep 1
log INFO "Perform gateway login"; sleep 1
touch "\$TUN"; log INFO "Connected to VPN (demo)"
while true; do sleep 0.5; done
EOF

# --- sudo y pkexec de mentira: ejecutan la orden tal cual ---
printf '#!/bin/bash\n[ "${1:-}" = -n ] && shift\nexec "$@"\n' > "$DEMO/bin/sudo"
printf '#!/bin/bash\necho "(demo: aquí GNOME pediría tu contraseña)"\nexec "$@"\n' > "$DEMO/bin/pkexec"

# --- el helper real, apuntando al gpclient falso y a una lista de demo ---
sed -e "s|^GPCLIENT=.*|GPCLIENT=$DEMO/gpclient|" \
    -e "s|^ALLOWLIST=.*|ALLOWLIST=$DEMO/etc/allowed-hosts|" \
    -e "s|^AUTH_BINARY=.*|AUTH_BINARY=/nonexistent|" \
    "$ROOT/system/vpnportal-helper.in" > "$DEMO/vpnportal-helper"

chmod +x "$DEMO/gpclient" "$DEMO/bin/sudo" "$DEMO/bin/pkexec" "$DEMO/vpnportal-helper"

# --- dos VPN inventadas (dominios .example: reservados para ejemplos) ---
cat > "$DEMO/vpns.ini" <<'EOF'
[demo-empresa]
name=Demo Empresa
portal=vpn.empresa.example
gateway=gw1.empresa.example
user=demo@empresa.example
hip=true

[demo-uni]
name=Demo Uni
portal=vpn.uni.example
gateway=
user=
hip=false

[demo-falla]
name=Demo Falla
portal=vpn.falla.example
gateway=
user=
hip=false
EOF

# --- compilar la versión demo ---
echo "Compilando la versión demo…"
# Los CSS de los temas van dentro del ejecutable: primero se convierten
# en un .c (es lo mismo que hace Meson con gnome.compile_resources).
glib-compile-resources --sourcedir "$ROOT/data" --generate-source \
  --c-name vpnportal --target "$DEMO/resources.c" \
  "$ROOT/data/vpnportal.gresource.xml"
gcc -std=c11 -O1 -o "$DEMO/vpnportal-demo" \
  "$ROOT"/src/main.c "$ROOT"/src/auth.c "$ROOT"/src/config.c \
  "$ROOT"/src/login.c "$ROOT"/src/theme.c "$ROOT"/src/tray.c \
  "$ROOT"/src/vpn.c "$DEMO/resources.c" \
  -DVPNPORTAL_APP_ID='"io.github.GabRanalli.VPNPortal.Demo"' \
  -DHELPER_PATH="\"$DEMO/vpnportal-helper\"" \
  -DALLOWLIST_PATH="\"$DEMO/etc/allowed-hosts\"" \
  -DTUN_PATH="\"$DEMO/tun0\"" \
  -DCONFIG_FILE_PATH="\"$DEMO/vpns.ini\"" \
  -DWEBKIT_DIR="\"$DEMO/webkit\"" \
  $(pkg-config --cflags --libs libadwaita-1 webkitgtk-6.0)

# Las traducciones: cada po/<idioma>.po -> build-demo/po/<idioma>/LC_MESSAGES
# (la app las busca en "po" junto al ejecutable). Necesita msgfmt (gettext).
if command -v msgfmt > /dev/null; then
  for lang in $(grep -v '^#' "$ROOT/po/LINGUAS"); do
    mkdir -p "$DEMO/po/$lang/LC_MESSAGES"
    msgfmt -o "$DEMO/po/$lang/LC_MESSAGES/vpnportal.mo" "$ROOT/po/$lang.po"
  done
else
  echo "Aviso: falta msgfmt (sudo apt install gettext): la demo saldrá en inglés."
fi

echo "Arrancando VPN Portal (demo)…"
PATH="$DEMO/bin:$PATH" exec "$DEMO/vpnportal-demo" "$@"
