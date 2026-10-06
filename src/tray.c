/*
 * tray.c — icono + menú en la barra superior, hablando D-Bus directamente.
 *
 * Los iconos de bandeja en Linux siguen dos protocolos de D-Bus:
 *
 *   - StatusNotifierItem (objeto /StatusNotifierItem): el icono en sí.
 *     Tiene propiedades (IconName, Title, Menu...) que quien pinta la
 *     barra lee, y señales (NewIcon...) para avisarle de cambios.
 *
 *   - DBusMenu (objeto /MenuBar, interfaz com.canonical.dbusmenu): el
 *     menú. La barra pide el "árbol" de entradas con GetLayout y, cuando
 *     pulsas una, nos llama a Event(id, "clicked").
 *
 * Para aparecer, nos apuntamos en org.kde.StatusNotifierWatcher (en GNOME
 * lo ofrece la extensión AppIndicator).
 *
 * (Antes usábamos libayatana-appindicator-glib, pero publica el menú en
 * otro formato, org.gtk.Menus, que la extensión de GNOME no entiende.)
 */
#include "tray.h"

#include <gio/gio.h>
#include <unistd.h>

#define ITEM_PATH "/StatusNotifierItem"
#define MENU_PATH "/MenuBar"
#define WATCHER_NAME "org.kde.StatusNotifierWatcher"

static const char introspection_xml[] =
  "<node>"
  "  <interface name='org.kde.StatusNotifierItem'>"
  "    <property name='Category' type='s' access='read'/>"
  "    <property name='Id' type='s' access='read'/>"
  "    <property name='Title' type='s' access='read'/>"
  "    <property name='Status' type='s' access='read'/>"
  "    <property name='IconName' type='s' access='read'/>"
  "    <property name='IconThemePath' type='s' access='read'/>"
  "    <property name='Menu' type='o' access='read'/>"
  "    <property name='ItemIsMenu' type='b' access='read'/>"
  "    <property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
  "    <method name='Activate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
  "    <method name='SecondaryActivate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
  "    <method name='ContextMenu'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
  "    <method name='Scroll'><arg type='i' direction='in'/><arg type='s' direction='in'/></method>"
  "    <signal name='NewIcon'/>"
  "    <signal name='NewTitle'/>"
  "    <signal name='NewToolTip'/>"
  "    <signal name='NewStatus'><arg type='s'/></signal>"
  "  </interface>"
  "  <interface name='com.canonical.dbusmenu'>"
  "    <property name='Version' type='u' access='read'/>"
  "    <property name='TextDirection' type='s' access='read'/>"
  "    <property name='Status' type='s' access='read'/>"
  "    <property name='IconThemePath' type='as' access='read'/>"
  "    <method name='GetLayout'>"
  "      <arg type='i' direction='in'/><arg type='i' direction='in'/>"
  "      <arg type='as' direction='in'/>"
  "      <arg type='u' direction='out'/><arg type='(ia{sv}av)' direction='out'/>"
  "    </method>"
  "    <method name='GetGroupProperties'>"
  "      <arg type='ai' direction='in'/><arg type='as' direction='in'/>"
  "      <arg type='a(ia{sv})' direction='out'/>"
  "    </method>"
  "    <method name='GetProperty'>"
  "      <arg type='i' direction='in'/><arg type='s' direction='in'/>"
  "      <arg type='v' direction='out'/>"
  "    </method>"
  "    <method name='Event'>"
  "      <arg type='i' direction='in'/><arg type='s' direction='in'/>"
  "      <arg type='v' direction='in'/><arg type='u' direction='in'/>"
  "    </method>"
  "    <method name='EventGroup'>"
  "      <arg type='a(isvu)' direction='in'/><arg type='ai' direction='out'/>"
  "    </method>"
  "    <method name='AboutToShow'>"
  "      <arg type='i' direction='in'/><arg type='b' direction='out'/>"
  "    </method>"
  "    <method name='AboutToShowGroup'>"
  "      <arg type='ai' direction='in'/>"
  "      <arg type='ai' direction='out'/><arg type='ai' direction='out'/>"
  "    </method>"
  "    <signal name='ItemsPropertiesUpdated'>"
  "      <arg type='a(ia{sv})'/><arg type='a(ias)'/>"
  "    </signal>"
  "    <signal name='LayoutUpdated'><arg type='u'/><arg type='i'/></signal>"
  "  </interface>"
  "</node>";

/* Una entrada del menú. Su id en D-Bus es su posición + 1 (el 0 es la
 * raíz, el "menú" que contiene a todas). */
typedef enum {
  ENTRY_TEXT,        /* solo texto (no se puede pulsar) */
  ENTRY_TOGGLE,      /* conectar/desconectar una VPN */
  ENTRY_SHOW,        /* mostrar la ventana */
  ENTRY_QUIT,        /* salir */
  ENTRY_SEPARATOR,   /* raya */
} EntryKind;

typedef struct {
  EntryKind  kind;
  char      *label;
  char      *vpn_id;   /* solo en ENTRY_TOGGLE */
} Entry;

static GDBusConnection *bus;
static GDBusNodeInfo   *node_info;
static guint            item_registration;
static guint            menu_registration;
static guint            name_owner_id;
static guint            watcher_watch_id;
static char            *bus_name;
static char            *app_id;

static GArray          *entries;      /* de Entry */
static guint32          revision;     /* sube cada vez que cambia el menú */
static char            *icon_name;
static char            *status_text;

static TrayCallbacks    callbacks;
static gpointer         callbacks_data;

/* ---------------------------------------------------------------- */
/* El menú                                                          */
/* ---------------------------------------------------------------- */

static void
entry_clear (gpointer data)
{
  Entry *entry = data;
  g_free (entry->label);
  g_free (entry->vpn_id);
}

static void
add_entry (EntryKind kind, const char *label, const char *vpn_id)
{
  Entry entry = { kind, NULL, g_strdup (vpn_id) };

  /* En DBusMenu, "_" marca la letra de atajo (como en "_Archivo"). Para
   * que un "_" del nombre de una VPN se vea tal cual, se escribe "__". */
  if (label != NULL) {
    g_auto (GStrv) parts = g_strsplit (label, "_", -1);
    entry.label = g_strjoinv ("__", parts);
  }
  g_array_append_val (entries, entry);
}

/* Las propiedades de una entrada, en el formato a{sv} de DBusMenu. */
static GVariant *
entry_properties (const Entry *entry)
{
  GVariantBuilder props;
  g_variant_builder_init (&props, G_VARIANT_TYPE ("a{sv}"));

  if (entry->kind == ENTRY_SEPARATOR) {
    g_variant_builder_add (&props, "{sv}", "type",
                           g_variant_new_string ("separator"));
  } else {
    g_variant_builder_add (&props, "{sv}", "label",
                           g_variant_new_string (entry->label));
    g_variant_builder_add (&props, "{sv}", "enabled",
                           g_variant_new_boolean (entry->kind != ENTRY_TEXT));
  }
  return g_variant_builder_end (&props);
}

/* Propiedades de la raíz (id 0): "tengo un submenú". */
static GVariant *
root_properties (void)
{
  GVariantBuilder props;
  g_variant_builder_init (&props, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&props, "{sv}", "children-display",
                         g_variant_new_string ("submenu"));
  return g_variant_builder_end (&props);
}

/* El árbol (id, propiedades, hijos) que pide GetLayout. Nuestro menú es
 * plano: la raíz tiene hijos y ellos ninguno. */
static GVariant *
build_layout (int parent_id, int depth)
{
  GVariantBuilder children;
  g_variant_builder_init (&children, G_VARIANT_TYPE ("av"));

  if (parent_id == 0) {
    if (depth != 0)
      for (guint i = 0; i < entries->len; i++) {
        const Entry *entry = &g_array_index (entries, Entry, i);
        GVariant *child = g_variant_new ("(i@a{sv}@av)", (int) i + 1,
                                         entry_properties (entry),
                                         g_variant_new_array (G_VARIANT_TYPE_VARIANT,
                                                              NULL, 0));
        g_variant_builder_add (&children, "v", child);
      }
    return g_variant_new ("(i@a{sv}av)", 0, root_properties (), &children);
  }

  const Entry *entry = &g_array_index (entries, Entry, parent_id - 1);
  return g_variant_new ("(i@a{sv}av)", parent_id, entry_properties (entry),
                        &children);
}

static gboolean
valid_id (int id)
{
  return id >= 0 && id <= (int) entries->len;
}

/* Pulsaron la entrada 'id'. Copiamos lo necesario ANTES de llamar a la
 * app, porque la app puede rehacer el menú (y liberar 'entries'). */
static void
activate_entry (int id)
{
  if (id <= 0 || id > (int) entries->len)
    return;

  const Entry *entry = &g_array_index (entries, Entry, id - 1);
  EntryKind kind = entry->kind;
  g_autofree char *vpn_id = g_strdup (entry->vpn_id);

  switch (kind) {
  case ENTRY_TOGGLE: callbacks.toggle (vpn_id, callbacks_data); break;
  case ENTRY_SHOW:   callbacks.show (callbacks_data);           break;
  case ENTRY_QUIT:   callbacks.quit (callbacks_data);           break;
  default:                                                       break;
  }
}

/* ---------------------------------------------------------------- */
/* D-Bus: lo que nos preguntan y nos piden                          */
/* ---------------------------------------------------------------- */

static void
handle_method_call (GDBusConnection       *connection,
                    const char            *sender,
                    const char            *object_path,
                    const char            *interface_name,
                    const char            *method_name,
                    GVariant              *parameters,
                    GDBusMethodInvocation *invocation,
                    gpointer               user_data)
{
  (void) connection; (void) sender; (void) object_path;
  (void) interface_name; (void) user_data;

  /* --- El icono --- */
  if (g_str_equal (method_name, "Activate") ||
      g_str_equal (method_name, "SecondaryActivate")) {
    /* Con ItemIsMenu = TRUE el clic normal abre el menú; si aun así nos
     * llaman (p. ej. clic central), mostramos la ventana. */
    g_dbus_method_invocation_return_value (invocation, NULL);
    callbacks.show (callbacks_data);
  } else if (g_str_equal (method_name, "ContextMenu") ||
             g_str_equal (method_name, "Scroll")) {
    g_dbus_method_invocation_return_value (invocation, NULL);

  /* --- El menú --- */
  } else if (g_str_equal (method_name, "GetLayout")) {
    int parent_id, depth;
    g_variant_get (parameters, "(ii@as)", &parent_id, &depth, NULL);
    if (!valid_id (parent_id)) {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_INVALID_ARGS,
                                             "Unknown id %d", parent_id);
      return;
    }
    g_dbus_method_invocation_return_value (
      invocation, g_variant_new ("(u@(ia{sv}av))", revision,
                                 build_layout (parent_id, depth)));
  } else if (g_str_equal (method_name, "GetGroupProperties")) {
    g_autoptr (GVariant) ids = NULL;
    g_variant_get (parameters, "(@ai@as)", &ids, NULL);

    GVariantBuilder result;
    g_variant_builder_init (&result, G_VARIANT_TYPE ("a(ia{sv})"));
    gsize n_ids;
    const gint32 *id_list = g_variant_get_fixed_array (ids, &n_ids,
                                                       sizeof (gint32));
    for (gsize i = 0; i < n_ids; i++) {
      int id = id_list[i];
      if (!valid_id (id))
        continue;
      GVariant *props = id == 0
        ? root_properties ()
        : entry_properties (&g_array_index (entries, Entry, id - 1));
      g_variant_builder_add (&result, "(i@a{sv})", id, props);
    }
    g_dbus_method_invocation_return_value (
      invocation, g_variant_new ("(a(ia{sv}))", &result));
  } else if (g_str_equal (method_name, "GetProperty")) {
    int id;
    const char *name;
    g_variant_get (parameters, "(i&s)", &id, &name);
    /* ref_sink: las variantes recién creadas son "flotantes"; así pasan
     * a ser nuestras y g_autoptr las puede liberar sin problema. */
    g_autoptr (GVariant) props = !valid_id (id) ? NULL
      : g_variant_ref_sink (id == 0 ? root_properties ()
                            : entry_properties (&g_array_index (entries, Entry,
                                                                id - 1)));
    g_autoptr (GVariant) value = props
      ? g_variant_lookup_value (props, name, NULL) : NULL;
    if (value == NULL) {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_INVALID_ARGS,
                                             "No property %s on %d", name, id);
      return;
    }
    g_dbus_method_invocation_return_value (invocation,
                                           g_variant_new ("(v)", value));
  } else if (g_str_equal (method_name, "Event")) {
    int id;
    const char *event;
    g_variant_get (parameters, "(i&s@vu)", &id, &event, NULL, NULL);
    g_dbus_method_invocation_return_value (invocation, NULL);
    if (g_str_equal (event, "clicked"))
      activate_entry (id);
  } else if (g_str_equal (method_name, "EventGroup")) {
    g_autoptr (GVariantIter) events = NULL;
    g_variant_get (parameters, "(a(isvu))", &events);

    /* Primero guardamos qué pulsar y respondemos; luego actuamos. */
    g_autoptr (GArray) clicked = g_array_new (FALSE, FALSE, sizeof (int));
    int id;
    const char *event;
    while (g_variant_iter_loop (events, "(i&svu)", &id, &event, NULL, NULL))
      if (g_str_equal (event, "clicked"))
        g_array_append_val (clicked, id);

    g_dbus_method_invocation_return_value (invocation,
                                           g_variant_new ("(@ai)",
                                             g_variant_new_array (G_VARIANT_TYPE_INT32,
                                                                  NULL, 0)));
    for (guint i = 0; i < clicked->len; i++)
      activate_entry (g_array_index (clicked, int, i));
  } else if (g_str_equal (method_name, "AboutToShow")) {
    g_dbus_method_invocation_return_value (invocation,
                                           g_variant_new ("(b)", FALSE));
  } else if (g_str_equal (method_name, "AboutToShowGroup")) {
    GVariant *empty1 = g_variant_new_array (G_VARIANT_TYPE_INT32, NULL, 0);
    GVariant *empty2 = g_variant_new_array (G_VARIANT_TYPE_INT32, NULL, 0);
    g_dbus_method_invocation_return_value (invocation,
                                           g_variant_new ("(@ai@ai)",
                                                          empty1, empty2));
  } else {
    g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                           G_DBUS_ERROR_UNKNOWN_METHOD,
                                           "Unknown method %s", method_name);
  }
}

static GVariant *
handle_get_property (GDBusConnection *connection,
                     const char      *sender,
                     const char      *object_path,
                     const char      *interface_name,
                     const char      *property_name,
                     GError         **error,
                     gpointer         user_data)
{
  (void) connection; (void) sender; (void) object_path; (void) user_data;

  if (g_str_equal (interface_name, "com.canonical.dbusmenu")) {
    if (g_str_equal (property_name, "Version"))
      return g_variant_new_uint32 (3);
    if (g_str_equal (property_name, "TextDirection"))
      return g_variant_new_string ("ltr");
    if (g_str_equal (property_name, "Status"))
      return g_variant_new_string ("normal");
    if (g_str_equal (property_name, "IconThemePath"))
      return g_variant_new_strv (NULL, 0);
  } else {
    if (g_str_equal (property_name, "Category"))
      return g_variant_new_string ("ApplicationStatus");
    if (g_str_equal (property_name, "Id"))
      return g_variant_new_string (app_id);
    if (g_str_equal (property_name, "Title"))
      return g_variant_new_string ("VPN Portal");
    if (g_str_equal (property_name, "Status"))
      return g_variant_new_string ("Active");
    if (g_str_equal (property_name, "IconName"))
      return g_variant_new_string (icon_name);
    if (g_str_equal (property_name, "IconThemePath"))
      return g_variant_new_string ("");
    if (g_str_equal (property_name, "Menu"))
      return g_variant_new_object_path (MENU_PATH);
    if (g_str_equal (property_name, "ItemIsMenu"))
      return g_variant_new_boolean (TRUE);
    if (g_str_equal (property_name, "ToolTip"))
      return g_variant_new ("(s@a(iiay)ss)", icon_name,
                            g_variant_new_array (G_VARIANT_TYPE ("(iiay)"),
                                                 NULL, 0),
                            "VPN Portal", status_text);
  }

  g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
               "Unknown property %s", property_name);
  return NULL;
}

/* ---------------------------------------------------------------- */
/* Apuntarse en la barra                                            */
/* ---------------------------------------------------------------- */

static void
register_with_watcher (void)
{
  /* Asíncrono y sin esperar respuesta: si la extensión no está, no pasa
   * nada; cuando aparezca, on_watcher_appeared lo volverá a intentar. */
  g_dbus_connection_call (bus, WATCHER_NAME, "/StatusNotifierWatcher",
                          WATCHER_NAME, "RegisterStatusNotifierItem",
                          g_variant_new ("(s)", bus_name), NULL,
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

/* Ya tenemos nuestro nombre en el bus: nos apuntamos. */
static void
on_name_acquired (GDBusConnection *connection, const char *name,
                  gpointer user_data)
{
  (void) connection; (void) name; (void) user_data;
  register_with_watcher ();
}

/* La extensión (re)aparece, p. ej. al reiniciar GNOME Shell. */
static void
on_watcher_appeared (GDBusConnection *connection, const char *name,
                     const char *owner, gpointer user_data)
{
  (void) connection; (void) name; (void) owner; (void) user_data;
  register_with_watcher ();
}

/* ---------------------------------------------------------------- */
/* API                                                              */
/* ---------------------------------------------------------------- */

void
tray_init (const char          *tray_app_id,
           const TrayCallbacks *tray_callbacks,
           gpointer             user_data)
{
  static const GDBusInterfaceVTable vtable = {
    .method_call = handle_method_call,
    .get_property = handle_get_property,
  };

  callbacks = *tray_callbacks;
  callbacks_data = user_data;
  app_id = g_strdup (tray_app_id);
  entries = g_array_new (FALSE, TRUE, sizeof (Entry));
  g_array_set_clear_func (entries, entry_clear);
  icon_name = g_strdup ("network-vpn-disconnected-symbolic");
  status_text = g_strdup ("");

  bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  if (bus == NULL)
    return;

  node_info = g_dbus_node_info_new_for_xml (introspection_xml, NULL);
  item_registration =
    g_dbus_connection_register_object (bus, ITEM_PATH,
                                       node_info->interfaces[0], &vtable,
                                       NULL, NULL, NULL);
  menu_registration =
    g_dbus_connection_register_object (bus, MENU_PATH,
                                       node_info->interfaces[1], &vtable,
                                       NULL, NULL, NULL);

  /* El protocolo pide un nombre de este estilo para cada icono. */
  bus_name = g_strdup_printf ("org.kde.StatusNotifierItem-%d-1", getpid ());
  name_owner_id = g_bus_own_name_on_connection (bus, bus_name,
                                                G_BUS_NAME_OWNER_FLAGS_NONE,
                                                on_name_acquired, NULL,
                                                NULL, NULL);
  watcher_watch_id = g_bus_watch_name_on_connection (bus, WATCHER_NAME,
                                                     G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                     on_watcher_appeared,
                                                     NULL, NULL, NULL);
  tray_update (NULL, 0);
}

/* Rehace el menú y el icono según el estado de las VPN. */
void
tray_update (const TrayItem *items, guint n_items)
{
  if (entries == NULL)
    return;

  /* Como mucho una VPN en uso (gpclient no admite más). */
  const TrayItem *active = NULL;
  for (guint i = 0; i < n_items; i++)
    if (items[i].state != VPN_DISCONNECTED)
      active = &items[i];

  const char *new_icon = "network-vpn-disconnected-symbolic";
  g_autofree char *status = NULL;
  if (active == NULL) {
    status = g_strdup ("Sin conexión");
  } else {
    new_icon = active->state == VPN_CONNECTED ? "network-vpn-symbolic"
                                              : "network-vpn-acquiring-symbolic";
    status = g_strdup_printf ("%s · %s", active->name,
                              vpn_state_to_string (active->state));
  }

  /* El menú: estado, VPN, y las opciones de la app. */
  g_array_set_size (entries, 0);
  add_entry (ENTRY_TEXT, status, NULL);
  add_entry (ENTRY_SEPARATOR, NULL, NULL);
  if (active != NULL) {
    g_autofree char *label = g_strdup_printf ("Desconectar %s", active->name);
    add_entry (ENTRY_TOGGLE, label, active->id);
  } else if (n_items == 0) {
    add_entry (ENTRY_TEXT, "Añade una VPN desde la ventana", NULL);
  } else {
    for (guint i = 0; i < n_items; i++) {
      g_autofree char *label = g_strdup_printf ("Conectar %s", items[i].name);
      add_entry (ENTRY_TOGGLE, label, items[i].id);
    }
  }
  add_entry (ENTRY_SEPARATOR, NULL, NULL);
  add_entry (ENTRY_SHOW, "Mostrar ventana", NULL);
  add_entry (ENTRY_QUIT, "Salir", NULL);
  revision++;

  gboolean icon_changed = g_strcmp0 (icon_name, new_icon) != 0;
  g_set_str (&icon_name, new_icon);
  g_set_str (&status_text, status);

  if (bus == NULL)
    return;

  /* Avisamos a la barra: "el menú ha cambiado" y, si toca, "el icono". */
  g_dbus_connection_emit_signal (bus, NULL, MENU_PATH,
                                 "com.canonical.dbusmenu", "LayoutUpdated",
                                 g_variant_new ("(ui)", revision, 0), NULL);
  if (icon_changed)
    g_dbus_connection_emit_signal (bus, NULL, ITEM_PATH,
                                   "org.kde.StatusNotifierItem", "NewIcon",
                                   NULL, NULL);
  g_dbus_connection_emit_signal (bus, NULL, ITEM_PATH,
                                 "org.kde.StatusNotifierItem", "NewToolTip",
                                 NULL, NULL);
}

void
tray_shutdown (void)
{
  g_clear_handle_id (&watcher_watch_id, g_bus_unwatch_name);
  g_clear_handle_id (&name_owner_id, g_bus_unown_name);
  if (bus != NULL) {
    if (item_registration != 0)
      g_dbus_connection_unregister_object (bus, item_registration);
    if (menu_registration != 0)
      g_dbus_connection_unregister_object (bus, menu_registration);
  }
  item_registration = menu_registration = 0;
  g_clear_pointer (&node_info, g_dbus_node_info_unref);
  g_clear_object (&bus);
  g_clear_pointer (&entries, g_array_unref);
  g_clear_pointer (&bus_name, g_free);
  g_clear_pointer (&app_id, g_free);
  g_clear_pointer (&icon_name, g_free);
  g_clear_pointer (&status_text, g_free);
}

/*
 * Quien pinta los iconos se registra en el bus con el nombre
 * org.kde.StatusNotifierWatcher. Si nadie lo tiene, nuestro icono no se
 * vería en ningún sitio.
 */
gboolean
tray_is_available (void)
{
  g_autoptr (GDBusConnection) connection = g_bus_get_sync (G_BUS_TYPE_SESSION,
                                                           NULL, NULL);
  if (connection == NULL)
    return FALSE;

  g_autoptr (GVariant) reply =
    g_dbus_connection_call_sync (connection, "org.freedesktop.DBus",
                                 "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                 "NameHasOwner",
                                 g_variant_new ("(s)", WATCHER_NAME),
                                 G_VARIANT_TYPE ("(b)"), G_DBUS_CALL_FLAGS_NONE,
                                 1000, NULL, NULL);
  gboolean has_owner = FALSE;
  if (reply != NULL)
    g_variant_get (reply, "(b)", &has_owner);
  return has_owner;
}
