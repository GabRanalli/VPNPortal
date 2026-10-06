# Ficheros de sistema

gpclient necesita root. La app nunca lo lanza directamente: pasa siempre por
`vpnportal-helper`, que solo acepta un puñado de opciones y valida cada valor.

- **Conectar** (`vpnportal-helper connect ...`) no pide contraseña gracias a la
  regla de sudo, pero el helper solo conecta a servidores **aprobados**.
- **Aprobar** un servidor (`vpnportal-helper allow ...`) siempre pide contraseña.
  La app lo hace sola (con `pkexec`) la primera vez que conectas a un portal o
  gateway nuevo. Los aprobados se guardan en `/etc/vpnportal/allowed-hosts`.

Así, un programa malicioso que corra con tu usuario no puede conectarte a un
servidor suyo sin conocer tu contraseña.

Para el **login dentro de la app**, el helper le dice a gpclient que use
`/usr/local/libexec/vpnportal-auth` en vez de su programa de login (`gpauth`).
`vpnportal-auth` le pide el login a la app por D-Bus y le devuelve el resultado a
gpclient. Si no está instalado, gpclient abre la ventana de gpauth como siempre.

## Instalar (una vez, y de nuevo si cambia el helper o vpnportal-auth)

```bash
# 1. El helper, propiedad de root (así nadie más puede modificarlo)
sudo install -o root -g root -m 0755 system/vpnportal-helper /usr/local/sbin/vpnportal-helper

# 2. El sustituto de gpauth (se compila con "meson compile -C build")
sudo install -o root -g root -m 0755 build/vpnportal-auth /usr/local/libexec/vpnportal-auth

# 3. La regla de sudo: primero se COMPRUEBA la sintaxis, y solo si está bien se instala
sudo visudo -cf system/vpnportal.sudoers && sudo install -o root -g root -m 0440 system/vpnportal.sudoers /etc/sudoers.d/vpnportal
```

La regla es para el grupo `sudo` (Debian/Ubuntu). En Fedora o Arch cambia
`%sudo` por `%wheel` en `vpnportal.sudoers` antes de instalarla.

Para comprobarlo: `sudo -n -l` debería listar `vpnportal-helper connect *`.

## Ver o quitar servidores aprobados

```bash
cat /etc/vpnportal/allowed-hosts                 # ver
sudoedit /etc/vpnportal/allowed-hosts            # quitar alguno
```

## Desinstalar

```bash
sudo rm -r /etc/sudoers.d/vpnportal /usr/local/sbin/vpnportal-helper /usr/local/libexec/vpnportal-auth /etc/vpnportal
```
