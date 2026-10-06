# VPN Portal

*[Read in English](README.md)*

Una pequeña app de GNOME para gestionar tus VPN de **GlobalProtect** en Linux,
construida sobre [GlobalProtect-openconnect](https://github.com/yuezk/GlobalProtect-openconnect)
(`gpclient`).

- Añade todas las VPN que quieras (portal, gateway, usuario, HIP) y conecta o
  desconecta cada una con un clic.
- **Login SAML dentro de la app**: la página de tu proveedor de identidad
  (Microsoft, Okta, el SSO de tu universidad…) se abre en un diálogo dentro de
  la ventana, no en una ventana aparte.
- **Recuerda tu sesión**: si tu proveedor de identidad te mantiene la sesión
  iniciada, el siguiente login pasa solo sin enseñar nada.
- **Icono en la barra superior** con el estado actual y un menú para conectar
  o desconectar. Al cerrar la ventana, la app sigue en segundo plano.
- **Conectar sin contraseña**, sin regalar root (ver [Seguridad](#seguridad)).

> **Estado:** en sus primeras versiones, pero ya sirve para el día a día. La
> interfaz está en español de momento (está previsto pasarla a inglés).
> Probada en Ubuntu 26.04 con GNOME 50.

## Requisitos

- [GlobalProtect-openconnect](https://github.com/yuezk/GlobalProtect-openconnect)
  2.x instalado (`gpclient` en `/usr/bin`).
- GTK 4, libadwaita ≥ 1.6, WebKitGTK 6.0 y libayatana-appindicator-glib.
  En Ubuntu/Debian:

  ```bash
  sudo apt install meson ninja-build pkg-config libadwaita-1-dev libwebkitgtk-6.0-dev libayatana-appindicator-glib-dev
  ```
- Para el icono de la barra superior en GNOME, la
  [extensión AppIndicator](https://extensions.gnome.org/extension/615/appindicator-support/)
  (viene activada en Ubuntu). Sin ella la app funciona, pero cerrar la ventana
  la cierra.

## Compilar

```bash
meson setup build
meson compile -C build
```

## Instalar los ficheros de sistema

`gpclient` necesita root. La app nunca lo lanza directamente: pasa siempre por
un pequeño script (el helper) que solo admite una lista cerrada de opciones.
Se instala una vez (y de nuevo cuando cambie):

```bash
# 1. El helper, propiedad de root
sudo install -o root -g root -m 0755 system/vpnportal-helper /usr/local/sbin/vpnportal-helper

# 2. El puente para el login dentro de la app (sustituye a la ventana de gpauth)
sudo install -o root -g root -m 0755 build/vpnportal-auth /usr/local/libexec/vpnportal-auth

# 3. La regla de sudo: primero se comprueba la sintaxis y solo si está bien se instala
sudo visudo -cf system/vpnportal.sudoers && sudo install -o root -g root -m 0440 system/vpnportal.sudoers /etc/sudoers.d/vpnportal
```

La regla es para el grupo `sudo` (Debian/Ubuntu). En Fedora o Arch, cambia
antes `%sudo` por `%wheel` en `system/vpnportal.sudoers`.

Más detalles, y cómo desinstalarlo, en [system/README.md](system/README.md).

## Uso

```bash
./build/vpnportal
```

Pulsa **+** para añadir una VPN. La primera vez que conectas a un portal o
gateway nuevo, GNOME te pide la contraseña de administrador una vez para
aprobarlo; a partir de ahí, conectar nunca la vuelve a pedir.

## Cómo funciona

```
VPN Portal ──sudo -n──> vpnportal-helper ──> gpclient (root)
     ^                                          │ necesita login SAML
     │ D-Bus                                    v
     └───────────────────────────────── vpnportal-auth (tu usuario)
```

- El helper valida cada argumento y lanza `gpclient connect`.
- Cuando `gpclient` necesita un login SAML, lanza su programa de login. El
  helper le indica (`GP_AUTH_BINARY`) que use `vpnportal-auth` en vez de
  `gpauth`.
- `vpnportal-auth` le pasa la petición a la app por D-Bus. La app muestra el
  login en un navegador embebido (WebKitGTK), captura las credenciales que
  devuelve el portal (cabeceras HTTP, HTML o URL `globalprotectcallback:`,
  igual que `gpauth`) y se las devuelve a `gpclient`.

## Seguridad

- La regla de sudo solo permite `vpnportal-helper connect *` sin contraseña.
- `connect` solo acepta un conjunto fijo de opciones y valida cada valor. Las
  opciones peligrosas de `gpclient` (`-s <script>`, `--hip <script>`,
  `--cookie-cache <ruta>`, certificados…) no se pueden pasar de ninguna forma.
- `connect` solo conecta a servidores **aprobados**
  (`/etc/vpnportal/allowed-hosts`). Aprobar un servidor (`allow`) siempre pide
  tu contraseña, así que un programa malicioso que corra con tu usuario no
  puede desviar tu VPN a un servidor suyo.
- La app solo atiende peticiones de login de conexiones que ha lanzado ella.

## Próximamente

Un registro más sencillo con un botón de "ver detalles", temas, interfaz en
inglés y arrancar sola al iniciar sesión. Ver [TODO.md](TODO.md).

## Licencia

[GPL-3.0 o posterior](LICENSE).

Este proyecto no está afiliado ni respaldado por Palo Alto Networks.
GlobalProtect es una marca registrada de Palo Alto Networks, Inc.
