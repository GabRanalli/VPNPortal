/*
 * tray.c — icono + menú en la barra superior.
 *
 * El menú es un GMenu (el mismo tipo de menú que usa GTK) y cada entrada
 * apunta a una ACCIÓN por su nombre ("indicator.toggle", ...). Cuando
 * pulsas una entrada, GLib busca esa acción y llama a su función. Es la
 * forma estándar en GTK de separar "qué se ve" (el menú) de "qué hace"
 * (la acción).
 */
#include "tray.h"

#include <ayatana-appindicator.h>

static AppIndicator       *indicator;
static GMenu              *menu;
static GSimpleActionGroup *actions;
static TrayCallbacks       callbacks;
static gpointer            callbacks_data;

/* ---------------------------------------------------------------- */
/* Acciones                                                         */
/* ---------------------------------------------------------------- */

/* "toggle" lleva un parámetro: el id de la VPN (un string, "s"). */
static void
on_toggle (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  (void) action; (void) user_data;
  callbacks.toggle (g_variant_get_string (parameter, NULL), callbacks_data);
}

static void
on_show (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  (void) action; (void) parameter; (void) user_data;
  callbacks.show (callbacks_data);
}

static void
on_quit (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  (void) action; (void) parameter; (void) user_data;
  callbacks.quit (callbacks_data);
}

/* ---------------------------------------------------------------- */
/* API                                                              */
/* ---------------------------------------------------------------- */

void
tray_init (const char          *app_id,
           const TrayCallbacks *tray_callbacks,
           gpointer             user_data)
{
  callbacks = *tray_callbacks;
  callbacks_data = user_data;

  /* GActionEntry: una tabla con nombre, función y tipo de parámetro de
   * cada acción. Más cómodo que crearlas una a una. */
  static const GActionEntry entries[] = {
    { .name = "toggle", .activate = on_toggle, .parameter_type = "s" },
    { .name = "show",   .activate = on_show },
    { .name = "quit",   .activate = on_quit },
  };
  actions = g_simple_action_group_new ();
  g_action_map_add_action_entries (G_ACTION_MAP (actions), entries,
                                   G_N_ELEMENTS (entries), NULL);

  menu = g_menu_new ();

  indicator = app_indicator_new (app_id, "network-vpn-disconnected-symbolic",
                                 APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
  app_indicator_set_title (indicator, "VPN Portal");
  app_indicator_set_actions (indicator, actions);
  app_indicator_set_menu (indicator, menu);
  /* Clic central (o el clic que tu escritorio use para esto): ventana. */
  app_indicator_set_secondary_activate_target (indicator, "show");
  app_indicator_set_status (indicator, APP_INDICATOR_STATUS_ACTIVE);

  tray_update (NULL, 0);
}

/* Rehace el menú y el icono según el estado de las VPN. */
void
tray_update (const TrayItem *items, guint n_items)
{
  if (indicator == NULL)
    return;

  /* Como mucho una VPN en uso (gpclient no admite más). */
  const TrayItem *active = NULL;
  for (guint i = 0; i < n_items; i++)
    if (items[i].state != VPN_DISCONNECTED)
      active = &items[i];

  /* El icono y el texto de estado. */
  const char *icon = "network-vpn-disconnected-symbolic";
  g_autofree char *status = NULL;
  if (active == NULL) {
    status = g_strdup ("Sin conexión");
  } else {
    icon = active->state == VPN_CONNECTED ? "network-vpn-symbolic"
                                          : "network-vpn-acquiring-symbolic";
    status = g_strdup_printf ("%s · %s", active->name,
                              vpn_state_to_string (active->state));
  }
  app_indicator_set_icon (indicator, icon, status);

  /* El menú, en tres secciones (se separan con una raya). Una entrada
   * sin acción (NULL) se muestra como texto que no se puede pulsar. */
  g_menu_remove_all (menu);

  g_autoptr (GMenu) status_section = g_menu_new ();
  g_menu_append (status_section, status, NULL);
  g_menu_append_section (menu, NULL, G_MENU_MODEL (status_section));

  g_autoptr (GMenu) vpn_section = g_menu_new ();
  if (active != NULL) {
    g_autofree char *label = g_strdup_printf ("Desconectar %s", active->name);
    g_autoptr (GMenuItem) item = g_menu_item_new (label, NULL);
    g_menu_item_set_action_and_target (item, "indicator.toggle", "s",
                                       active->id);
    g_menu_append_item (vpn_section, item);
  } else {
    for (guint i = 0; i < n_items; i++) {
      g_autofree char *label = g_strdup_printf ("Conectar %s", items[i].name);
      g_autoptr (GMenuItem) item = g_menu_item_new (label, NULL);
      g_menu_item_set_action_and_target (item, "indicator.toggle", "s",
                                         items[i].id);
      g_menu_append_item (vpn_section, item);
    }
    if (n_items == 0)
      g_menu_append (vpn_section, "Añade una VPN desde la ventana", NULL);
  }
  g_menu_append_section (menu, NULL, G_MENU_MODEL (vpn_section));

  g_autoptr (GMenu) app_section = g_menu_new ();
  g_menu_append (app_section, "Mostrar ventana", "indicator.show");
  g_menu_append (app_section, "Salir", "indicator.quit");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (app_section));
}

void
tray_shutdown (void)
{
  g_clear_object (&indicator);
  g_clear_object (&menu);
  g_clear_object (&actions);
}

/*
 * Los iconos de bandeja siguen el protocolo "StatusNotifier": quien los
 * pinta (en GNOME, la extensión AppIndicator) se registra en el bus con el
 * nombre org.kde.StatusNotifierWatcher. Si nadie tiene ese nombre, nuestro
 * icono no se vería en ningún sitio.
 */
gboolean
tray_is_available (void)
{
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SESSION,
                                                    NULL, NULL);
  if (bus == NULL)
    return FALSE;

  g_autoptr (GVariant) reply =
    g_dbus_connection_call_sync (bus, "org.freedesktop.DBus",
                                 "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                 "NameHasOwner",
                                 g_variant_new ("(s)",
                                                "org.kde.StatusNotifierWatcher"),
                                 G_VARIANT_TYPE ("(b)"), G_DBUS_CALL_FLAGS_NONE,
                                 1000, NULL, NULL);
  gboolean has_owner = FALSE;
  if (reply != NULL)
    g_variant_get (reply, "(b)", &has_owner);
  return has_owner;
}
