/*
 * vpn.h — la "interfaz pública" del módulo que gestiona una VPN.
 *
 * En C, un .h dice QUÉ se puede usar desde fuera y el .c dice CÓMO está
 * hecho. main.c solo incluye este fichero: no sabe (ni le importa) que
 * por debajo hay un proceso, tuberías y temporizadores.
 */
#pragma once

#include <gio/gio.h>

typedef enum {
  VPN_DISCONNECTED,
  VPN_CONNECTING,     /* proceso lanzado, aún sin túnel (login, etc.) */
  VPN_CONNECTED,      /* existe el interfaz del túnel */
  VPN_DISCONNECTING,  /* le hemos mandado Ctrl+C y esperamos a que salga */
} VpnState;

/* Datos fijos de cada VPN. 'id' es lo que recibe el helper con sudo. */
typedef struct {
  const char *id;
  const char *title;
  const char *subtitle;
} VpnInfo;

/*
 * "Tipo opaco": declaramos que existe struct _Vpn pero NO sus campos.
 * Desde fuera solo se puede manejar un puntero Vpn* y usar las funciones
 * de abajo; nadie puede tocar los campos por error.
 */
typedef struct _Vpn Vpn;

/*
 * Punteros a función: así el módulo avisa a quien lo usa (la interfaz)
 * sin conocerlo. "Cuando cambie el estado, llama a ESTA función tuya."
 * Es la misma idea que las señales de GTK, pero en C pelado.
 */
typedef void (*VpnStateFunc) (Vpn *vpn, VpnState state, gpointer user_data);
typedef void (*VpnLineFunc)  (Vpn *vpn, const char *line, gpointer user_data);

Vpn           *vpn_new             (const VpnInfo *info,
                                    VpnStateFunc   on_state,
                                    VpnLineFunc    on_line,
                                    gpointer       user_data);
void           vpn_free            (Vpn *vpn);

const VpnInfo *vpn_get_info        (Vpn *vpn);
VpnState       vpn_get_state       (Vpn *vpn);

void           vpn_connect         (Vpn *vpn);
void           vpn_disconnect      (Vpn *vpn);

const char    *vpn_state_to_string (VpnState state);
