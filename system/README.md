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
`vpnportal-auth` (en `libexec`) en vez de su programa de login (`gpauth`).
`vpnportal-auth` le pide el login a la app por D-Bus y le devuelve el resultado a
gpclient. Si no está instalado, gpclient abre la ventana de gpauth como siempre.

## Los ficheros de esta carpeta

Son plantillas (`.in`): Meson rellena las marcas `@SBINDIR@`, `@LIBEXECDIR@` y
`@SUDO_GROUP@` con las rutas reales al configurar, y las instala con
`sudo meson install -C build`:

| Plantilla                | Se instala en (con `/usr/local`)        | Permisos   |
|--------------------------|-----------------------------------------|------------|
| `vpnportal-helper.in`    | `/usr/local/sbin/vpnportal-helper`      | `0755`     |
| `vpnportal.sudoers.in`   | `/etc/sudoers.d/vpnportal`              | `0440`     |

La regla de sudo se comprueba con `visudo -cf` al configurar: si no fuera
válida, Meson se detiene antes de instalar nada (un fichero de sudo roto
puede dejarte sin sudo).

Opciones (`meson setup build -D...`):

- `sudo_group` (por defecto `sudo`): el grupo de administradores. En Fedora o
  Arch, `-Dsudo_group=wheel`.
- `system_files` (por defecto `true`): con `false` no se instalan el helper ni
  la regla (la app no podrá conectar sin ellos).

Para comprobar la regla instalada: `sudo -n -l` debería listar
`vpnportal-helper connect *`.

## Ver o quitar servidores aprobados

```bash
cat /etc/vpnportal/allowed-hosts                 # ver
sudoedit /etc/vpnportal/allowed-hosts            # quitar alguno
```

## Desinstalar

```bash
sudo ninja -C build uninstall    # todo lo instalado
sudo rm -r /etc/vpnportal        # y, si quieres, la lista de aprobados
```
