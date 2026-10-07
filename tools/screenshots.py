#!/usr/bin/env python3
"""
screenshots.py — genera las capturas del README (docs/screenshots/).

Compila una copia aparte de la app (otro id, todo en build-shots/) con
tools/snapshot.c, que guarda una captura PNG al recibir SIGUSR2. Usa VPN
inventadas, el helper real con un gpclient falso (como tools/demo.sh) y una
página de inicio de sesión de mentira para la captura del login. Tu
configuración y tus VPN reales no se tocan.

La app se maneja por D-Bus, como lo haría GNOME: el menú del icono de la
barra (para conectar), las acciones de la app (tema, Preferencias)...

Uso:  tools/screenshots.py          (necesita una sesión gráfica)
"""
import http.server
import os
import shutil
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

import gi
gi.require_version('GdkPixbuf', '2.0')
from gi.repository import GdkPixbuf, Gio, GLib

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'build-shots'
SHOTS = ROOT / 'docs' / 'screenshots'
APP_ID = 'io.github.GabRanalli.VPNPortal.Shots'
APP_PATH = '/' + APP_ID.replace('.', '/')

# Las VPN de las capturas y los textos de la página de login, por idioma.
LANGS = {
    'en': {'work': 'Work', 'uni': 'University',
           'idp': ('Example University', 'Sign in', 'Use your university account',
                   'you@uni.example', 'Password', 'Sign in')},
    'es': {'work': 'Trabajo', 'uni': 'Universidad',
           'idp': ('Universidad de Ejemplo', 'Iniciar sesión', 'Usa tu cuenta de la universidad',
                   'tu@uni.example', 'Contraseña', 'Entrar')},
}

bus = Gio.bus_get_sync(Gio.BusType.SESSION)


def run(*cmd, **kw):
    subprocess.run(cmd, check=True, **kw)


# ------------------------------------------------------------------
# Preparar: compilar la app de capturas y sus piezas de mentira
# ------------------------------------------------------------------

def build():
    shutil.rmtree(OUT, ignore_errors=True)
    (OUT / 'bin').mkdir(parents=True)
    (OUT / 'etc').mkdir()

    # gpclient de mentira: como el de la demo, pero la VPN "uni" pide login
    # (llama al sustituto de gpauth, como hace el gpclient de verdad).
    (OUT / 'gpclient').write_text(f'''#!/bin/bash
TUN="{OUT}/tun0"
portal="${{!#}}"
log() {{ echo "[shots $1 gpclient] $2"; }}
trap 'rm -f "$TUN"; exit 0' INT TERM
log INFO "Portal prelogin: $portal"; sleep 0.5
if [[ "$portal" == *uni* ]]; then
  log INFO "SAML auth launch"
  "$GP_AUTH_BINARY" "$portal" --saml-request "$MOCK_IDP_URL" --os Linux \\
    --host-id shots --client-version 6.3.3-619 > /dev/null || exit 1
fi
log INFO "Retrieve the portal config"; sleep 0.5
log INFO "Perform gateway login"; sleep 0.5
log INFO "HIP report submitted"; sleep 0.3
touch "$TUN"; log INFO "Connected to VPN"
while true; do sleep 0.5; done
''')
    (OUT / 'bin' / 'sudo').write_text('#!/bin/bash\n[ "${1:-}" = -n ] && shift\nexec "$@"\n')
    (OUT / 'bin' / 'pkexec').write_text('#!/bin/bash\nexec "$@"\n')
    helper = (ROOT / 'system' / 'vpnportal-helper.in').read_text()
    helper = '\n'.join(
        f'GPCLIENT={OUT}/gpclient' if l.startswith('GPCLIENT=') else
        f'ALLOWLIST={OUT}/etc/allowed-hosts' if l.startswith('ALLOWLIST=') else
        f'AUTH_BINARY={OUT}/vpnportal-auth' if l.startswith('AUTH_BINARY=') else l
        for l in helper.split('\n'))
    (OUT / 'vpnportal-helper').write_text(helper)
    for f in ('gpclient', 'bin/sudo', 'bin/pkexec', 'vpnportal-helper'):
        (OUT / f).chmod(0o755)
    # Servidores ya aprobados: así no sale el paso de la contraseña.
    (OUT / 'etc' / 'allowed-hosts').write_text('vpn.work.example\nvpn.uni.example\n')

    run('glib-compile-resources', '--sourcedir', str(ROOT / 'data'), '--generate-source',
        '--c-name', 'vpnportal', '--target', str(OUT / 'resources.c'),
        str(ROOT / 'data' / 'vpnportal.gresource.xml'))
    for lang in LANGS:
        if lang == 'en':
            continue
        mo = OUT / 'po' / lang / 'LC_MESSAGES'
        mo.mkdir(parents=True)
        run('msgfmt', '-o', str(mo / 'vpnportal.mo'), str(ROOT / 'po' / f'{lang}.po'))

    flags = subprocess.run(['pkg-config', '--cflags', '--libs', 'libadwaita-1', 'webkitgtk-6.0'],
                           check=True, capture_output=True, text=True).stdout.split()
    src = [str(ROOT / 'src' / f) for f in
           ('main.c', 'auth.c', 'config.c', 'login.c', 'theme.c', 'tray.c', 'vpn.c')]
    run('gcc', '-std=c11', '-O1', '-o', str(OUT / 'vpnportal-shots'), *src,
        str(OUT / 'resources.c'), str(ROOT / 'tools' / 'snapshot.c'),
        f'-DVPNPORTAL_APP_ID="{APP_ID}"',
        f'-DHELPER_PATH="{OUT}/vpnportal-helper"',
        f'-DALLOWLIST_PATH="{OUT}/etc/allowed-hosts"',
        f'-DTUN_PATH="{OUT}/tun0"',
        f'-DCONFIG_FILE_PATH="{OUT}/config/vpns.ini"',
        f'-DWEBKIT_DIR="{OUT}/webkit"', *flags)
    gio = subprocess.run(['pkg-config', '--cflags', '--libs', 'gio-2.0'],
                         check=True, capture_output=True, text=True).stdout.split()
    run('gcc', '-std=c11', '-o', str(OUT / 'vpnportal-auth'),
        str(ROOT / 'src' / 'vpnportal-auth.c'), f'-DVPNPORTAL_APP_ID="{APP_ID}"', *gio)


# ------------------------------------------------------------------
# La página de inicio de sesión de mentira
# ------------------------------------------------------------------

def idp_page(texts):
    org, title, subtitle, user, password, button = texts
    return f'''<!doctype html><html><head><meta charset="utf-8"><style>
body {{ margin: 0; height: 100vh; display: flex; align-items: center; justify-content: center;
       font-family: Cantarell, system-ui, sans-serif;
       background: linear-gradient(160deg, #1c71d8, #613583); }}
.card {{ background: #fff; border-radius: 14px; padding: 30px 28px; width: 290px;
        box-shadow: 0 10px 30px rgba(0, 0, 0, .25); }}
.org {{ font-weight: 700; color: #1c71d8; margin-bottom: 18px; }}
h1 {{ font-size: 21px; margin: 0 0 4px; color: #222; }}
p {{ color: #666; margin: 0 0 18px; font-size: 14px; }}
input {{ width: 100%; box-sizing: border-box; padding: 10px 12px; margin: 5px 0 12px;
        border: 1px solid #ccc; border-radius: 8px; font-size: 14px; }}
button {{ width: 100%; padding: 11px; border: 0; border-radius: 8px; background: #1c71d8;
         color: #fff; font-size: 15px; margin-top: 6px; }}
</style></head><body><div class="card"><div class="org">{org}</div><h1>{title}</h1>
<p>{subtitle}</p><input placeholder="{user}"><input type="password" placeholder="{password}">
<button>{button}</button></div></body></html>'''


def start_idp():
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_GET(self):
            lang = self.path.rsplit('=', 1)[-1] if '=' in self.path else 'en'
            body = idp_page(LANGS.get(lang, LANGS['en'])['idp']).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.end_headers()
            self.wfile.write(body)

    server = http.server.HTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server.server_address[1]


# ------------------------------------------------------------------
# Manejar la app por D-Bus
# ------------------------------------------------------------------

def call(dest, path, iface, method, args=None, reply=None):
    result = bus.call_sync(dest, path, iface, method, args,
                           GLib.VariantType(reply) if reply else None,
                           Gio.DBusCallFlags.NONE, 5000, None)
    return result.unpack() if result else None


def activate_action(name, param=None):
    call(APP_ID, APP_PATH, 'org.gtk.Actions', 'Activate',
         GLib.Variant('(sava{sv})', (name, [param] if param else [], {})))


class Tray:
    """El menú del icono de la barra, igual que lo usa GNOME."""

    def __init__(self, pid):
        self.dest = f'org.kde.StatusNotifierItem-{pid}-1'

    def icon(self):
        return call(self.dest, '/StatusNotifierItem', 'org.freedesktop.DBus.Properties', 'Get',
                    GLib.Variant('(ss)', ('org.kde.StatusNotifierItem', 'IconName')), '(v)')[0]

    def click(self, label):
        _, (_, _, children) = call(self.dest, '/MenuBar', 'com.canonical.dbusmenu', 'GetLayout',
                                   GLib.Variant('(iias)', (0, -1, [])), '(u(ia{sv}av))')
        item = next(cid for cid, props, _ in children if props.get('label') == label)
        call(self.dest, '/MenuBar', 'com.canonical.dbusmenu', 'Event',
             GLib.Variant('(isvu)', (item, 'clicked', GLib.Variant('i', 0), 0)))

    def wait_icon(self, name, timeout=15):
        end = time.time() + timeout
        while time.time() < end:
            if self.icon() == name:
                return
            time.sleep(0.2)
        sys.exit(f'timeout esperando el icono {name}')


def snapshot(proc, target):
    """Pide una captura (SIGUSR2) y espera a que aparezca el PNG."""
    tmp = OUT / 'snapshot.png'
    tmp.unlink(missing_ok=True)
    for _ in range(3):
        proc.send_signal(signal.SIGUSR2)
        for _ in range(20):
            time.sleep(0.15)
            if tmp.exists() and tmp.stat().st_size > 0:
                time.sleep(0.2)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.move(tmp, target)
                return
    sys.exit(f'no se pudo capturar {target}')


def side_by_side(left, right, target, gap=24):
    a = GdkPixbuf.Pixbuf.new_from_file(str(left))
    b = GdkPixbuf.Pixbuf.new_from_file(str(right))
    out = GdkPixbuf.Pixbuf.new(GdkPixbuf.Colorspace.RGB, True, 8,
                               a.get_width() + gap + b.get_width(),
                               max(a.get_height(), b.get_height()))
    out.fill(0x00000000)   # transparente
    a.copy_area(0, 0, a.get_width(), a.get_height(), out, 0, 0)
    b.copy_area(0, 0, b.get_width(), b.get_height(), out, a.get_width() + gap, 0)
    out.savev(str(target), 'png', [], [])


def shoot(lang, idp_port):
    names = LANGS[lang]
    config = OUT / 'config'
    shutil.rmtree(config, ignore_errors=True)
    config.mkdir()
    (config / 'vpns.ini').write_text(
        f'[work]\nname={names["work"]}\nportal=vpn.work.example\ngateway=\n'
        f'user=me@work.example\nhip=true\n\n'
        f'[uni]\nname={names["uni"]}\nportal=vpn.uni.example\ngateway=\nuser=\nhip=false\n')
    (config / 'settings.ini').write_text(
        f'[app]\ntheme=system\nlanguage={lang}\nclose-hint-shown=true\n')

    env = dict(os.environ,
               PATH=f'{OUT}/bin:{os.environ["PATH"]}',
               SNAPSHOT_PATH=str(OUT / 'snapshot.png'),
               MOCK_IDP_URL=f'http://127.0.0.1:{idp_port}/login?lang={lang}')
    log = open(OUT / f'app-{lang}.log', 'w')   # por si algo falla
    proc = subprocess.Popen([str(OUT / 'vpnportal-shots')], env=env,
                            stdout=log, stderr=subprocess.STDOUT)
    try:
        time.sleep(3)
        tray = Tray(proc.pid)
        target = SHOTS / lang

        # 1. Conectada, con su actividad.
        tray.click(_('Connect %s', lang) % names['work'])
        tray.wait_icon('network-vpn-symbolic')
        time.sleep(1)
        snapshot(proc, target / 'main.png')

        # 2. El login dentro de la app (otra VPN, que pide iniciar sesión).
        tray.click(_('Disconnect %s', lang) % names['work'])
        tray.wait_icon('network-vpn-disconnected-symbolic')
        tray.click(_('Connect %s', lang) % names['uni'])
        time.sleep(4.5)   # el login enseña la página a los 2 s; que cargue
        snapshot(proc, target / 'login.png')
        tray.click(_('Disconnect %s', lang) % names['uni'])
        tray.wait_icon('network-vpn-disconnected-symbolic')
        time.sleep(1)

        # 3. Temas (con una VPN conectada: se ven más botones del tema).
        tray.click(_('Connect %s', lang) % names['work'])
        tray.wait_icon('network-vpn-symbolic')
        time.sleep(1)
        for theme in ('aero', 'neon'):
            activate_action('theme', GLib.Variant('s', theme))
            time.sleep(1)
            snapshot(proc, OUT / f'theme-{theme}.png')
        side_by_side(OUT / 'theme-aero.png', OUT / 'theme-neon.png', target / 'themes.png')

        # 4. Preferencias.
        tray.click(_('Disconnect %s', lang) % names['work'])
        tray.wait_icon('network-vpn-disconnected-symbolic')
        activate_action('theme', GLib.Variant('s', 'system'))
        activate_action('preferences')
        time.sleep(2)
        snapshot(proc, target / 'preferences.png')
    finally:
        try:
            activate_action('quit')
            proc.wait(timeout=10)
        except Exception:
            proc.kill()


def _(text, lang):
    """Los textos del menú de la barra, traducidos como los traduciría la app."""
    if lang == 'en':
        return text
    env = dict(os.environ, LANGUAGE=lang, TEXTDOMAINDIR=str(OUT / 'po'))
    return subprocess.run(['gettext', '-d', 'vpnportal', text], env=env,
                          capture_output=True, text=True).stdout or text


if __name__ == '__main__':
    print('Compilando…')
    build()
    port = start_idp()
    for lang in LANGS:
        print(f'Capturas en {lang}…')
        shoot(lang, port)
    print(f'Listo: {SHOTS}')
