#include "x11_internal.h"

#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Everything one window needs, asked for in a single pass. The naive shape --
 * issue a request and immediately wait for it -- costs a round trip apiece, and
 * with eight per window that is what a window list spends nearly all its time
 * on. xcb hands back a cookie without waiting, so every request for every
 * window goes on the wire before any answer is read. */
typedef struct window_query {
    xcb_get_geometry_cookie_t geometry;
    xcb_translate_coordinates_cookie_t place;
    xcb_get_property_cookie_t state;
    xcb_get_property_cookie_t desktop;
    xcb_get_property_cookie_t pid;
    xcb_get_property_cookie_t opacity;
    xcb_get_property_cookie_t name;
    xcb_get_property_cookie_t app;
} window_query;

static void issue_window_query(xcb_connection_t *c, const x11_atoms *atoms,
                               xcb_window_t root, xcb_window_t window,
                               window_query *query)
{
    query->geometry = xcb_get_geometry(c, window);
    /* Translating against the screen root rather than the root reported by the
     * geometry reply, so this does not have to wait for that reply first. They
     * are the same window on every screen this opens. */
    query->place = xcb_translate_coordinates(c, window, root, 0, 0);
    /* Once, not once per state atom. The four states asked about all live in
     * this one property, and fetching it four times was four round trips. */
    query->state = ksd_x11_property_cookie(c, window, atoms->wm_state,
                                           XCB_ATOM_ATOM, 64u);
    query->desktop = ksd_x11_property_cookie(c, window, atoms->wm_desktop,
                                             XCB_ATOM_ANY, 1u);
    query->pid = ksd_x11_property_cookie(c, window, atoms->wm_pid,
                                         XCB_ATOM_ANY, 1u);
    query->opacity = ksd_x11_property_cookie(c, window, atoms->opacity,
                                             XCB_ATOM_ANY, 1u);
    query->name = ksd_x11_property_cookie(c, window, atoms->wm_name,
                                          atoms->utf8_string,
                                          KSD_X11_MAX_TEXT / 4u);
    query->app = ksd_x11_property_cookie(c, window, XCB_ATOM_WM_CLASS,
                                         XCB_ATOM_STRING,
                                         KSD_X11_MAX_TEXT / 4u);
}

static uint32_t reply_cardinal(xcb_get_property_reply_t *reply,
                               uint32_t fallback, bool *present)
{
    uint32_t value = fallback;

    if (present != NULL)
        *present = false;
    if (reply == NULL)
        return value;
    if (xcb_get_property_value_length(reply) >= 4) {
        memcpy(&value, xcb_get_property_value(reply), sizeof(value));
        if (present != NULL)
            *present = true;
    }
    free(reply);
    return value;
}

/* One window object, in the field order the compositor providers emit, so a
 * consumer parses one format whichever backend answered. */
static bool append_window(ksd_buffer *out, xcb_connection_t *c,
                          const x11_atoms *atoms, xcb_window_t window,
                          xcb_window_t active, uint32_t current_desktop,
                          window_query *query,
                          xcb_get_property_reply_t *state)
{
    char scratch[512];
    xcb_get_geometry_reply_t *geometry =
        xcb_get_geometry_reply(c, query->geometry, NULL);
    xcb_translate_coordinates_reply_t *place =
        xcb_translate_coordinates_reply(c, query->place, NULL);
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    bool has_desktop = false;

    if (geometry == NULL) {
        free(place);
        return false;
    }
    width = geometry->width;
    height = geometry->height;
    /* A reparenting window manager nests a window inside frames of its own, so
     * the geometry of the window is relative to a parent and is not where it
     * sits on screen. */
    if (place != NULL) {
        x = place->dst_x;
        y = place->dst_y;
        free(place);
    } else {
        x = geometry->x;
        y = geometry->y;
    }
    free(geometry);

    bool minimized = ksd_x11_state_has(state, atoms->state_hidden);
    bool maximized = ksd_x11_state_has(state, atoms->state_max_vert)
        && ksd_x11_state_has(state, atoms->state_max_horz);
    bool above = ksd_x11_state_has(state, atoms->state_above);
    uint32_t desktop = reply_cardinal(ksd_x11_take_property(c, query->desktop),
                                      0u, &has_desktop);
    uint32_t pid = reply_cardinal(ksd_x11_take_property(c, query->pid), 0u,
                                  NULL);
    uint32_t opacity = reply_cardinal(ksd_x11_take_property(c, query->opacity),
                                      0xffffffffu, NULL);
    int written = snprintf(scratch, sizeof(scratch),
                           "{\"id\":\"%u\",\"title\":", (unsigned)window);

    if (written <= 0 || (size_t)written >= sizeof(scratch)
        || !ksd_buffer_bytes(out, scratch, (size_t)written)
        || !ksd_x11_append_text_reply(out,
                                      ksd_x11_take_property(c, query->name))
        || !ksd_buffer_bytes(out, ",\"appId\":", 9u)
        || !ksd_x11_append_text_reply(out,
                                      ksd_x11_take_property(c, query->app)))
        return false;

    /* The providers report transparency as 0 to 255 and the consumer parses it
     * as an integer. _NET_WM_WINDOW_OPACITY is a 32-bit value where all ones is
     * opaque, so the top byte is the same scale. */
    written = snprintf(scratch, sizeof(scratch),
        ",\"pid\":%u"
        ",\"frame\":{\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d}"
        ",\"client\":{\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d}"
        ",\"buffer\":null"
        ",\"active\":%s"
        ",\"minimized\":%s"
        ",\"maximized\":%s"
        ",\"visible\":%s"
        ",\"alwaysOnTop\":%s"
        ",\"decorated\":true"
        ",\"transparency\":%u"
        ",\"onCurrentWorkspace\":%s}",
        (unsigned)pid, x, y, width, height, x, y, width, height,
        window == active ? "true" : "false",
        minimized ? "true" : "false",
        maximized ? "true" : "false",
        minimized ? "false" : "true",
        above ? "true" : "false",
        (unsigned)(opacity >> 24u),
        (!has_desktop || desktop == 0xffffffffu || desktop == current_desktop)
            ? "true" : "false");
    return written > 0 && (size_t)written < sizeof(scratch)
        && ksd_buffer_bytes(out, scratch, (size_t)written);
}

/* Collects and discards a window whose replies are not going into the output,
 * because every issued request must be consumed or its reply is leaked. */
static void discard_window_query(xcb_connection_t *c, window_query *query)
{
    free(xcb_get_geometry_reply(c, query->geometry, NULL));
    free(xcb_translate_coordinates_reply(c, query->place, NULL));
    free(ksd_x11_take_property(c, query->desktop));
    free(ksd_x11_take_property(c, query->pid));
    free(ksd_x11_take_property(c, query->opacity));
    free(ksd_x11_take_property(c, query->name));
    free(ksd_x11_take_property(c, query->app));
}

/* The providers frame a JSON reply as a length and then the bytes. */
static void json_result(ksd_buffer *out, ksd_operation_result *result)
{
    ksd_buffer framed;

    if (out->length > KSD_MAX_TEXT_BYTES) {
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "the window list is too large");
        ksd_buffer_clear(out);
        return;
    }
    ksd_buffer_init(&framed, KSD_MAX_TEXT_BYTES + 4u);
    if (!ksd_buffer_u32(&framed, (uint32_t)out->length)
        || !ksd_buffer_bytes(&framed, out->data, out->length)
        || !ksd_result_copy(result, framed.data, (uint32_t)framed.length))
        ksd_result_error(result, KSD_STATUS_INTERNAL, 0u, "out of memory");
    ksd_buffer_clear(&framed);
    ksd_buffer_clear(out);
}

void ksd_x11_window_list(ksd_x11 *connection, bool include_hidden,
                         ksd_operation_result *result)
{
    xcb_connection_t *c = connection->connection;
    xcb_window_t root = connection->screen->root;
    x11_atoms atoms;
    uint32_t current_desktop = 0u;
    uint32_t active_value = 0u;
    ksd_buffer out;
    window_query *queries;
    bool ok;
    bool first = true;

    ksd_x11_load_atoms(c, &atoms);
    (void)ksd_x11_cardinal(c, root, atoms.current_desktop, &current_desktop);
    (void)ksd_x11_cardinal(c, root, atoms.active_window, &active_value);

    /* _NET_CLIENT_LIST is the list of windows the window manager manages.
     * Walking the children of the root instead would report override-redirect
     * windows, which are menus, tooltips and drag surfaces, and are not what a
     * caller asking for the window list means. */
    xcb_get_property_reply_t *reply = ksd_x11_property(c, root,
        atoms.client_list, XCB_ATOM_WINDOW, KSD_X11_MAX_WINDOWS);
    if (reply == NULL) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                         "no window manager publishes _NET_CLIENT_LIST");
        return;
    }
    int length = xcb_get_property_value_length(reply);
    const xcb_window_t *windows = xcb_get_property_value(reply);
    size_t count = length > 0 ? (size_t)length / sizeof(xcb_window_t) : 0u;

    if (count > KSD_X11_MAX_WINDOWS)
        count = KSD_X11_MAX_WINDOWS;
    queries = count != 0u ? calloc(count, sizeof(*queries)) : NULL;
    if (count != 0u && queries == NULL) {
        free(reply);
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "out of memory");
        return;
    }

    /* Issue everything, then read everything. */
    for (size_t index = 0u; index < count; index++)
        issue_window_query(c, &atoms, root, windows[index], &queries[index]);

    ksd_buffer_init(&out, KSD_MAX_TEXT_BYTES);
    ok = ksd_buffer_bytes(&out, "{\"ok\":true,\"windows\":[", 22u);
    for (size_t index = 0u; index < count; index++) {
        xcb_get_property_reply_t *state =
            ksd_x11_take_property(c, queries[index].state);
        bool hidden = ksd_x11_state_has(state, atoms.state_hidden);

        if (!ok || (!include_hidden && hidden)) {
            discard_window_query(c, &queries[index]);
            free(state);
            continue;
        }
        if (!first)
            ok = ksd_buffer_bytes(&out, ",", 1u);
        ok = ok && append_window(&out, c, &atoms, windows[index],
                                 (xcb_window_t)active_value, current_desktop,
                                 &queries[index], state);
        free(state);
        first = false;
    }
    free(queries);
    free(reply);
    ok = ok && ksd_buffer_bytes(&out, "]}", 2u);
    if (!ok) {
        ksd_buffer_clear(&out);
        ksd_result_error(result, KSD_STATUS_INTERNAL, 0u,
                         "could not build the window list");
        return;
    }
    json_result(&out, result);
}

void ksd_x11_window_active(ksd_x11 *connection, ksd_operation_result *result)
{
    xcb_connection_t *c = connection->connection;
    xcb_window_t root = connection->screen->root;
    x11_atoms atoms;
    uint32_t current_desktop = 0u;
    uint32_t active_value = 0u;
    ksd_buffer out;
    bool ok;

    ksd_x11_load_atoms(c, &atoms);
    (void)ksd_x11_cardinal(c, root, atoms.current_desktop, &current_desktop);
    ksd_buffer_init(&out, KSD_MAX_TEXT_BYTES);
    if (!ksd_x11_cardinal(c, root, atoms.active_window, &active_value)
        || active_value == 0u) {
        /* No active window is an answer rather than a failure: a session with
         * every window minimised has none. */
        ok = ksd_buffer_bytes(&out, "{\"ok\":true,\"window\":null}", 25u);
    } else {
        window_query query;
        xcb_get_property_reply_t *state;

        issue_window_query(c, &atoms, root, (xcb_window_t)active_value,
                           &query);
        state = ksd_x11_take_property(c, query.state);
        ok = ksd_buffer_bytes(&out, "{\"ok\":true,\"window\":", 20u)
            && append_window(&out, c, &atoms, (xcb_window_t)active_value,
                             (xcb_window_t)active_value, current_desktop,
                             &query, state)
            && ksd_buffer_bytes(&out, "}", 1u);
        free(state);
    }
    if (!ok) {
        ksd_buffer_clear(&out);
        ksd_result_error(result, KSD_STATUS_INTERNAL, 0u,
                         "could not build the active window");
        return;
    }
    json_result(&out, result);
}
