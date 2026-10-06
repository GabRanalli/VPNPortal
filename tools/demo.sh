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
# Uso: tools/demo.sh

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO="$ROOT/build-demo"

rm -rf "$DEMO"
mkdir -p "$DEMO/bin" "$DEMO/config/vpnportal" "$DEMO/data" "$DEMO/cache" "$DEMO/etc"

# --- gpclient de mentira: imita los pasos y crea un "túnel" (un fichero) ---
cat > "$DEMO/gpclient" <<EOF
#!/bin/bash
TUN="$DEMO/tun0"
portal="\${!#}"   # el último argumento es el portal
trap 'echo "Desconectando de \$portal…"; sleep 1; rm -f "\$TUN"; echo "Desconectada."; exit 0' INT TERM
echo "gpclient (demo) \$*"
echo "Conectando al portal \$portal…"; sleep 1
echo "Inicio de sesión correcto (demo, sin login de verdad)"; sleep 1
echo "Conectando a la gateway…"; sleep 1
touch "\$TUN"; echo "Connected to VPN (demo)"
while true; do sleep 0.5; done
EOF

# --- sudo y pkexec de mentira: ejecutan la orden tal cual ---
printf '#!/bin/bash\n[ "${1:-}" = -n ] && shift\nexec "$@"\n' > "$DEMO/bin/sudo"
printf '#!/bin/bash\necho "(demo: aquí GNOME pediría tu contraseña)"\nexec "$@"\n' > "$DEMO/bin/pkexec"

# --- el helper real, apuntando al gpclient falso y a una lista de demo ---
sed -e "s|^GPCLIENT=.*|GPCLIENT=$DEMO/gpclient|" \
    -e "s|^ALLOWLIST=.*|ALLOWLIST=$DEMO/etc/allowed-hosts|" \
    -e "s|^AUTH_BINARY=.*|AUTH_BINARY=/nonexistent|" \
    "$ROOT/system/vpnportal-helper" > "$DEMO/vpnportal-helper"

chmod +x "$DEMO/gpclient" "$DEMO/bin/sudo" "$DEMO/bin/pkexec" "$DEMO/vpnportal-helper"

# --- dos VPN inventadas (dominios .example: reservados para ejemplos) ---
cat > "$DEMO/config/vpnportal/vpns.ini" <<'EOF'
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
EOF

# --- compilar la versión demo ---
echo "Compilando la versión demo…"
gcc -std=c11 -O1 -o "$DEMO/vpnportal-demo" \
  "$ROOT"/src/main.c "$ROOT"/src/auth.c "$ROOT"/src/config.c \
  "$ROOT"/src/login.c "$ROOT"/src/tray.c "$ROOT"/src/vpn.c \
  -DVPNPORTAL_APP_ID='"io.github.GabRanalli.VPNPortal.Demo"' \
  -DHELPER_PATH="\"$DEMO/vpnportal-helper\"" \
  -DALLOWLIST_PATH="\"$DEMO/etc/allowed-hosts\"" \
  -DTUN_PATH="\"$DEMO/tun0\"" \
  $(pkg-config --cflags --libs libadwaita-1 webkitgtk-6.0)

echo "Arrancando VPN Portal (demo)…"
PATH="$DEMO/bin:$PATH" \
XDG_CONFIG_HOME="$DEMO/config" \
XDG_DATA_HOME="$DEMO/data" \
XDG_CACHE_HOME="$DEMO/cache" \
  exec "$DEMO/vpnportal-demo"
