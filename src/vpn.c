/*
 * vpn.c — lanza gpclient (a través del helper con sudo), lee lo que
 * imprime y detecta cuándo está conectado.
 *
 * TODO es asíncrono: nunca nos quedamos esperando ("bloqueados") a que
 * el proceso escriba o termine, porque mientras tanto la ventana se
 * congelaría. En su lugar decimos "cuando haya una línea / cuando
 * termine, llama a esta función" y volvemos al bucle de eventos.
 *
 * Conectar tiene dos pasos:
 *   1. Si el portal (o la gateway) no está aprobado todavía, se aprueba
 *      con "pkexec vpnportal-helper allow ..." -> GNOME pide la contraseña.
 *      Solo pasa la primera vez para cada servidor.
 *   2. "sudo -n vpnportal-helper connect ..." -> sin contraseña, pero el
 *      helper solo acepta servidores aprobados en el paso 1.
 */
#include "vpn.h"

#include <signal.h>
#include <stdarg.h>

/* Los #ifndef permiten sustituir estas rutas al compilar una versión
 * de pruebas (gcc -DHELPER_PATH=...), sin tocar el código. */
#ifndef HELPER_PATH
#define HELPER_PATH "/usr/local/sbin/vpnportal-helper"
#endif

/* Lista de servidores aprobados. La escribe el helper (como root); la
 * app solo la lee para saber si hace falta pedir aprobación. */
#ifndef ALLOWLIST_PATH
#define ALLOWLIST_PATH "/etc/vpnportal/allowed-hosts"
#endif

/* Cuando gpclient conecta, aparece el interfaz de red del túnel. */
#ifndef TUN_PATH
#define TUN_PATH "/sys/class/net/tun0"
#endif

/* Segundos que esperamos tras Ctrl+C antes de insistir con SIGTERM. */
#define DISCONNECT_TIMEOUT 10

struct _Vpn {
  const VpnConfig  *config;
  VpnState          state;

  VpnStateFunc      on_state;
  VpnLineFunc       on_line;
  gpointer          user_data;

  GSubprocess      *approve_proc; /* pkexec ... allow (paso 1), o NULL */
  GSubprocess      *proc;         /* sudo ... connect (paso 2), o NULL */
  GDataInputStream *out;          /* lector línea a línea de su salida */
  GCancellable     *cancellable;  /* para abortar lo pendiente al cerrar */
  guint             poll_id;      /* temporizador que mira el túnel */
  guint             kill_id;      /* temporizador de "desconexión forzada" */
};

/* ---------------------------------------------------------------- */
/* Pequeñas ayudas                                                  */
/* ---------------------------------------------------------------- */

static void
set_state (Vpn *vpn, VpnState state)
{
  if (vpn->state == state)
    return;
  vpn->state = state;
  vpn->on_state (vpn, state, vpn->user_data);
}

/* Mensaje NUESTRO para el registro (con "»" para distinguirlo de los
 * de gpclient). Funciona como printf gracias a los "..." (varargs). */
static void G_GNUC_PRINTF (2, 3)
emit_note (Vpn *vpn, const char *format, ...)
{
  va_list args;
  va_start (args, format);
  g_autofree char *text = g_strdup_vprintf (format, args);
  va_end (args);

  g_autofree char *line = g_strconcat ("» ", text, NULL);
  vpn->on_line (vpn, line, vpn->user_data);
}

/* Pasa al registro un bloque de texto, línea a línea. */
static void
emit_output (Vpn *vpn, const char *output)
{
  if (output == NULL)
    return;

  g_auto (GStrv) lines = g_strsplit (output, "\n", -1);
  for (char **line = lines; *line != NULL; line++)
    if (**line != '\0')
      vpn->on_line (vpn, *line, vpn->user_data);
}

/* Para y limpia todo lo de la conexión actual (menos el lector de
 * salida, que se suelta solo cuando llega al final; ver on_line_read). */
static void
stop_run (Vpn *vpn)
{
  /* g_clear_handle_id: si el id no es 0, llama a g_source_remove y lo
   * pone a 0. Así nunca quitamos dos veces el mismo temporizador. */
  g_clear_handle_id (&vpn->poll_id, g_source_remove);
  g_clear_handle_id (&vpn->kill_id, g_source_remove);
  /* g_clear_object: unref + poner el puntero a NULL. */
  g_clear_object (&vpn->proc);
}

/* ---------------------------------------------------------------- */
/* Paso 2: la conexión                                              */
/* ---------------------------------------------------------------- */

static void read_next_line (Vpn *vpn);

/* Llega una línea de la salida de gpclient (o el final de la salida). */
static void
on_line_read (GObject *source, GAsyncResult *result, gpointer user_data)
{
  GDataInputStream *stream = G_DATA_INPUT_STREAM (source);
  g_autoptr (GError) error = NULL;

  /* g_autofree / g_autoptr: GLib libera la variable automáticamente al
   * salir de la función. Ahorra muchos free() y olvidos. */
  g_autofree char *raw = g_data_input_stream_read_line_finish (stream, result,
                                                               NULL, &error);

  /* Si se canceló, la app se está cerrando: 'user_data' (el Vpn) puede
   * estar ya liberado, así que NO lo tocamos. Por eso se mira lo primero. */
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;

  Vpn *vpn = user_data;

  /* Lector de una conexión anterior que ya no es la actual: ignorar. */
  if (stream != vpn->out)
    return;

  if (raw == NULL) {
    /* Fin de la salida (el proceso cerró su extremo de la tubería). */
    g_clear_object (&vpn->out);
    return;
  }

  /* gpclient debería escribir UTF-8, pero por si acaso lo saneamos:
   * GTK se queja si le das texto que no es UTF-8 válido. */
  g_autofree char *line = g_utf8_make_valid (raw, -1);
  vpn->on_line (vpn, line, vpn->user_data);

  /* Si quien se queja es sudo, casi seguro falta la regla de sudoers. */
  if (g_str_has_prefix (line, "sudo: "))
    emit_note (vpn, "sudo pide contraseña: ¿está instalado "
                    "system/vpnportal.sudoers? (ver system/README.md)");

  read_next_line (vpn);   /* y a por la siguiente */
}

static void
read_next_line (Vpn *vpn)
{
  g_data_input_stream_read_line_async (vpn->out, G_PRIORITY_DEFAULT,
                                       vpn->cancellable, on_line_read, vpn);
}

/* El proceso ha terminado (por Ctrl+C, por error o porque falló sudo). */
static void
on_process_exited (GObject *source, GAsyncResult *result, gpointer user_data)
{
  GSubprocess *proc = G_SUBPROCESS (source);
  g_autoptr (GError) error = NULL;

  if (!g_subprocess_wait_finish (proc, result, &error) &&
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;   /* app cerrándose: no tocar el Vpn (ver on_line_read) */

  Vpn *vpn = user_data;
  if (proc != vpn->proc)
    return;

  if (g_subprocess_get_if_exited (proc))
    emit_note (vpn, "gpclient ha terminado (código %d)",
               g_subprocess_get_exit_status (proc));
  else if (g_subprocess_get_if_signaled (proc))
    emit_note (vpn, "gpclient ha terminado por la señal %d",
               g_subprocess_get_term_sig (proc));

  stop_run (vpn);
  set_state (vpn, VPN_DISCONNECTED);
}

/* Cada segundo: ¿existe ya el interfaz del túnel? */
static gboolean
poll_tunnel (gpointer user_data)
{
  Vpn *vpn = user_data;
  gboolean tun_up = g_file_test (TUN_PATH, G_FILE_TEST_EXISTS);

  if (vpn->state == VPN_CONNECTING && tun_up)
    set_state (vpn, VPN_CONNECTED);
  else if (vpn->state == VPN_CONNECTED && !tun_up)
    set_state (vpn, VPN_CONNECTING);   /* se cayó; gpclient reintenta */

  return G_SOURCE_CONTINUE;   /* = seguir llamándome */
}

/* Han pasado DISCONNECT_TIMEOUT segundos tras Ctrl+C y sigue vivo. */
static gboolean
force_disconnect (gpointer user_data)
{
  Vpn *vpn = user_data;

  vpn->kill_id = 0;   /* este temporizador ya no existirá al volver */
  if (vpn->proc != NULL) {
    emit_note (vpn, "No responde a Ctrl+C; enviando SIGTERM");
    g_subprocess_send_signal (vpn->proc, SIGTERM);
  }
  return G_SOURCE_REMOVE;   /* = no volver a llamarme */
}

static void
start_connection (Vpn *vpn)
{
  const VpnConfig *config = vpn->config;
  g_autoptr (GError) error = NULL;

  /*
   * Montamos la orden pieza a pieza, por ejemplo:
   *   sudo -n vpnportal-helper connect vpn.empresa.example --user yo --hip
   *   -n = "non-interactive": si sudo necesitara contraseña, falla al
   *        momento en vez de quedarse esperando un teclado que no hay.
   * Cada dato va en su propio argumento (nunca se junta en un texto que
   * luego interprete una shell), así un valor raro no puede "romper" la
   * orden. Aun así, el helper vuelve a comprobarlo todo.
   */
  g_autoptr (GStrvBuilder) builder = g_strv_builder_new ();
  g_strv_builder_add_many (builder, "sudo", "-n", HELPER_PATH, "connect",
                           config->portal, NULL);
  if (*config->gateway != '\0')
    g_strv_builder_add_many (builder, "--gateway", config->gateway, NULL);
  if (*config->user != '\0')
    g_strv_builder_add_many (builder, "--user", config->user, NULL);
  if (config->hip)
    g_strv_builder_add (builder, "--hip");
  g_auto (GStrv) argv = g_strv_builder_end (builder);

  g_autofree char *command = g_strjoinv (" ", argv);
  emit_note (vpn, "Lanzando: %s", command);

  /* Flags: queremos LEER su salida (STDOUT_PIPE) y que los errores
   * (stderr) vayan por la misma tubería (STDERR_MERGE). La entrada
   * estándar queda en /dev/null: si gpclient preguntara algo por
   * teclado, falla en lugar de colgarse. */
  vpn->proc = g_subprocess_newv ((const char * const *) argv,
                                 G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                 G_SUBPROCESS_FLAGS_STDERR_MERGE,
                                 &error);
  if (vpn->proc == NULL) {
    emit_note (vpn, "No se pudo lanzar sudo: %s", error->message);
    set_state (vpn, VPN_DISCONNECTED);
    return;
  }

  set_state (vpn, VPN_CONNECTING);

  /* Si quedaba un lector de una conexión anterior, lo soltamos; su
   * lectura pendiente verá que ya no es vpn->out y se parará sola. */
  g_clear_object (&vpn->out);
  vpn->out = g_data_input_stream_new (g_subprocess_get_stdout_pipe (vpn->proc));
  read_next_line (vpn);

  g_subprocess_wait_async (vpn->proc, vpn->cancellable,
                           on_process_exited, vpn);
  vpn->poll_id = g_timeout_add_seconds (1, poll_tunnel, vpn);
}

/* ---------------------------------------------------------------- */
/* Paso 1: aprobación de hosts                                      */
/* ---------------------------------------------------------------- */

/* ¿Está 'host' en la lista de aprobados? (Solo para no pedir permiso
 * de más; quien de verdad lo hace cumplir es el helper.) */
static gboolean
host_is_approved (const char *host)
{
  g_autofree char *contents = NULL;

  if (!g_file_get_contents (ALLOWLIST_PATH, &contents, NULL, NULL))
    return FALSE;   /* aún no existe: no hay nada aprobado */

  g_auto (GStrv) lines = g_strsplit (contents, "\n", -1);
  for (char **line = lines; *line != NULL; line++)
    if (g_ascii_strcasecmp (g_strstrip (*line), host) == 0)
      return TRUE;
  return FALSE;
}

/* pkexec ha terminado: contraseña correcta, cancelada o fallida. */
static void
on_approval_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  GSubprocess *proc = G_SUBPROCESS (source);
  g_autoptr (GError) error = NULL;
  g_autofree char *output = NULL;

  if (!g_subprocess_communicate_utf8_finish (proc, result, &output, NULL,
                                             &error) &&
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;   /* app cerrándose: no tocar el Vpn */

  Vpn *vpn = user_data;
  if (proc != vpn->approve_proc)
    return;

  emit_output (vpn, output);

  gboolean approved = g_subprocess_get_if_exited (proc) &&
                      g_subprocess_get_exit_status (proc) == 0;
  g_clear_object (&vpn->approve_proc);

  /* Si mientras tanto le diste a Desconectar, el estado ya no es
   * CONNECTING y no seguimos aunque se haya aprobado. */
  if (approved && vpn->state == VPN_CONNECTING) {
    start_connection (vpn);
    return;
  }

  if (!approved)
    emit_note (vpn, "No se ha aprobado (¿se canceló la contraseña?)");
  set_state (vpn, VPN_DISCONNECTED);
}

static void
start_approval (Vpn *vpn, const char * const *hosts)
{
  g_autoptr (GError) error = NULL;

  /* pkexec es el "sudo gráfico" de GNOME: muestra la ventana de
   * contraseña de siempre y ejecuta el helper como root. */
  g_autoptr (GStrvBuilder) builder = g_strv_builder_new ();
  g_strv_builder_add_many (builder, "pkexec", HELPER_PATH, "allow", NULL);
  g_strv_builder_addv (builder, (const char **) hosts);
  g_auto (GStrv) argv = g_strv_builder_end (builder);

  g_autofree char *list = g_strjoinv (", ", (char **) hosts);
  emit_note (vpn, "Primera conexión a %s: hay que aprobarlo una vez "
                  "(te pedirá la contraseña)", list);

  vpn->approve_proc = g_subprocess_newv ((const char * const *) argv,
                                         G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                         G_SUBPROCESS_FLAGS_STDERR_MERGE,
                                         &error);
  if (vpn->approve_proc == NULL) {
    emit_note (vpn, "No se pudo lanzar pkexec: %s", error->message);
    return;
  }

  set_state (vpn, VPN_CONNECTING);

  /* communicate = "espera a que termine y dame todo lo que imprimió". */
  g_subprocess_communicate_utf8_async (vpn->approve_proc, NULL,
                                       vpn->cancellable,
                                       on_approval_done, vpn);
}

/* ---------------------------------------------------------------- */
/* API pública                                                      */
/* ---------------------------------------------------------------- */

Vpn *
vpn_new (const VpnConfig *config,
         VpnStateFunc     on_state,
         VpnLineFunc      on_line,
         gpointer         user_data)
{
  /* g_new0: malloc + poner todo a cero (punteros NULL, ids 0...). */
  Vpn *vpn = g_new0 (Vpn, 1);
  vpn->config = config;
  vpn->state = VPN_DISCONNECTED;
  vpn->on_state = on_state;
  vpn->on_line = on_line;
  vpn->user_data = user_data;
  vpn->cancellable = g_cancellable_new ();
  return vpn;
}

void
vpn_free (Vpn *vpn)
{
  if (vpn == NULL)
    return;

  /* Si cerramos la app con la VPN puesta, la desconectamos. */
  if (vpn->proc != NULL)
    g_subprocess_send_signal (vpn->proc, SIGINT);
  if (vpn->approve_proc != NULL)
    g_subprocess_send_signal (vpn->approve_proc, SIGTERM);

  /* Las operaciones pendientes recibirán "cancelado" y no tocarán vpn. */
  g_cancellable_cancel (vpn->cancellable);
  stop_run (vpn);
  g_clear_object (&vpn->approve_proc);
  g_clear_object (&vpn->out);
  g_clear_object (&vpn->cancellable);
  g_free (vpn);
}

const VpnConfig *
vpn_get_config (Vpn *vpn)
{
  return vpn->config;
}

VpnState
vpn_get_state (Vpn *vpn)
{
  return vpn->state;
}

void
vpn_connect (Vpn *vpn)
{
  if (vpn->state != VPN_DISCONNECTED)
    return;

  /* gpclient solo admite una instancia a la vez; si ya hay túnel,
   * es que hay otra VPN puesta (quizá desde la terminal). */
  if (g_file_test (TUN_PATH, G_FILE_TEST_EXISTS)) {
    emit_note (vpn, "Ya hay un túnel VPN activo (%s). Desconéctalo antes.",
               TUN_PATH);
    return;
  }

  /* ¿Qué servidores de esta VPN faltan por aprobar? Como mucho dos
   * (portal y gateway) + el NULL que marca el final de la lista. */
  const char *pending[3];
  guint n = 0;
  if (!host_is_approved (vpn->config->portal))
    pending[n++] = vpn->config->portal;
  if (*vpn->config->gateway != '\0' &&
      !host_is_approved (vpn->config->gateway))
    pending[n++] = vpn->config->gateway;
  pending[n] = NULL;

  if (n > 0)
    start_approval (vpn, pending);
  else
    start_connection (vpn);
}

void
vpn_disconnect (Vpn *vpn)
{
  if (vpn->state == VPN_DISCONNECTING)
    return;

  /* Aún en el paso 1 (ventana de contraseña abierta): lo cancelamos. */
  if (vpn->approve_proc != NULL) {
    set_state (vpn, VPN_DISCONNECTING);
    g_subprocess_send_signal (vpn->approve_proc, SIGTERM);
    return;
  }

  if (vpn->proc == NULL)
    return;

  /*
   * SIGINT es exactamente lo que manda Ctrl+C. Se lo enviamos a sudo
   * (nuestro hijo directo, que es "nuestro" y podemos señalizar), y sudo
   * lo reenvía a gpclient, que corre como root.
   */
  emit_note (vpn, "Desconectando (Ctrl+C)…");
  set_state (vpn, VPN_DISCONNECTING);
  g_subprocess_send_signal (vpn->proc, SIGINT);
  vpn->kill_id = g_timeout_add_seconds (DISCONNECT_TIMEOUT,
                                        force_disconnect, vpn);
}

const char *
vpn_state_to_string (VpnState state)
{
  switch (state) {
  case VPN_DISCONNECTED:  return "Desconectada";
  case VPN_CONNECTING:    return "Conectando…";
  case VPN_CONNECTED:     return "Conectada";
  case VPN_DISCONNECTING: return "Desconectando…";
  }
  return "?";
}
