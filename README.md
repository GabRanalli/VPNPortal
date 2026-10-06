# VPN Portal

*[Leer en español](README.es.md)*

A small GNOME app to manage your **GlobalProtect** VPNs on Linux, built on top
of [GlobalProtect-openconnect](https://github.com/yuezk/GlobalProtect-openconnect)
(`gpclient`).

- Add as many VPNs as you want (portal, gateway, user, HIP) and connect or
  disconnect each one with a click.
- **SAML login inside the app**: the identity provider's page (Microsoft,
  Okta, your university's SSO…) opens in a dialog in the app window instead
  of a separate window.
- **Remembers your session**: if your identity provider keeps you signed in,
  the next login happens on its own without showing anything.
- **Top bar icon** showing the current status, with a menu to connect or
  disconnect. Closing the window keeps the app running in the background.
- **No password to connect**, without giving away root (see
  [Security](#security)).

> **Status:** early, but usable day to day. The UI is in Spanish for now
> (English is planned). Tested on Ubuntu 26.04 with GNOME 50.

## Requirements

- [GlobalProtect-openconnect](https://github.com/yuezk/GlobalProtect-openconnect)
  2.x installed (`gpclient` in `/usr/bin`).
- GTK 4, libadwaita ≥ 1.6 and WebKitGTK 6.0. On Ubuntu/Debian:

  ```bash
  sudo apt install meson ninja-build pkg-config libadwaita-1-dev libwebkitgtk-6.0-dev
  ```
- For the top bar icon on GNOME, the
  [AppIndicator extension](https://extensions.gnome.org/extension/615/appindicator-support/)
  (enabled by default on Ubuntu). Without it the app still works, but closing
  the window quits it.

## Build

```bash
meson setup build
meson compile -C build
```

## Install the system files

`gpclient` needs root. The app never runs it directly: it always goes through
a small helper script with a strict allowlist of options. Install it once
(and again whenever it changes):

```bash
# 1. The helper, owned by root
sudo install -o root -g root -m 0755 system/vpnportal-helper /usr/local/sbin/vpnportal-helper

# 2. The in-app login bridge (replaces gpauth's window)
sudo install -o root -g root -m 0755 build/vpnportal-auth /usr/local/libexec/vpnportal-auth

# 3. The sudo rule: check the syntax first, install only if it is valid
sudo visudo -cf system/vpnportal.sudoers && sudo install -o root -g root -m 0440 system/vpnportal.sudoers /etc/sudoers.d/vpnportal
```

The sudo rule is for the `sudo` group (Debian/Ubuntu). On Fedora or Arch,
change `%sudo` to `%wheel` in `system/vpnportal.sudoers` first.

More details, and how to uninstall, in [system/README.md](system/README.md)
(Spanish).

## Usage

```bash
./build/vpnportal
```

Press **+** to add a VPN. The first time you connect to a new portal or
gateway, GNOME asks for your admin password once to approve it; after that,
connecting never asks again.

## How it works

```
VPN Portal ──sudo -n──> vpnportal-helper ──> gpclient (root)
     ^                                          │ needs a SAML login
     │ D-Bus                                    v
     └───────────────────────────────── vpnportal-auth (your user)
```

- The helper validates every argument and runs `gpclient connect`.
- When `gpclient` needs a SAML login, it launches its login program. The
  helper points it (`GP_AUTH_BINARY`) to `vpnportal-auth` instead of `gpauth`.
- `vpnportal-auth` forwards the request to the app over D-Bus. The app shows
  the login in an embedded WebKitGTK view, captures the credentials the
  portal returns (HTTP headers, HTML or `globalprotectcallback:` URL, the same
  way `gpauth` does) and sends them back to `gpclient`.

## Security

- The sudo rule only allows `vpnportal-helper connect *` without a password.
- `connect` only accepts a fixed set of options and validates every value.
  The dangerous `gpclient` options (`-s <script>`, `--hip <script>`,
  `--cookie-cache <path>`, certificates…) cannot be passed at all.
- `connect` only connects to **approved** servers
  (`/etc/vpnportal/allowed-hosts`). Approving a server (`allow`) always requires
  your password, so a malicious program running as your user cannot point
  your VPN to its own server.
- The app only answers login requests for a connection it started itself.

## Roadmap

A simpler log with a "show details" button, themes, an English UI and
starting automatically with the session. See
[TODO.md](TODO.md) (Spanish).

## License

[GPL-3.0-or-later](LICENSE).

This project is not affiliated with or endorsed by Palo Alto Networks.
GlobalProtect is a trademark of Palo Alto Networks, Inc.
