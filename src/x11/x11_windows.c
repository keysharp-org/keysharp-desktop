#include "x11_internal.h"

#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One window object, in the field order the compositor providers emit, so a
 * consumer parses one format whichever backend answered. */
static bool append_window(ksd_buffer *out, xcb_connection_t *c,
                          const x11_atoms *atoms, xcb_window_t window,
                          xcb_window_t active, uint32_t current_desktop)
{
    char scratch[512];
    xcb_get_geometry_reply_t *geometry =
        xcb_get_geometry_reply(c, xcb_get_geometry(c, window), NULL);
    xcb_translate_coordinates_reply_t *place;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;

    if (geometry == NULL)
        return false;
    width = geometry->width;
    height = geometry->height;

    /* A reparenting window manager nests a window inside frames of its own, so
     * the geometry of the window is relative to a parent and is not where it
     * sits on screen. Translating to the root is the only way to learn that
     * without assuming how deep the nesting goes. */
    place = xcb_translate_coordinates_reply(c,
        xcb_translate_coordinates(c, window, geometry->root, 0, 0), NULL);
    if (place != NULL) {
        x = place->dst_x;
        y = place->dst_y;
        free(place);
    } else {
        x = geometry->x;
        y = geometry->y;
    }
    free(geometry);

    bool minimized = ksd_x11_has_state(c, atoms, window, atoms->state_hidden);
    bool maximized = ksd_x11_has_state(c, atoms, window, atoms->state_max_vert)
        && ksd_x11_has_state(c, atoms, window, atoms->state_max_horz);
    bool above = ksd_x11_has_state(c, atoms, window, atoms->state_above);
    uint32_t pid = 0u;
    uint32_t desktop = 0u;
    uint32_t opacity = 0xffffffffu;
    bool has_desktop = ksd_x11_cardinal(c, window, atoms->wm_desktop, &desktop);
    int written;

    (void)ksd_x11_cardinal(c, window, atoms->wm_pid, &pid);
    (void)ksd_x11_cardinal(c, window, atoms->opacity, &opacity);

    written = snprintf(scratch, sizeof(scratch), "{\"id\":\"%u\",\"title\":",
                       (unsigned)window);
    if (written <= 0 || (size_t)written >= sizeof(scratch)
        || !ksd_buffer_bytes(out, scratch, (size_t)written)
        || !ksd_x11_append_text(out, c, window, atoms->wm_name,
                                atoms->utf8_string)
        || !ksd_buffer_bytes(out, ",\"appId\":", 9u)
        || !ksd_x11_append_text(out, c, window, XCB_ATOM_WM_CLASS,
                                XCB_ATOM_STRING))
        return false;

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
        ",\"transparency\":%.6f"
        ",\"onCurrentWorkspace\":%s}",
        (unsigned)pid, x, y, width, height, x, y, width, height,
        window == active ? "true" : "false",
        minimized ? "true" : "false",
        maximized ? "true" : "false",
        minimized ? "false" : "true",
        above ? "true" : "false",
        (double)opacity / 4294967295.0,
        (!has_desktop || desktop == 0xffffffffu || desktop == current_desktop)
            ? "true" : "false");
    return written > 0 && (size_t)written < sizeof(scratch)
        && ksd_buffer_bytes(out, scratch, (size_t)written);
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
    x11_atoms atoms;
    uint32_t current_desktop = 0u;
    uint32_t active_value = 0u;
    ksd_buffer out;
    bool ok;
    bool first = true;

    ksd_x11_load_atoms(c, &atoms);
    (void)ksd_x11_cardinal(c, connection->screen->root, atoms.current_desktop,
                           &current_desktop);
    (void)ksd_x11_cardinal(c, connection->screen->root, atoms.active_window,
                           &active_value);

    /* _NET_CLIENT_LIST is the list of windows the window manager manages.
     * Walking the children of the root instead would report override-redirect
     * windows, which are menus, tooltips and drag surfaces, and are not what a
     * caller asking for the window list means. */
    xcb_get_property_reply_t *reply = ksd_x11_property(c,
        connection->screen->root, atoms.client_list, XCB_ATOM_WINDOW,
        KSD_X11_MAX_WINDOWS);
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

    ksd_buffer_init(&out, KSD_MAX_TEXT_BYTES);
    ok = ksd_buffer_bytes(&out, "{\"ok\":true,\"windows\":[", 22u);
    for (size_t index = 0u; ok && index < count; index++) {
        if (!include_hidden
            && ksd_x11_has_state(c, &atoms, windows[index],
                                 atoms.state_hidden))
            continue;
        if (!first)
            ok = ksd_buffer_bytes(&out, ",", 1u);
        ok = ok && append_window(&out, c, &atoms, windows[index],
                                 (xcb_window_t)active_value, current_desktop);
        first = false;
    }
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
    x11_atoms atoms;
    uint32_t current_desktop = 0u;
    uint32_t active_value = 0u;
    ksd_buffer out;
    bool ok;

    ksd_x11_load_atoms(c, &atoms);
    (void)ksd_x11_cardinal(c, connection->screen->root, atoms.current_desktop,
                           &current_desktop);
    ksd_buffer_init(&out, KSD_MAX_TEXT_BYTES);
    if (!ksd_x11_cardinal(c, connection->screen->root, atoms.active_window,
                          &active_value)
        || active_value == 0u) {
        /* No active window is an answer rather than a failure: a session with
         * every window minimised has none. */
        ok = ksd_buffer_bytes(&out, "{\"ok\":true,\"window\":null}", 25u);
    } else {
        ok = ksd_buffer_bytes(&out, "{\"ok\":true,\"window\":", 20u)
            && append_window(&out, c, &atoms, (xcb_window_t)active_value,
                             (xcb_window_t)active_value, current_desktop)
            && ksd_buffer_bytes(&out, "}", 1u);
    }
    if (!ok) {
        ksd_buffer_clear(&out);
        ksd_result_error(result, KSD_STATUS_INTERNAL, 0u,
                         "could not build the active window");
        return;
    }
    json_result(&out, result);
}
