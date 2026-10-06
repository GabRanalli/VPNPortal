# Ficheros de sistema

gpclient necesita root. La app nunca lo lanza directamente: pasa siempre por
`gp-vpn-helper`, que solo acepta un puñado de opciones y valida cada valor.

- **Conectar** (`gp-vpn-helper connect ...`) no pide contraseña gracias a la
  regla de sudo, pero el helper solo conecta a servidores **aprobados**.
- **Aprobar** un servidor (`gp-vpn-helper allow ...`) siempre pide contraseña.
  La app lo hace sola (con `pkexec`) la primera vez que conectas a un portal o
  gateway nuevo. Los aprobados se guardan en `/etc/gp-vpn/allowed-hosts`.

Así, un programa malicioso que corra con tu usuario no puede conectarte a un
servidor suyo sin conocer tu contraseña.

## Instalar (una vez, y de nuevo si cambia el helper)

```bash
# 1. El helper, propiedad de root (así nadie más puede modificarlo)
sudo install -o root -g root -m 0755 system/gp-vpn-helper /usr/local/sbin/gp-vpn-helper

# 2. La regla de sudo: primero se COMPRUEBA la sintaxis, y solo si está bien se instala
sudo visudo -cf system/gp-vpn.sudoers && sudo install -o root -g root -m 0440 system/gp-vpn.sudoers /etc/sudoers.d/gp-vpn
```

La regla es para el grupo `sudo` (Debian/Ubuntu). En Fedora o Arch cambia
`%sudo` por `%wheel` en `gp-vpn.sudoers` antes de instalarla.

Para comprobarlo: `sudo -n -l` debería listar `gp-vpn-helper connect *`.

## Ver o quitar servidores aprobados

```bash
cat /etc/gp-vpn/allowed-hosts                 # ver
sudoedit /etc/gp-vpn/allowed-hosts            # quitar alguno
```

## Desinstalar

```bash
sudo rm -r /etc/sudoers.d/gp-vpn /usr/local/sbin/gp-vpn-helper /etc/gp-vpn
```
