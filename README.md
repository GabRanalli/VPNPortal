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
  disconnect. Closing the window keeps the app running in the background,
  and it can start automatically (hidden in the top bar) when you log in.
- **Clear activity view**: the steps of each connection in plain words
  ("Contacting the portal…", "Connected"), with the full `gpclient` log one
  click away (and a button to copy it).
- **Themes**: System, White, Black, Neon blue, Red and Frutiger Aero
  (sky, glass, bubbles and glossy buttons), in Preferences (Ctrl+,). You
  can also **make your own**: a theme is just a `.css` file, see
  [docs/THEMES.md](docs/THEMES.md).
- **English and Spanish**, following your system language or the one you
  choose in Preferences.
- **No password to connect**, without giving away root (see
  [Security](#security)).

> **Status:** early, but usable day to day. Available in English and
> Spanish. Tested on Ubuntu 26.04 with GNOME 50.

## Requirements

- [GlobalProtect-openconnect](https://github.com/yuezk/GlobalProtect-openconnect)
  2.x installed (`gpclient` in `/usr/bin`).
- GTK 4, libadwaita ≥ 1.6 and WebKitGTK 6.0. On Ubuntu/Debian:

  ```bash
  sudo apt install meson ninja-build pkg-config gettext libadwaita-1-dev libwebkitgtk-6.0-dev
  ```
- For the top bar icon on GNOME, the
  [AppIndicator extension](https://extensions.gnome.org/extension/615/appindicator-support/)
  (enabled by default on Ubuntu). Without it the app still works, but closing
  the window quits it.

## Build and install

```bash
meson setup build
meson compile -C build
sudo meson install -C build
```

This installs, under `/usr/local`: the app, its launcher and icon (it shows
up in GNOME's app grid), the translations, and the two system pieces it needs
to connect without a password: a small helper script and a sudo rule (see
[Security](#security)). The sudo rule is checked with `visudo` before
anything is installed.

The sudo rule is for the `sudo` group (Debian/Ubuntu). On Fedora or Arch,
configure with `meson setup build -Dsudo_group=wheel`.

To uninstall: `sudo ninja -C build uninstall`. More details in
[system/README.md](system/README.md) (Spanish).

## Usage

Open **VPN Portal** from the app grid (or run `vpnportal`) and press **+** to
add a VPN. The first time you connect to a new portal or
gateway, GNOME asks for your admin password once to approve it; after that,
connecting never asks again.

### Try it without a VPN

`tools/demo.sh` starts a separate demo copy of the app with two made-up VPNs
that "connect" without any network, login or GlobalProtect install (it only
needs the build dependencies). Your real configuration is not touched.

```bash
tools/demo.sh
```

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

More languages: translations live in [po/](po/). To add one, put its code
in `po/LINGUAS` and create `po/<code>.po` (for example with
`msginit -i po/vpnportal.pot -l fr`, after
`meson compile -C build vpnportal-pot`). See
[TODO.md](TODO.md) (Spanish).

## License

[GPL-3.0-or-later](LICENSE).

This project is not affiliated with or endorsed by Palo Alto Networks.
GlobalProtect is a trademark of Palo Alto Networks, Inc.
