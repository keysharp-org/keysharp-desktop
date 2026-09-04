#include "kwin_bus.h"

#include "kwin_wire.h"

#include <unistd.h>

#include <gio/gio.h>
#include <glib-unix.h>
#include <string.h>

#define KSD_KWIN_BUS_NAME "io.github.keysharp.KWinProvider1"
#define KSD_KWIN_BUS_PATH "/io/github/keysharp/KWinProvider"

/* Kept in step with interfaces/private/io.github.keysharp.KWinProvider1.xml,
 * which the XML gate parses and holds to four methods of one signature. This
 * copy is what GDBus registers; the file is what is reviewed and installed. */
static const char introspection[] =
    "<node>"
    "  <interface name='io.github.keysharp.KWinProvider1'>"
    "    <method name='Hello'>"
    "      <arg type='s' direction='in' name='generation'/>"
    "      <arg type='s' direction='in' name='envelope'/>"
    "      <arg type='s' direction='out' name='reply'/>"
    "    </method>"
    "    <method name='Poll'>"
    "      <arg type='s' direction='in' name='generation'/>"
    "      <arg type='s' direction='in' name='envelope'/>"
    "      <arg type='s' direction='out' name='reply'/>"
    "    </method>"
    "    <method name='Report'>"
    "      <arg type='s' direction='in' name='generation'/>"
    "      <arg type='s' direction='in' name='envelope'/>"
    "      <arg type='s' direction='out' name='reply'/>"
    "    </method>"
    "    <method name='Event'>"
    "      <arg type='s' direction='in' name='generation'/>"
    "      <arg type='s' direction='in' name='envelope'/>"
    "      <arg type='s' direction='out' name='reply'/>"
    "    </method>"
    "  </interface>"
    "</node>";

struct ksd_kwin_bus {
    ksd_kwin_host *host;
    /* The unique name of the script, learned from the first call that carries
     * the right generation, and required to match on every call after. Empty
     * until then. */
    char peer[64];
    uid_t uid;
    GDBusNodeInfo *node;
    GDBusConnection *connection;
    guint name_id;
    guint object_id;
    GMainLoop *loop;
    /* One parked invocation per lane, and the timer that will answer it if no
     * work arrives first. A parked invocation is always completed and never
     * dropped: the script has exactly one outstanding per lane, and dropping
     * it ends that lane for good. */
    GDBusMethodInvocation *parked[KSD_KWIN_LANES];
    guint idle_source[KSD_KWIN_LANES];
    int authority;
};

static size_t lane_slot(ksd_kwin_lane lane)
{
    return lane == KSD_KWIN_LANE_FAST ? 0u : 1u;
}

static void answer(GDBusMethodInvocation *invocation, const ksd_buffer *reply)
{
    char *text = g_strndup((const char *)reply->data, reply->length);

    g_dbus_method_invocation_return_value(invocation,
                                          g_variant_new("(s)", text));
    g_free(text);
}

/* Completes a parked poll with whatever the host has for that lane now. Used
 * both when work arrives and when the idle timer fires, so there is one path
 * that ends a park rather than two that could disagree. */
static void release_park(ksd_kwin_bus *bus, ksd_kwin_lane lane)
{
    size_t slot = lane_slot(lane);
    GDBusMethodInvocation *invocation = bus->parked[slot];
    ksd_buffer reply;

    if (invocation == NULL)
        return;
    bus->parked[slot] = NULL;
    if (bus->idle_source[slot] != 0u) {
        g_source_remove(bus->idle_source[slot]);
        bus->idle_source[slot] = 0u;
    }
    /* An idle reply carries no jobs, which is what tells the script the lane
     * is alive and lets it re-park. */
    ksd_buffer_init(&reply, 4096u);
    if (ksd_kwin_format_poll_reply(ksd_kwin_host_generation(bus->host), lane,
                                   KSD_KWIN_IDLE_REPLY_MS, NULL, 0u,
                                   &reply)) {
        answer(invocation, &reply);
    } else {
        g_dbus_method_invocation_return_error_literal(invocation,
            G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "cannot format an idle reply");
    }
    ksd_buffer_clear(&reply);
}

/* The idle timer answers the parked poll rather than merely expiring. A timer
 * that fired and did nothing would leave the lane parked for ever: the script
 * has one request outstanding on it and re-parks only from the reply, so a
 * lane that is never answered is a lane that never speaks again.
 *
 * One thunk per lane because a GLib timeout carries a single pointer and the
 * lane has to come from somewhere; the alternative is an allocation per park
 * to free on every path out of it. */
static gboolean idle_fast(gpointer data)
{
    ksd_kwin_bus *bus = data;

    bus->idle_source[0] = 0u;
    release_park(bus, KSD_KWIN_LANE_FAST);
    return G_SOURCE_REMOVE;
}

static gboolean idle_slow(gpointer data)
{
    ksd_kwin_bus *bus = data;

    bus->idle_source[1] = 0u;
    release_park(bus, KSD_KWIN_LANE_SLOW);
    return G_SOURCE_REMOVE;
}

static void handle_call(GDBusConnection *connection, const gchar *sender,
                        const gchar *path, const gchar *iface,
                        const gchar *method, GVariant *parameters,
                        GDBusMethodInvocation *invocation, gpointer data)
{
    ksd_kwin_bus *bus = data;
    const gchar *generation = NULL;
    const gchar *envelope = NULL;
    ksd_buffer reply;
    bool ok = false;

    (void)path;
    (void)iface;
    g_variant_get(parameters, "(&s&s)", &generation, &envelope);
    ksd_buffer_init(&reply, 65536u);

    /* Exactly one peer is ever legitimate on this interface, and until now the
     * sender was discarded -- ksd_kwin_peer_allowed existed and was never
     * applied, so any process on the session bus could have driven the
     * channel. The first caller presenting the right generation is taken as
     * the script; every later call must be the same connection.
     *
     * The generation is what makes that safe to learn rather than configure:
     * it is 32 random hex digits this daemon issued and told nobody else, so
     * a caller that has it has been through Hello on this run. */
    if (sender != NULL && generation != NULL) {
        if (bus->peer[0] == 0) {
            if (g_strcmp0(generation,
                          ksd_kwin_host_generation(bus->host)) == 0) {
                g_strlcpy(bus->peer, sender, sizeof(bus->peer));
                bus->uid = getuid();
            }
        } else if (!ksd_kwin_peer_allowed(bus->peer, sender, bus->uid,
                                          bus->uid, generation,
                                          ksd_kwin_host_generation(bus->host))) {
            g_dbus_method_invocation_return_error_literal(invocation,
                G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED,
                "not the script this daemon is talking to");
            ksd_buffer_clear(&reply);
            return;
        }
    }
    (void)connection;

    if (g_strcmp0(method, "Hello") == 0) {
        ok = ksd_kwin_host_hello(bus->host, &reply);
    } else if (g_strcmp0(method, "Report") == 0) {
        ok = ksd_kwin_host_report(bus->host, (const uint8_t *)envelope,
                                  strlen(envelope), &reply);
    } else if (g_strcmp0(method, "Event") == 0) {
        /* Events never carry a job in either direction, so an event storm
         * cannot delay dispatched work. Acknowledged and nothing more until
         * the watch surface lands. */
        ok = ksd_kwin_host_hello(bus->host, &reply);
    } else if (g_strcmp0(method, "Poll") == 0) {
        ksd_kwin_poll poll;
        ksd_kwin_poll_outcome outcome;

        outcome = ksd_kwin_host_poll(bus->host, (const uint8_t *)envelope,
                                     strlen(envelope),
                                     (uint64_t)g_get_monotonic_time() / 1000u,
                                     &reply);
        if (outcome == KSD_KWIN_POLL_ANSWERED) {
            answer(invocation, &reply);
            ksd_buffer_clear(&reply);
            return;
        }
        if (outcome == KSD_KWIN_POLL_PARKED
            && ksd_kwin_parse_poll((const uint8_t *)envelope,
                                   strlen(envelope), &poll)) {
            size_t slot = lane_slot(poll.lane);

            /* A second poll on a lane that already has one parked means the
             * script lost track. The old one is answered rather than leaked,
             * because an invocation nobody completes holds a slot in the
             * script's outstanding-call budget for ever. */
            if (bus->parked[slot] != NULL)
                release_park(bus, poll.lane);
            bus->parked[slot] = invocation;
            /* Offset per lane so an idle desktop does not produce a two-fold
             * poll storm every eight seconds. */
            bus->idle_source[slot] = g_timeout_add(
                KSD_KWIN_IDLE_REPLY_MS
                    + (guint)(slot * KSD_KWIN_IDLE_STAGGER_MS),
                slot == 0u ? idle_fast : idle_slow, bus);
            ksd_buffer_clear(&reply);
            return;
        }
        ok = false;
    }

    if (ok)
        answer(invocation, &reply);
    else
        g_dbus_method_invocation_return_error_literal(invocation,
            G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
            "the daemon did not understand that envelope");
    ksd_buffer_clear(&reply);
}

static const GDBusInterfaceVTable vtable = {
    handle_call, NULL, NULL, { 0 }
};

static void on_bus_acquired(GDBusConnection *connection, const gchar *name,
                            gpointer data)
{
    ksd_kwin_bus *bus = data;
    GError *error = NULL;

    (void)name;
    bus->connection = connection;
    bus->object_id = g_dbus_connection_register_object(connection,
        KSD_KWIN_BUS_PATH, bus->node->interfaces[0], &vtable, bus, NULL,
        &error);
    if (bus->object_id == 0u) {
        g_printerr("keysharp-desktop daemon: cannot register the KWin"
                   " provider object: %s\n",
                   error != NULL ? error->message : "unknown");
        g_clear_error(&error);
    }
}

static void on_name_lost(GDBusConnection *connection, const gchar *name,
                         gpointer data)
{
    ksd_kwin_bus *bus = data;

    (void)connection;
    (void)name;
    /* Another daemon holds the name, or the bus went away. Either way this
     * process is no longer the one the script talks to, and continuing to run
     * a loop for it would be pretending otherwise. */
    g_printerr("keysharp-desktop daemon: lost the KWin provider name\n");
    if (bus->loop != NULL)
        g_main_loop_quit(bus->loop);
}

ksd_kwin_bus *ksd_kwin_bus_start(ksd_kwin_host *host)
{
    ksd_kwin_bus *bus;
    GError *error = NULL;

    if (host == NULL)
        return NULL;
    bus = g_new0(ksd_kwin_bus, 1);
    bus->host = host;
    bus->authority = -1;
    bus->node = g_dbus_node_info_new_for_xml(introspection, &error);
    if (bus->node == NULL) {
        g_printerr("keysharp-desktop daemon: bad KWin introspection: %s\n",
                   error != NULL ? error->message : "unknown");
        g_clear_error(&error);
        g_free(bus);
        return NULL;
    }
    /* DO_NOT_QUEUE and no replacement. Queueing would leave this daemon
     * waiting behind another for a name the script has already been told to
     * call, and allowing replacement would let a later process take the
     * channel out from under a script mid-job. */
    bus->name_id = g_bus_own_name(G_BUS_TYPE_SESSION, KSD_KWIN_BUS_NAME,
                                  G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE,
                                  on_bus_acquired, NULL, on_name_lost, bus,
                                  NULL);
    return bus;
}

static gboolean authority_readable(gint descriptor, GIOCondition condition,
                                   gpointer data)
{
    ksd_kwin_bus *bus = data;

    (void)descriptor;
    (void)condition;
    /* Any traffic or hangup on the authority socket ends the daemon, which is
     * the same rule the plain poll loop follows. The registration is the
     * daemon's whole reason to be running. */
    if (bus->loop != NULL)
        g_main_loop_quit(bus->loop);
    return G_SOURCE_REMOVE;
}

int ksd_kwin_bus_run(ksd_kwin_bus *bus, int descriptor)
{
    if (bus == NULL)
        return 1;
    bus->authority = descriptor;
    bus->loop = g_main_loop_new(NULL, FALSE);
    g_unix_fd_add(descriptor, G_IO_IN | G_IO_HUP | G_IO_ERR,
                  authority_readable, bus);
    g_main_loop_run(bus->loop);
    return 0;
}

void ksd_kwin_bus_stop(ksd_kwin_bus *bus)
{
    if (bus == NULL)
        return;
    for (size_t slot = 0u; slot < KSD_KWIN_LANES; slot++) {
        if (bus->idle_source[slot] != 0u)
            g_source_remove(bus->idle_source[slot]);
        /* Anything still parked is answered with an error rather than left
         * hanging, so the script learns the channel is gone instead of waiting
         * on a reply that will never come. */
        if (bus->parked[slot] != NULL)
            g_dbus_method_invocation_return_error_literal(bus->parked[slot],
                G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "the daemon is stopping");
    }
    if (bus->object_id != 0u && bus->connection != NULL)
        g_dbus_connection_unregister_object(bus->connection, bus->object_id);
    if (bus->name_id != 0u)
        g_bus_unown_name(bus->name_id);
    if (bus->loop != NULL)
        g_main_loop_unref(bus->loop);
    if (bus->node != NULL)
        g_dbus_node_info_unref(bus->node);
    g_free(bus);
}
