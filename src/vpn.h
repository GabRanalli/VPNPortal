/*
 * vpn.h — la "interfaz pública" del módulo que gestiona una VPN.
 *
 * En C, un .h dice QUÉ se puede usar desde fuera y el .c dice CÓMO está
 * hecho. main.c solo incluye este fichero: no sabe (ni le importa) que
 * por debajo hay un proceso, tuberías y temporizadores.
 */
#pragma once

#include <gio/gio.h>

#include "config.h"

typedef enum {
  VPN_DISCONNECTED,
  VPN_CONNECTING,     /* proceso lanzado, aún sin túnel (login, etc.) */
  VPN_CONNECTED,      /* existe el interfaz del túnel */
  VPN_DISCONNECTING,  /* le hemos mandado Ctrl+C y esperamos a que salga */
} VpnState;

/*
 * "Tipo opaco": declaramos que existe struct _Vpn pero NO sus campos.
 * Desde fuera solo se puede manejar un puntero Vpn* y usar las funciones
 * de abajo; nadie puede tocar los campos por error.
 */
typedef struct _Vpn Vpn;

/* Los "pasos" son el resumen para personas de lo que va pasando
 * ("Contactando con el portal…", "Conectada"...). BEGIN marca el primer
 * paso de un intento de conexión nuevo (la interfaz empieza la lista). */
typedef enum {
  VPN_STEP_BEGIN,
  VPN_STEP_PROGRESS,
  VPN_STEP_DONE,
  VPN_STEP_ERROR,
} VpnStepKind;

/*
 * Punteros a función: así el módulo avisa a quien lo usa (la interfaz)
 * sin conocerlo. "Cuando cambie el estado, llama a ESTA función tuya."
 * Es la misma idea que las señales de GTK, pero en C pelado.
 */
typedef void (*VpnStateFunc) (Vpn *vpn, VpnState state, gpointer user_data);
typedef void (*VpnLineFunc)  (Vpn *vpn, const char *line, gpointer user_data);
typedef void (*VpnStepFunc)  (Vpn *vpn, VpnStepKind kind, const char *text,
                              gpointer user_data);

/* 'config' no se copia: debe vivir al menos tanto como el Vpn. */
Vpn             *vpn_new             (const VpnConfig *config,
                                      VpnStateFunc     on_state,
                                      VpnLineFunc      on_line,
                                      VpnStepFunc      on_step,
                                      gpointer         user_data);
void             vpn_free            (Vpn *vpn);

const VpnConfig *vpn_get_config      (Vpn *vpn);
VpnState         vpn_get_state       (Vpn *vpn);

void             vpn_connect         (Vpn *vpn);
void             vpn_disconnect      (Vpn *vpn);

const char      *vpn_state_to_string (VpnState state);
