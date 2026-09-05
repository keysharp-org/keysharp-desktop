#include "kwin_bus.h"

#include "kwin_wire.h"
#include "protocol.h"
#include "transport.h"

#include <unistd.h>

#include <gio/gio.h>
#include <glib-unix.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define KSD_KWIN_BUS_NAME "io.github.keysharp.KWinProvider1"
#define KSD_KWIN_BUS_PATH "/io/github/keysharp/KWinProvider"
#define KSD_KWIN_COMPOSITOR_NAME "org.kde.KWin"
#define KSD_KWIN_SCRIPTING_PATH "/Scripting"
#define KSD_KWIN_SCRIPTING_INTERFACE "org.kde.kwin.Scripting"
#define KSD_KWIN_SCRIPT_ID "io.github.keysharp.desktop.kwin"
#define KSD_KWIN_COMPOSITOR_RECHECK_SECONDS 2u
#ifndef KSD_KWIN_SCRIPT_PATH
#define KSD_KWIN_SCRIPT_PATH \
    "/usr/share/kwin/scripts/io.github.keysharp.desktop.kwin/contents/code/main.js"
#endif

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
    ksd_kwin_queue *queue;
    /* The unique bus owner of org.kde.KWin. Every script call is pinned to it,
     * so another process in the session cannot take the provider channel. */
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
    /* The socket the authority relays requests over, and the requests still
     * waiting on the script. Answered out of order, because the whole point of
     * the queue is that a cheap verb need not wait behind an enumeration. */
    int relay;
    guint relay_source;
    guint compositor_source;
    struct {
        bool active;
        uint64_t request_id;
        uint16_t opcode;
        char sequence[KSD_KWIN_SEQ_HEX + 1u];
    } inflight[KSD_KWIN_MAX_JOBS];
};

static bool compositor_owner(char peer[64])
{
    GError *error = NULL;
    GDBusConnection *connection =
        g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    GVariant *reply = NULL;
    const char *owner = NULL;
    bool valid = false;

    if (connection != NULL) {
        reply = g_dbus_connection_call_sync(connection,
            "org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "GetNameOwner",
            g_variant_new("(s)", KSD_KWIN_COMPOSITOR_NAME),
            G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL,
            &error);
    }
    if (reply != NULL) {
        g_variant_get(reply, "(&s)", &owner);
        if (g_dbus_is_unique_name(owner) && strlen(owner) < 64u) {
            memcpy(peer, owner, strlen(owner) + 1u);
            valid = true;
        }
        g_variant_unref(reply);
    }
    g_clear_error(&error);
    if (connection != NULL)
        g_object_unref(connection);
    return valid;
}

/* Writes one response frame back to the authority. */
static void relay_answer(ksd_kwin_bus *bus, uint64_t request_id,
                         uint16_t opcode, uint32_t status, const uint8_t *body,
                         uint32_t body_length)
{
    ksd_frame frame;
    ksd_buffer packed;
    ksd_buffer payload;
    ksd_buffer translated;

    ksd_buffer_init(&translated, KSD_MAX_TEXT_BYTES + 4u);
    if (status == KSD_STATUS_OK
        && !ksd_kwin_response_payload(opcode, body, body_length,
                                      &translated))
        status = KSD_STATUS_INTERNAL;

    /* The status rides in the payload, not the header: a frame carries no
     * status field, and the rest of this service already answers with an
     * eight-byte status and detail prologue ahead of the tail. One shape. */
    ksd_buffer_init(&payload, translated.length + 8u);
    if (!ksd_buffer_u32(&payload, status) || !ksd_buffer_u32(&payload, 0u)
        || (status == KSD_STATUS_OK && translated.length != 0u
            && !ksd_buffer_bytes(&payload, translated.data,
                                 translated.length))) {
        ksd_buffer_clear(&payload);
        ksd_buffer_clear(&translated);
        return;
    }
    memset(&frame, 0, sizeof(frame));
    frame.magic[0] = KSD_FRAME_MAGIC_0;
    frame.magic[1] = KSD_FRAME_MAGIC_1;
    frame.magic[2] = KSD_FRAME_MAGIC_2;
    frame.magic[3] = KSD_FRAME_MAGIC_3;
    frame.major = KSD_PROTOCOL_MAJOR;
    frame.minor = KSD_PROTOCOL_MINOR;
    frame.request_id = request_id;
    frame.payload = payload.data;
    frame.payload_length = (uint32_t)payload.length;
    ksd_buffer_init(&packed, KSD_FRAME_HEADER_SIZE + payload.length + 16u);
    if (ksd_frame_pack(&frame, &packed))
        (void)ksd_write_all(bus->relay, packed.data, packed.length);
    ksd_buffer_clear(&packed);
    ksd_buffer_clear(&payload);
    ksd_buffer_clear(&translated);
}

/* Answers every relayed request whose job the script has now reported. Called
 * after each Report, because that is the only thing that can complete one. */
static void relay_drain(ksd_kwin_bus *bus)
{
    for (size_t index = 0u; index < KSD_KWIN_MAX_JOBS; index++) {
        uint32_t status = 0u;
        const uint8_t *body = NULL;
        uint32_t body_length = 0u;

        if (!bus->inflight[index].active)
            continue;
        if (!ksd_kwin_queue_result(bus->queue, bus->inflight[index].sequence,
                                  &status, &body, &body_length))
            continue;
        relay_answer(bus, bus->inflight[index].request_id,
                     bus->inflight[index].opcode, status, body, body_length);
        ksd_kwin_queue_release(bus->queue, bus->inflight[index].sequence);
        bus->inflight[index].active = false;
    }
}

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

/* Completes a parked poll with whatever the queue has for that lane. Used
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
    /* Queued work produces a batch; the idle timer produces an empty reply
     * that tells the script the lane is alive and lets it re-park. Asking the
     * queue prevents work arriving at the timer boundary from being left
     * waiting. */
    ksd_buffer_init(&reply, 65536u);
    if (ksd_kwin_queue_poll_parked(bus->queue, lane,
                                  (uint64_t)g_get_monotonic_time() / 1000u,
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

    bool hello = g_strcmp0(method, "Hello") == 0;
    bool allowed = sender != NULL && generation != NULL
        && strcmp(bus->peer, sender) == 0
        && (hello ? generation[0] == '\0'
                  : ksd_kwin_peer_allowed(bus->peer, sender, bus->uid,
                        bus->uid, ksd_kwin_queue_generation(bus->queue),
                        generation));
    if (!allowed) {
        g_dbus_method_invocation_return_error_literal(invocation,
            G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED,
            "not the KWin compositor this daemon is serving");
        ksd_buffer_clear(&reply);
        return;
    }
    (void)connection;

    if (g_strcmp0(method, "Hello") == 0) {
        ok = ksd_kwin_queue_hello(bus->queue, &reply);
    } else if (g_strcmp0(method, "Report") == 0) {
        ok = ksd_kwin_queue_report(bus->queue, (const uint8_t *)envelope,
                                  strlen(envelope), &reply);
        /* A report is the only thing that can complete a job, so it is the
         * only moment a relayed request can be answered. */
        if (ok)
            relay_drain(bus);
    } else if (g_strcmp0(method, "Event") == 0) {
        /* Events never carry a job in either direction, so an event storm
         * cannot delay dispatched work. Acknowledged and nothing more until
         * the watch surface lands. */
        ok = ksd_kwin_queue_hello(bus->queue, &reply);
    } else if (g_strcmp0(method, "Poll") == 0) {
        ksd_kwin_poll poll;
        ksd_kwin_poll_outcome outcome;

        outcome = ksd_kwin_queue_poll(bus->queue, (const uint8_t *)envelope,
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

static bool scripting_call(GDBusConnection *connection, const char *method,
                           GVariant *parameters)
{
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(connection,
        KSD_KWIN_COMPOSITOR_NAME, KSD_KWIN_SCRIPTING_PATH,
        KSD_KWIN_SCRIPTING_INTERFACE, method, parameters, NULL,
        G_DBUS_CALL_FLAGS_NONE, (gint)KSD_KWIN_SCRIPTING_PROBE_MS, NULL,
        &error);
    bool succeeded = reply != NULL;

    if (reply != NULL)
        g_variant_unref(reply);
    if (error != NULL) {
        g_printerr("keysharp-desktop daemon: KWin script %s failed: %s\n",
                   method, error->message);
        g_error_free(error);
    }
    return succeeded;
}

static void on_name_acquired(GDBusConnection *connection, const gchar *name,
                             gpointer data)
{
    (void)name;
    (void)data;
    /* A loaded KWin script is not started again by Scripting.start(). It may
     * have sent Hello before this daemon owned its name, or may still carry a
     * generation from the previous daemon. Reloading after name acquisition
     * gives it one deterministic handshake point and removes both races. */
    bool unloaded = scripting_call(connection, "unloadScript",
        g_variant_new("(s)", KSD_KWIN_SCRIPT_ID));
    bool loaded = scripting_call(connection, "loadScript",
        g_variant_new("(ss)", KSD_KWIN_SCRIPT_PATH, KSD_KWIN_SCRIPT_ID));
    bool started = loaded && scripting_call(connection, "start", NULL);

    if (!unloaded || !loaded || !started)
        g_printerr("keysharp-desktop daemon: could not restart the KWin"
                   " provider script; window operations may be unavailable\n");
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

ksd_kwin_bus *ksd_kwin_bus_start(ksd_kwin_queue *queue, int relay)
{
    ksd_kwin_bus *bus;
    GError *error = NULL;

    if (queue == NULL)
        return NULL;
    bus = g_new0(ksd_kwin_bus, 1);
    bus->queue = queue;
    bus->uid = getuid();
    bus->authority = -1;
    bus->relay = relay;
    if (!compositor_owner(bus->peer)) {
        g_free(bus);
        return NULL;
    }
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
                                  on_bus_acquired, on_name_acquired,
                                  on_name_lost, bus, NULL);
    return bus;
}

/* The public socket protocol is binary, while the KWin script channel is a
 * D-Bus string. Translate the small fixed request records at that boundary so
 * a zero byte in a handle or coordinate cannot truncate a script job. */
bool ksd_kwin_request_text(uint16_t opcode, const uint8_t *payload,
                           uint32_t payload_length, char *text,
                           size_t capacity, uint32_t *text_length)
{
    int written = -1;

    if (text == NULL || capacity == 0u || text_length == NULL)
        return false;
    if (opcode == KSD_OP_WINDOW_HANDLES || opcode == KSD_OP_WINDOW_ACTIVE
        || opcode == KSD_OP_CURSOR_POSITION || opcode == KSD_OP_WORK_AREA) {
        if (payload_length != 0u)
            return false;
        text[0] = '\0';
        *text_length = 0u;
        return true;
    }
    if (payload == NULL)
        return false;
    if (opcode == KSD_OP_WINDOW_LIST) {
        if (payload_length != 8u || ksd_decode_u32(payload) > 1u
            || ksd_decode_u32(payload + 4u) != 0u)
            return false;
        written = snprintf(text, capacity, "%u", ksd_decode_u32(payload));
    } else if (opcode == KSD_OP_WINDOW_QUERY
               || opcode == KSD_OP_WINDOW_FOCUS
               || opcode == KSD_OP_WINDOW_RAISE
               || opcode == KSD_OP_WINDOW_LOWER
               || opcode == KSD_OP_WINDOW_CLOSE) {
        if (payload_length != 8u || ksd_decode_u64(payload) == 0u)
            return false;
        written = snprintf(text, capacity, "%llu",
            (unsigned long long)ksd_decode_u64(payload));
    } else if (opcode == KSD_OP_WINDOW_MOVE_RESIZE) {
        if (payload_length != 24u || ksd_decode_u64(payload) == 0u)
            return false;
        written = snprintf(text, capacity, "%llu %d %d %u %u",
            (unsigned long long)ksd_decode_u64(payload),
            (int32_t)ksd_decode_u32(payload + 8u),
            (int32_t)ksd_decode_u32(payload + 12u),
            ksd_decode_u32(payload + 16u),
            ksd_decode_u32(payload + 20u));
    } else if (opcode == KSD_OP_WINDOW_SET_STATE
               || opcode == KSD_OP_WINDOW_SET_OPACITY
               || opcode == KSD_OP_WINDOW_SET_ABOVE
               || opcode == KSD_OP_WINDOW_SET_DECORATED
               || opcode == KSD_OP_WINDOW_SET_SKIP_TASKBAR) {
        uint32_t value;
        uint32_t maximum;

        if (payload_length != 16u || ksd_decode_u64(payload) == 0u
            || ksd_decode_u32(payload + 12u) != 0u)
            return false;
        value = ksd_decode_u32(payload + 8u);
        maximum = opcode == KSD_OP_WINDOW_SET_OPACITY ? 255u
            : opcode == KSD_OP_WINDOW_SET_STATE ? 2u : 1u;
        if (value > maximum)
            return false;
        written = snprintf(text, capacity, "%llu %u",
            (unsigned long long)ksd_decode_u64(payload), value);
    } else {
        return false;
    }
    if (written < 0 || (size_t)written >= capacity)
        return false;
    *text_length = (uint32_t)written;
    return true;
}

static bool parse_point(const uint8_t *body, uint32_t body_length,
                        int32_t *x, int32_t *y)
{
    char text[128];
    long long parsed_x;
    long long parsed_y;
    int consumed = 0;

    if (body == NULL || body_length == 0u || body_length >= sizeof(text))
        return false;
    memcpy(text, body, body_length);
    text[body_length] = '\0';
    if (sscanf(text, "{\"x\":%lld,\"y\":%lld}%n", &parsed_x,
               &parsed_y, &consumed) != 2
        || consumed != (int)body_length
        || parsed_x < INT32_MIN || parsed_x > INT32_MAX
        || parsed_y < INT32_MIN || parsed_y > INT32_MAX)
        return false;
    *x = (int32_t)parsed_x;
    *y = (int32_t)parsed_y;
    return true;
}

static bool parse_area(const uint8_t *body, uint32_t body_length,
                       int32_t *x, int32_t *y, int32_t *width,
                       int32_t *height)
{
    char text[192];
    long long parsed_x;
    long long parsed_y;
    long long parsed_width;
    long long parsed_height;
    int consumed = 0;

    if (body == NULL || body_length == 0u || body_length >= sizeof(text))
        return false;
    memcpy(text, body, body_length);
    text[body_length] = '\0';
    if (sscanf(text,
               "{\"x\":%lld,\"y\":%lld,\"width\":%lld,\"height\":%lld}%n",
               &parsed_x, &parsed_y, &parsed_width, &parsed_height,
               &consumed) != 4
        || consumed != (int)body_length
        || parsed_x < INT32_MIN || parsed_x > INT32_MAX
        || parsed_y < INT32_MIN || parsed_y > INT32_MAX
        || parsed_width <= 0 || parsed_width > INT32_MAX
        || parsed_height <= 0 || parsed_height > INT32_MAX)
        return false;
    *x = (int32_t)parsed_x;
    *y = (int32_t)parsed_y;
    *width = (int32_t)parsed_width;
    *height = (int32_t)parsed_height;
    return true;
}

bool ksd_kwin_response_payload(uint16_t opcode, const uint8_t *body,
                               uint32_t body_length, ksd_buffer *payload)
{
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;

    if (payload == NULL || (body == NULL && body_length != 0u))
        return false;
    if (opcode == KSD_OP_WINDOW_LIST || opcode == KSD_OP_WINDOW_HANDLES
        || opcode == KSD_OP_WINDOW_ACTIVE || opcode == KSD_OP_WINDOW_QUERY) {
        return body_length <= KSD_MAX_TEXT_BYTES
            && ksd_utf8_valid(body, body_length, false)
            && ksd_buffer_u32(payload, body_length)
            && ksd_buffer_bytes(payload, body, body_length);
    }
    if (opcode == KSD_OP_CURSOR_POSITION) {
        return parse_point(body, body_length, &x, &y)
            && ksd_buffer_u32(payload, (uint32_t)x)
            && ksd_buffer_u32(payload, (uint32_t)y);
    }
    if (opcode == KSD_OP_WORK_AREA) {
        return parse_area(body, body_length, &x, &y, &width, &height)
            && ksd_buffer_u32(payload, (uint32_t)x)
            && ksd_buffer_u32(payload, (uint32_t)y)
            && ksd_buffer_u32(payload, (uint32_t)width)
            && ksd_buffer_u32(payload, (uint32_t)height);
    }
    if (opcode == KSD_OP_WINDOW_FOCUS || opcode == KSD_OP_WINDOW_RAISE
        || opcode == KSD_OP_WINDOW_LOWER || opcode == KSD_OP_WINDOW_CLOSE
        || opcode == KSD_OP_WINDOW_MOVE_RESIZE
        || opcode == KSD_OP_WINDOW_SET_STATE
        || opcode == KSD_OP_WINDOW_SET_OPACITY
        || opcode == KSD_OP_WINDOW_SET_ABOVE
        || opcode == KSD_OP_WINDOW_SET_DECORATED
        || opcode == KSD_OP_WINDOW_SET_SKIP_TASKBAR)
        return body_length == 0u;
    return false;
}

/* One relayed request from the authority: queue it for the script, and wake
 * the lane it belongs to so it goes out on the poll that is already parked
 * rather than waiting for the idle timer. */
static gboolean relay_readable(gint descriptor, GIOCondition condition,
                               gpointer data)
{
    ksd_kwin_bus *bus = data;
    uint8_t header[KSD_FRAME_HEADER_SIZE];
    uint8_t *body = NULL;
    uint32_t payload_length;
    uint64_t request_id;
    uint16_t opcode;
    char sequence[KSD_KWIN_SEQ_HEX + 1u];
    char text_body[160];
    uint32_t text_length = 0u;
    size_t slot = KSD_KWIN_MAX_JOBS;

    (void)descriptor;
    if ((condition & (G_IO_HUP | G_IO_ERR)) != 0) {
        bus->relay_source = 0u;
        return G_SOURCE_REMOVE;
    }
    if (!ksd_read_all(bus->relay, header, sizeof(header))) {
        bus->relay_source = 0u;
        return G_SOURCE_REMOVE;
    }
    payload_length = ksd_decode_u32(header + KSD_FRAME_PAYLOAD_LENGTH_OFFSET);
    opcode = ksd_decode_u16(header + KSD_FRAME_OPCODE_OFFSET);
    request_id = ksd_decode_u64(header + KSD_FRAME_REQUEST_ID_OFFSET);
    if (payload_length > KSD_MAX_TEXT_BYTES) {
        bus->relay_source = 0u;
        return G_SOURCE_REMOVE;
    }
    if (payload_length != 0u) {
        body = g_malloc(payload_length);
        if (!ksd_read_all(bus->relay, body, payload_length)) {
            g_free(body);
            bus->relay_source = 0u;
            return G_SOURCE_REMOVE;
        }
    }
    if (!ksd_kwin_request_text(opcode, body, payload_length, text_body,
                               sizeof(text_body), &text_length)) {
        relay_answer(bus, request_id, opcode, KSD_STATUS_INVALID_REQUEST,
                     NULL, 0u);
        g_free(body);
        return G_SOURCE_CONTINUE;
    }
    for (size_t index = 0u; index < KSD_KWIN_MAX_JOBS; index++) {
        if (!bus->inflight[index].active) {
            slot = index;
            break;
        }
    }
    /* Both refusals are BUSY rather than a failure: neither reached the
     * compositor, so the caller may retry safely. */
    if (slot == KSD_KWIN_MAX_JOBS
        || !ksd_kwin_queue_submit(bus->queue, opcode,
                                 (const uint8_t *)text_body, text_length,
                                 (uint64_t)g_get_monotonic_time() / 1000u,
                                 sequence)) {
        relay_answer(bus, request_id, opcode, KSD_STATUS_BUSY, NULL, 0u);
        g_free(body);
        return G_SOURCE_CONTINUE;
    }
    g_free(body);
    bus->inflight[slot].active = true;
    bus->inflight[slot].request_id = request_id;
    bus->inflight[slot].opcode = opcode;
    memcpy(bus->inflight[slot].sequence, sequence, sizeof(sequence));
    /* The lane this job belongs to may have a poll parked on it. Releasing it
     * now is what makes a submitted job leave promptly instead of waiting out
     * the idle timer. */
    release_park(bus, ksd_kwin_lane_for(opcode));
    return G_SOURCE_CONTINUE;
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

static gboolean compositor_current(gpointer data)
{
    ksd_kwin_bus *bus = data;
    char owner[64] = { 0 };

    if (compositor_owner(owner) && strcmp(owner, bus->peer) == 0)
        return G_SOURCE_CONTINUE;
    bus->compositor_source = 0u;
    g_printerr("keysharp-desktop daemon: KWin compositor changed;"
               " restarting the session backend\n");
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
    bus->compositor_source = g_timeout_add_seconds(
        KSD_KWIN_COMPOSITOR_RECHECK_SECONDS, compositor_current, bus);
    if (bus->relay >= 0)
        bus->relay_source = g_unix_fd_add(bus->relay,
            G_IO_IN | G_IO_HUP | G_IO_ERR, relay_readable, bus);
    g_unix_fd_add(descriptor, G_IO_IN | G_IO_HUP | G_IO_ERR,
                  authority_readable, bus);
    g_main_loop_run(bus->loop);
    return 0;
}

void ksd_kwin_bus_stop(ksd_kwin_bus *bus)
{
    if (bus == NULL)
        return;
    if (bus->compositor_source != 0u)
        g_source_remove(bus->compositor_source);
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
