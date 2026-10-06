# Ficheros de sistema

La app lanza `sudo -n /usr/local/sbin/gp-vpn-helper empresa|uni`. Para que
funcione sin pedir contraseña hay que instalar dos cosas (una sola vez, y
otra vez si cambias el helper):

```bash
# 1. El helper, propiedad de root (así nadie más puede modificarlo)
sudo install -o root -g root -m 0755 system/gp-vpn-helper /usr/local/sbin/gp-vpn-helper

# 2. La regla de sudo: primero se COMPRUEBA la sintaxis, y solo si está bien se instala
sudo visudo -cf system/gp-vpn.sudoers && sudo install -o root -g root -m 0440 system/gp-vpn.sudoers /etc/sudoers.d/gp-vpn
```

Para comprobarlo: `sudo -n -l` debería listar las dos órdenes del helper.

Para deshacerlo: `sudo rm /etc/sudoers.d/gp-vpn /usr/local/sbin/gp-vpn-helper`.
