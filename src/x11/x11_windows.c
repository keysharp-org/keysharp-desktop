#include "x11_internal.h"
#include "x11_extended.h"

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
    xcb_get_window_attributes_cookie_t attributes;
    xcb_get_property_cookie_t extents;
    xcb_get_property_cookie_t fallback_name;
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
    query->attributes = xcb_get_window_attributes(c, window);
    query->extents = ksd_x11_property_cookie(c, window, atoms->frame_extents, XCB_ATOM_CARDINAL, 4u);
    query->fallback_name = ksd_x11_property_cookie(c, window, XCB_ATOM_WM_NAME, XCB_ATOM_ANY, KSD_X11_MAX_TEXT / 4u);
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

static xcb_get_property_reply_t *inherited_class(xcb_connection_t *c,
                                                 xcb_window_t window,
                                                 xcb_get_property_reply_t *reply)
{
    for (unsigned depth = 0u; depth <= 16u; depth++) {
        if (reply != NULL && reply->format == 8u) {
            const uint8_t *bytes = xcb_get_property_value(reply);
            int length = xcb_get_property_value_length(reply);
            for (int i = 0; i < length; i++) if (bytes[i] != 0u) return reply;
        }
        free(reply);
        reply = NULL;
        if (depth == 16u) break;
        xcb_query_tree_reply_t *tree = xcb_query_tree_reply(c, xcb_query_tree(c, window), NULL);
        if (tree == NULL) break;
        xcb_window_t parent = tree->parent;
        bool done = parent == tree->root || parent == XCB_WINDOW_NONE || parent == window;
        free(tree);
        if (done) break;
        window = parent;
        reply = ksd_x11_property(c, window, XCB_ATOM_WM_CLASS,
                                  XCB_ATOM_STRING, KSD_X11_MAX_TEXT / 4u);
    }
    return NULL;
}

static xcb_atom_t icccm_state_atom(xcb_connection_t *c)
{
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(c,
        xcb_intern_atom(c, 1u, 8u, "WM_STATE"), NULL);
    xcb_atom_t atom = reply != NULL ? reply->atom : XCB_ATOM_NONE;
    free(reply);
    return atom;
}

static xcb_window_t managed_child(xcb_connection_t *c, xcb_window_t window,
                                   xcb_atom_t wm_state, unsigned depth, unsigned *budget)
{
    if (wm_state == XCB_ATOM_NONE || depth >= 64u || *budget == 0u) return XCB_WINDOW_NONE;
    (*budget)--;
    xcb_get_property_reply_t *state = ksd_x11_property(c, window, wm_state, XCB_ATOM_ANY, 2u);
    bool managed = state != NULL && state->format == 32u && xcb_get_property_value_length(state) >= 4;
    free(state);
    if (managed) return window;
    xcb_query_tree_reply_t *tree = xcb_query_tree_reply(c, xcb_query_tree(c, window), NULL);
    if (tree == NULL) return XCB_WINDOW_NONE;
    xcb_window_t found = XCB_WINDOW_NONE;
    const xcb_window_t *children = xcb_query_tree_children(tree);
    for (int i = xcb_query_tree_children_length(tree) - 1; i >= 0 && found == XCB_WINDOW_NONE && *budget != 0u; i--)
        found = managed_child(c, children[i], wm_state, depth + 1u, budget);
    free(tree);
    return found;
}

static xcb_window_t client_toplevel(xcb_connection_t *c, xcb_window_t window)
{
    xcb_atom_t wm_state = icccm_state_atom(c);
    for (unsigned depth = 0u; depth < 64u; depth++) {
        xcb_get_property_reply_t *state = ksd_x11_property(c, window, wm_state, XCB_ATOM_ANY, 2u);
        bool managed = state != NULL && state->format == 32u && xcb_get_property_value_length(state) >= 4;
        free(state);
        if (managed) return window;
        xcb_query_tree_reply_t *tree = xcb_query_tree_reply(c, xcb_query_tree(c, window), NULL);
        if (tree == NULL) return window;
        xcb_window_t parent = tree->parent;
        bool done = parent == tree->root || parent == XCB_WINDOW_NONE || parent == window;
        free(tree);
        if (done) break;
        window = parent;
    }
    unsigned budget = KSD_X11_MAX_WINDOWS;
    xcb_window_t client = managed_child(c, window, wm_state, 0u, &budget);
    return client == XCB_WINDOW_NONE ? window : client;
}

static xcb_window_t active_window(ksd_x11 *connection, const x11_atoms *atoms)
{
    uint32_t active = XCB_WINDOW_NONE;
    if (ksd_x11_cardinal(connection->connection, connection->screen->root, atoms->active_window, &active)
        && active != XCB_WINDOW_NONE) return active;
    xcb_get_input_focus_reply_t *focus = xcb_get_input_focus_reply(connection->connection,
        xcb_get_input_focus(connection->connection), NULL);
    active = focus != NULL ? focus->focus : XCB_WINDOW_NONE;
    free(focus);
    if (active == XCB_INPUT_FOCUS_NONE || active == XCB_INPUT_FOCUS_POINTER_ROOT
        || active == connection->screen->root) return XCB_WINDOW_NONE;
    return client_toplevel(connection->connection, active);
}

static bool add_handle(xcb_window_t *windows, size_t *count, xcb_window_t window)
{
    if (window == XCB_WINDOW_NONE) return true;
    for (size_t i = 0u; i < *count; i++) if (windows[i] == window) return true;
    if (*count == KSD_X11_MAX_WINDOWS) return false;
    windows[(*count)++] = window;
    return true;
}

/* Preserve EWMH stacking order, then include root windows that the window
 * manager leaves out, such as tooltips. Without EWMH the root tree supplies
 * the list, promoting reparenting frames to their WM_STATE client. */
static bool enumerate_windows(ksd_x11 *connection, const x11_atoms *atoms,
                                xcb_window_t *windows, size_t *count)
{
    xcb_connection_t *c = connection->connection;
    xcb_window_t root = connection->screen->root;
    *count = 0u;
    xcb_get_property_reply_t *reply = ksd_x11_property(c, root,
        atoms->client_list_stacking, XCB_ATOM_WINDOW, KSD_X11_MAX_WINDOWS);
    if (reply == NULL) reply = ksd_x11_property(c, root,
        atoms->client_list, XCB_ATOM_WINDOW, KSD_X11_MAX_WINDOWS);
    bool ok = true;
    if (reply != NULL && reply->format == 32u) {
        const xcb_window_t *ids = xcb_get_property_value(reply);
        size_t length = (size_t)xcb_get_property_value_length(reply) / sizeof(*ids);
        for (size_t i = 0u; ok && i < length; i++) ok = add_handle(windows, count, ids[i]);
        ok = ok && reply->bytes_after == 0u;
    }
    free(reply);
    xcb_query_tree_reply_t *tree = xcb_query_tree_reply(c, xcb_query_tree(c, root), NULL);
    if (tree == NULL) return false;
    xcb_atom_t wm_state = icccm_state_atom(c);
    const xcb_window_t *children = xcb_query_tree_children(tree);
    unsigned budget = KSD_X11_MAX_WINDOWS;
    for (int i = 0; ok && i < xcb_query_tree_children_length(tree); i++) {
        bool listed = false;
        for (size_t j = 0u; j < *count; j++) if (windows[j] == children[i]) { listed = true; break; }
        if (listed) continue;
        xcb_window_t client = managed_child(c, children[i], wm_state, 0u, &budget);
        if (client == XCB_WINDOW_NONE && budget == 0u) { ok = false; break; }
        ok = add_handle(windows, count, client == XCB_WINDOW_NONE ? children[i] : client);
    }
    free(tree);
    return ok && !ksd_x11_connection_failed(connection);
}

/* One window object, in the field order the compositor providers emit, so a
 * consumer parses one format whichever backend answered. */
static bool append_window(ksd_buffer *out, xcb_connection_t *c,
                          const x11_atoms *atoms, xcb_window_t window,
                          xcb_window_t active, uint32_t current_desktop,
                          window_query *query,
                          xcb_get_property_reply_t *state,
                          bool include_hidden, bool *included)
{
    xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(c, query->geometry, NULL);
    xcb_get_window_attributes_reply_t *attributes = xcb_get_window_attributes_reply(c, query->attributes, NULL);
    xcb_translate_coordinates_reply_t *place = xcb_translate_coordinates_reply(c, query->place, NULL);
    xcb_get_property_reply_t *extents = ksd_x11_take_property(c, query->extents);
    xcb_get_property_reply_t *name = ksd_x11_take_property(c, query->name);
    xcb_get_property_reply_t *fallback = ksd_x11_take_property(c, query->fallback_name);
    xcb_get_property_reply_t *app = ksd_x11_take_property(c, query->app);
    bool has_desktop = false, has_pid = false;
    uint32_t desktop = reply_cardinal(ksd_x11_take_property(c, query->desktop), 0u, &has_desktop);
    uint32_t pid = reply_cardinal(ksd_x11_take_property(c, query->pid), 0u, &has_pid);
    uint32_t opacity = reply_cardinal(ksd_x11_take_property(c, query->opacity), 0xffffffffu, NULL);
    bool ok = g != NULL && attributes != NULL && place != NULL;
    char scratch[1024];
    if (included != NULL) *included = false;
    if (included != NULL && (!ok || (!include_hidden && attributes->map_state != XCB_MAP_STATE_VIEWABLE))) {
        ok = true;
    } else if (ok) {
        if (included != NULL) *included = true;
        app = inherited_class(c, window, app);
        int32_t x = place->dst_x, y = place->dst_y;
        uint32_t borders[4] = { 0u };
        if (extents != NULL && extents->format == 32 && xcb_get_property_value_length(extents) >= 16)
            memcpy(borders, xcb_get_property_value(extents), sizeof(borders));
        for (unsigned i = 0u; i < 4u; i++) if (borders[i] > 32768u) borders[i] = 0u;
        bool minimized = ksd_x11_state_has(state, atoms->state_hidden);
        bool maximized = ksd_x11_state_has(state, atoms->state_max_vert)
            && ksd_x11_state_has(state, atoms->state_max_horz);
        bool above = ksd_x11_state_has(state, atoms->state_above);
        int size = snprintf(scratch, sizeof(scratch), "{\"id\":\"%u\",\"title\":", window);
        ok = size > 0 && (size_t)size < sizeof(scratch) && ksd_buffer_bytes(out, scratch, (size_t)size);
        xcb_get_property_reply_t *title = name != NULL ? name : fallback;
        if (title == name) name = NULL; else fallback = NULL;
        bool title_known = title != NULL;
        bool class_known = app != NULL;
        /* WM_CLASS contains instance then class. The class is the script's
         * ahk_class value; applications without it retain their instance. */
        if (app != NULL) {
            char *bytes = xcb_get_property_value(app);
            int length = xcb_get_property_value_length(app);
            char *separator = length > 0 ? memchr(bytes, 0, (size_t)length) : NULL;
            if (separator != NULL && separator + 1 < bytes + length && separator[1] != 0) {
                size_t remaining = (size_t)(bytes + length - separator - 1);
                memmove(bytes, separator + 1, remaining);
                memset(bytes + remaining, 0, (size_t)length - remaining);
            }
        }
        bool title_ok = ksd_x11_append_text_reply(out, title);
        ok = ok && title_ok && ksd_buffer_bytes(out, ",\"appId\":", 9u);
        bool app_ok = ksd_x11_append_text_reply(out, app); app = NULL;
        ok = ok && app_ok;
        size = snprintf(scratch, sizeof(scratch),
            ",\"pid\":%u,\"frame\":{\"x\":%d,\"y\":%d,\"width\":%u,\"height\":%u}"
            ",\"client\":{\"x\":%d,\"y\":%d,\"width\":%u,\"height\":%u}"
            ",\"active\":%s,\"minimized\":%s,\"maximized\":%s,\"visible\":%s"
            ",\"alwaysOnTop\":%s,\"decorated\":%s,\"transparency\":%u,\"onCurrentWorkspace\":%s"
            ",\"validFields\":[\"frame\",\"client\",\"visible\",\"transparency\"%s%s%s%s]}",
            pid, x-(int32_t)borders[0], y-(int32_t)borders[2],
            g->width+borders[0]+borders[1], g->height+borders[2]+borders[3],
            x, y, g->width, g->height, window == active ? "true" : "false",
            minimized ? "true" : "false", maximized ? "true" : "false",
            attributes->map_state == XCB_MAP_STATE_VIEWABLE ? "true" : "false",
            above ? "true" : "false", (borders[0]|borders[1]|borders[2]|borders[3]) != 0u ? "true" : "false",
            opacity >> 24u, (!has_desktop || desktop == UINT32_MAX || desktop == current_desktop) ? "true" : "false",
            title_known ? ",\"title\"" : "", class_known ? ",\"appId\"" : "", has_pid ? ",\"pid\"" : "",
            state != NULL ? ",\"minimized\",\"maximized\",\"alwaysOnTop\"" : "");
        ok = ok && size > 0 && (size_t)size < sizeof(scratch) && ksd_buffer_bytes(out, scratch, (size_t)size);
    }
    free(g); free(attributes); free(place); free(extents); free(name); free(fallback); free(app);
    return ok;
}

/* Collects and discards a window whose replies are not going into the output,
 * because every issued request must be consumed or its reply is leaked. */
static void discard_window_query(xcb_connection_t *c, window_query *query)
{
    free(xcb_get_window_attributes_reply(c, query->attributes, NULL));
    free(ksd_x11_take_property(c, query->extents));
    free(ksd_x11_take_property(c, query->fallback_name));
    free(xcb_get_geometry_reply(c, query->geometry, NULL));
    free(xcb_translate_coordinates_reply(c, query->place, NULL));
    free(ksd_x11_take_property(c, query->desktop));
    free(ksd_x11_take_property(c, query->pid));
    free(ksd_x11_take_property(c, query->opacity));
    free(ksd_x11_take_property(c, query->name));
    free(ksd_x11_take_property(c, query->app));
}

/* The providers frame a JSON reply as a length and then the bytes. */
void ksd_x11_json_result(ksd_buffer *out, ksd_operation_result *result)
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

/* Handles carry no title, class, pid, geometry or other window metadata. */
void ksd_x11_window_handles(ksd_x11 *connection, ksd_operation_result *result)
{
    x11_atoms atoms;
    xcb_window_t windows[KSD_X11_MAX_WINDOWS];
    size_t count;
    ksd_buffer out;
    bool ok;
    bool first = true;
    ksd_x11_load_atoms(connection->connection, &atoms);
    if (!enumerate_windows(connection, &atoms, windows, &count)) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u, "could not enumerate X11 windows");
        return;
    }
    ksd_buffer_init(&out, KSD_MAX_TEXT_BYTES);
    ok = ksd_buffer_bytes(&out, "{\"ok\":true,\"handles\":[", 22u);
    for (size_t index = 0u; ok && index < count; index++) {
        char text[32];
        int written = snprintf(text, sizeof(text), first ? "\"%u\"" : ",\"%u\"",
                               (unsigned)windows[index]);

        first = false;
        ok = written > 0 && (size_t)written < sizeof(text)
            && ksd_buffer_bytes(&out, text, (size_t)written);
    }
    ok = ok && ksd_buffer_bytes(&out, "]}", 2u);
    if (!ok) {
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "the handle list is too large");
        ksd_buffer_clear(&out);
        return;
    }
    ksd_x11_json_result(&out, result);
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
    active_value = active_window(connection, &atoms);

    xcb_window_t windows[KSD_X11_MAX_WINDOWS];
    size_t count;
    if (!enumerate_windows(connection, &atoms, windows, &count)) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u, "could not enumerate X11 windows");
        return;
    }
    queries = count != 0u ? calloc(count, sizeof(*queries)) : NULL;
    if (count != 0u && queries == NULL) {
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
        size_t checkpoint = out.length;
        bool included = false;
        if (!first && !ksd_buffer_bytes(&out, ",", 1u)) {
            discard_window_query(c, &queries[index]);
            free(state);
            ok = false;
            continue;
        }
        ok = append_window(&out, c, &atoms, windows[index],
                                 (xcb_window_t)active_value, current_desktop,
                                 &queries[index], state, include_hidden, &included);
        free(state);
        if (included) first = false;
        else out.length = checkpoint;
    }
    free(queries);
    ok = ok && ksd_buffer_bytes(&out, "]}", 2u);
    if (!ok) {
        ksd_buffer_clear(&out);
        ksd_result_error(result, KSD_STATUS_INTERNAL, 0u,
                         "could not build the window list");
        return;
    }
    ksd_x11_json_result(&out, result);
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
    active_value = active_window(connection, &atoms);
    if (active_value == XCB_WINDOW_NONE) {
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
                             &query, state, true, NULL)
            && ksd_buffer_bytes(&out, "}", 1u);
        free(state);
    }
    if (!ok) {
        ksd_buffer_clear(&out);
        ksd_result_error(result, KSD_STATUS_INTERNAL, 0u,
                         "could not build the active window");
        return;
    }
    ksd_x11_json_result(&out, result);
}

void ksd_x11_window_query(ksd_x11 *connection, uint32_t window,
                           ksd_operation_result *result)
{
    xcb_connection_t *c = connection->connection;
    x11_atoms atoms;
    uint32_t active = 0u, desktop = 0u;
    window_query query;
    ksd_buffer out;
    ksd_x11_load_atoms(c, &atoms);
    active = active_window(connection, &atoms);
    (void)ksd_x11_cardinal(c, connection->screen->root, atoms.current_desktop, &desktop);
    issue_window_query(c, &atoms, connection->screen->root, window, &query);
    xcb_get_property_reply_t *state = ksd_x11_take_property(c, query.state);
    ksd_buffer_init(&out, KSD_MAX_TEXT_BYTES);
    bool ok = ksd_buffer_bytes(&out, "{\"ok\":true,\"window\":", 20u)
        && append_window(&out, c, &atoms, window, active, desktop, &query, state, true, NULL);
    free(state);
    if (!ok) {
        ksd_buffer_clear(&out);
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u, "the window no longer exists");
        return;
    }
    xcb_query_tree_reply_t *tree = xcb_query_tree_reply(c, xcb_query_tree(c, window), NULL);
    xcb_window_t parent = tree != NULL ? tree->parent : XCB_WINDOW_NONE;
    free(tree);
    xcb_window_t top = client_toplevel(c, window);
    char relationships[128];
    int written = snprintf(relationships, sizeof(relationships),
        ",\"parent\":\"%u\",\"topLevel\":\"%u\"}}", parent, top);
    out.length--; /* Extend the window object before closing its envelope. */
    if (written <= 0 || (size_t)written >= sizeof(relationships)
        || !ksd_buffer_bytes(&out, relationships, (size_t)written)) {
        ksd_buffer_clear(&out);
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u, "window result is too large");
        return;
    }
    ksd_x11_json_result(&out, result);
}
