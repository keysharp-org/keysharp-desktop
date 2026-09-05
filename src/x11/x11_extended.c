#include "x11_extended.h"
#include "x11_internal.h"
#include "protocol.h"

#include <limits.h>
#include <errno.h>
#include <sys/random.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/randr.h>
#include <xcb/shape.h>
#include <xcb/xkb.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-x11.h>

static void finish(ksd_buffer *out, bool ok, ksd_operation_result *result)
{
    if (ok)
        (void)ksd_result_take_framed_text(out, result, KSD_STATUS_INTERNAL,
                                          "out of memory");
    else {
        ksd_buffer_clear(out);
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "desktop result is too large");
    }
}

static bool checked(ksd_x11 *connection, xcb_void_cookie_t cookie,
                    ksd_operation_result *result)
{
    xcb_generic_error_t *error = xcb_request_check(connection->connection, cookie);
    if (error != NULL) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, error->error_code,
                         "the X server rejected the operation");
        free(error);
        return false;
    }
    return ksd_result_copy(result, NULL, 0u);
}

void ksd_x11_window_children(ksd_x11 *connection, uint32_t window,
                             ksd_operation_result *result)
{
    xcb_query_tree_reply_t *tree = xcb_query_tree_reply(connection->connection,
        xcb_query_tree(connection->connection, window), NULL);
    if (tree == NULL) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u, "the window no longer exists");
        return;
    }
    int count = xcb_query_tree_children_length(tree);
    xcb_window_t *children = xcb_query_tree_children(tree);
    ksd_buffer out;
    ksd_buffer_init(&out, KSD_MAX_TEXT_BYTES);
    bool ok = count >= 0 && (unsigned)count <= KSD_X11_MAX_WINDOWS
        && ksd_buffer_bytes(&out, "{\"ok\":true,\"handles\":[", 22u);
    for (int i = 0; ok && i < count; i++) {
        char value[32];
        int written = snprintf(value, sizeof(value), i == 0 ? "\"%u\"" : ",\"%u\"", children[i]);
        ok = written > 0 && (size_t)written < sizeof(value)
            && ksd_buffer_bytes(&out, value, (size_t)written);
    }
    free(tree);
    finish(&out, ok && ksd_buffer_bytes(&out, "]}", 2u), result);
}

static bool contains_point(ksd_x11 *connection, xcb_window_t window,
                            int32_t x, int32_t y, bool shape)
{
    xcb_connection_t *c = connection->connection;
    xcb_get_window_attributes_cookie_t ac = xcb_get_window_attributes(c, window);
    xcb_get_geometry_cookie_t gc = xcb_get_geometry(c, window);
    xcb_translate_coordinates_cookie_t pc = xcb_translate_coordinates(c, window,
        connection->screen->root, 0, 0);
    xcb_get_window_attributes_reply_t *a = xcb_get_window_attributes_reply(c, ac, NULL);
    xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(c, gc, NULL);
    xcb_translate_coordinates_reply_t *p = xcb_translate_coordinates_reply(c, pc, NULL);
    bool inside = a != NULL && g != NULL && p != NULL
        && a->map_state == XCB_MAP_STATE_VIEWABLE
        && x >= p->dst_x && y >= p->dst_y
        && (int64_t)x < (int64_t)p->dst_x + g->width
        && (int64_t)y < (int64_t)p->dst_y + g->height;
    if (inside && shape) {
        xcb_shape_get_rectangles_reply_t *rects = xcb_shape_get_rectangles_reply(c,
            xcb_shape_get_rectangles(c, window, XCB_SHAPE_SK_INPUT), NULL);
        if (rects != NULL) {
            inside = false;
            int count = xcb_shape_get_rectangles_rectangles_length(rects);
            const xcb_rectangle_t *values = xcb_shape_get_rectangles_rectangles(rects);
            int32_t local_x = x - p->dst_x, local_y = y - p->dst_y;
            for (int i = 0; i < count && !inside; i++)
                inside = local_x >= values[i].x && local_y >= values[i].y
                    && local_x < (int32_t)values[i].x + values[i].width
                    && local_y < (int32_t)values[i].y + values[i].height;
            free(rects);
        }
    }
    free(a); free(g); free(p);
    return inside;
}

static xcb_window_t at_point(ksd_x11 *connection, xcb_window_t parent,
                             int32_t x, int32_t y, bool shape,
                             unsigned depth, unsigned *budget)
{
    if (depth >= 64u || *budget == 0u) return parent;
    xcb_query_tree_reply_t *tree = xcb_query_tree_reply(connection->connection,
        xcb_query_tree(connection->connection, parent), NULL);
    if (tree == NULL) return parent;
    int count = xcb_query_tree_children_length(tree);
    xcb_window_t *children = xcb_query_tree_children(tree);
    xcb_window_t found = parent;
    for (int i = count - 1; i >= 0 && *budget != 0u; i--) {
        (*budget)--;
        if (contains_point(connection, children[i], x, y, shape)) {
            found = at_point(connection, children[i], x, y, shape, depth + 1u, budget);
            break;
        }
    }
    free(tree);
    return found;
}

static xcb_window_t managed_descendant(ksd_x11 *connection, xcb_window_t window,
                                         xcb_atom_t wm_state, unsigned depth, unsigned *budget)
{
    if (wm_state == XCB_ATOM_NONE || depth >= 64u || *budget == 0u) return XCB_WINDOW_NONE;
    (*budget)--;
    xcb_get_property_reply_t *state = ksd_x11_property(connection->connection,
        window, wm_state, XCB_ATOM_ANY, 2u);
    if (state != NULL) { free(state); return window; }
    xcb_query_tree_reply_t *tree = xcb_query_tree_reply(connection->connection,
        xcb_query_tree(connection->connection, window), NULL);
    if (tree == NULL) return XCB_WINDOW_NONE;
    xcb_window_t found = XCB_WINDOW_NONE;
    const xcb_window_t *children = xcb_query_tree_children(tree);
    for (int i = xcb_query_tree_children_length(tree) - 1; i >= 0 && found == XCB_WINDOW_NONE; i--)
        found = managed_descendant(connection, children[i], wm_state, depth + 1u, budget);
    free(tree);
    return found;
}

void ksd_x11_window_at_point(ksd_x11 *connection, int32_t x, int32_t y,
                             bool deepest, ksd_operation_result *result)
{
    const xcb_query_extension_reply_t *extension = xcb_get_extension_data(
        connection->connection, &xcb_shape_id);
    bool shape = extension != NULL && extension->present;
    unsigned budget = KSD_X11_MAX_WINDOWS;
    xcb_window_t window = at_point(connection, connection->screen->root,
                                    x, y, shape, 0u, &budget);
    if (window == connection->screen->root) {
        ksd_buffer out;
        ksd_buffer_init(&out, 64u);
        finish(&out, ksd_buffer_bytes(&out, "{\"ok\":true,\"window\":null}", 25u), result);
        return;
    }
    if (!deepest) {
        xcb_connection_t *c = connection->connection;
        xcb_intern_atom_reply_t *atom = xcb_intern_atom_reply(c,
            xcb_intern_atom(c, 1u, 8u, "WM_STATE"), NULL);
        for (unsigned depth = 0u; depth < 64u; depth++) {
            xcb_get_property_reply_t *state = atom == NULL ? NULL
                : ksd_x11_property(c, window, atom->atom, XCB_ATOM_ANY, 2u);
            if (state != NULL) { free(state); break; }
            xcb_query_tree_reply_t *tree = xcb_query_tree_reply(c, xcb_query_tree(c, window), NULL);
            if (tree == NULL) break;
            xcb_window_t parent = tree->parent;
            bool done = parent == tree->root || parent == XCB_WINDOW_NONE || parent == window;
            free(tree);
            if (done) break;
            window = parent;
        }
        if (atom != NULL) {
            budget = KSD_X11_MAX_WINDOWS;
            xcb_window_t client = managed_descendant(connection, window, atom->atom, 0u, &budget);
            if (client != XCB_WINDOW_NONE) window = client;
        }
        free(atom);
    }
    ksd_x11_window_query(connection, window, result);
}

void ksd_x11_mouse_move_absolute(ksd_x11 *connection, int32_t x, int32_t y,
                                 ksd_operation_result *result)
{
    if (x < INT16_MIN || x > INT16_MAX || y < INT16_MIN || y > INT16_MAX) {
        ksd_result_error(result, KSD_STATUS_INVALID_REQUEST, 0u, "pointer coordinates exceed X11 range");
        return;
    }
    (void)checked(connection, xcb_warp_pointer_checked(connection->connection,
        XCB_WINDOW_NONE, connection->screen->root, 0, 0, 0, 0, (int16_t)x, (int16_t)y), result);
}

void ksd_x11_window_set_title(ksd_x11 *connection, uint32_t window,
                              const uint8_t *value, uint32_t length,
                              ksd_operation_result *result)
{
    xcb_connection_t *c = connection->connection;
    xcb_intern_atom_cookie_t name = xcb_intern_atom(c, 0u, 12u, "_NET_WM_NAME");
    xcb_intern_atom_cookie_t utf8 = xcb_intern_atom(c, 0u, 11u, "UTF8_STRING");
    xcb_intern_atom_reply_t *n = xcb_intern_atom_reply(c, name, NULL);
    xcb_intern_atom_reply_t *u = xcb_intern_atom_reply(c, utf8, NULL);
    if (n == NULL || u == NULL)
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u, "the X server is unavailable");
    else if (checked(connection, xcb_change_property_checked(c, XCB_PROP_MODE_REPLACE,
                 window, n->atom, u->atom, 8u, length, value), result))
        (void)checked(connection, xcb_change_property_checked(c, XCB_PROP_MODE_REPLACE,
            window, XCB_ATOM_WM_NAME, u->atom, 8u, length, value), result);
    free(n); free(u);
}

void ksd_x11_window_set_visible(ksd_x11 *connection, uint32_t window,
                                bool visible, ksd_operation_result *result)
{
    (void)checked(connection, visible ? xcb_map_window_checked(connection->connection, window)
        : xcb_unmap_window_checked(connection->connection, window), result);
}

void ksd_x11_window_redraw(ksd_x11 *connection, uint32_t window, ksd_operation_result *result)
{
    (void)checked(connection, xcb_clear_area_checked(connection->connection,
        1u, window, 0, 0, 0u, 0u), result);
}

void ksd_x11_window_click(ksd_x11 *connection, uint32_t window,
                          int32_t x, int32_t y, uint32_t button, uint32_t count,
                          ksd_operation_result *result)
{
    if (x < INT16_MIN || x > INT16_MAX || y < INT16_MIN || y > INT16_MAX
        || button == 0u || button > 5u || count == 0u || count > 100u) {
        ksd_result_error(result, KSD_STATUS_INVALID_REQUEST, 0u, "invalid client click");
        return;
    }
    xcb_translate_coordinates_reply_t *place = xcb_translate_coordinates_reply(
        connection->connection, xcb_translate_coordinates(connection->connection,
            window, connection->screen->root, (int16_t)x, (int16_t)y), NULL);
    if (place == NULL) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u, "the window no longer exists");
        return;
    }
    xcb_button_press_event_t event = { 0 };
    event.detail = (uint8_t)(button == 2u ? 3u : button == 3u ? 2u : button >= 4u ? button + 4u : button);
    event.root = connection->screen->root;
    event.event = window;
    event.root_x = place->dst_x; event.root_y = place->dst_y;
    event.event_x = (int16_t)x; event.event_y = (int16_t)y;
    event.same_screen = 1u;
    free(place);
    for (uint32_t i = 0u; i < count; i++) {
        event.response_type = XCB_BUTTON_PRESS;
        event.state = 0u;
        if (!checked(connection, xcb_send_event_checked(connection->connection,
                1u, window, XCB_EVENT_MASK_BUTTON_PRESS, (const char *)&event), result)) return;
        event.response_type = XCB_BUTTON_RELEASE;
        event.state = event.detail <= 5u ? (uint16_t)(1u << (event.detail + 7u)) : 0u;
        if (!checked(connection, xcb_send_event_checked(connection->connection,
                1u, window, XCB_EVENT_MASK_BUTTON_RELEASE, (const char *)&event), result)) return;
    }
}

void ksd_x11_window_button(ksd_x11 *connection, uint32_t window,
                           int32_t x, int32_t y, uint32_t button, bool down,
                           ksd_operation_result *result)
{
    if (x < INT16_MIN || x > INT16_MAX || y < INT16_MIN || y > INT16_MAX
        || button == 0u || button > 32u) {
        ksd_result_error(result, KSD_STATUS_INVALID_REQUEST, 0u, "invalid window button event");
        return;
    }
    xcb_translate_coordinates_reply_t *place = xcb_translate_coordinates_reply(
        connection->connection, xcb_translate_coordinates(connection->connection,
            window, connection->screen->root, (int16_t)x, (int16_t)y), NULL);
    if (place == NULL) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u, "the window no longer exists");
        return;
    }
    xcb_button_press_event_t event = { 0 };
    event.response_type = down ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE;
    event.detail = (uint8_t)button;
    event.root = connection->screen->root;
    event.event = window;
    event.root_x = place->dst_x; event.root_y = place->dst_y;
    event.event_x = (int16_t)x; event.event_y = (int16_t)y;
    event.same_screen = 1u;
    event.state = !down && button <= 5u ? (uint16_t)(1u << (button + 7u)) : 0u;
    free(place);
    (void)checked(connection, xcb_send_event_checked(connection->connection, 1u, window,
        down ? XCB_EVENT_MASK_BUTTON_PRESS : XCB_EVENT_MASK_BUTTON_RELEASE, (const char *)&event), result);
}

void ksd_x11_window_focus_child(ksd_x11 *connection, uint32_t window,
                                ksd_operation_result *result)
{
    (void)checked(connection, xcb_set_input_focus_checked(connection->connection,
        XCB_INPUT_FOCUS_PARENT, window, XCB_CURRENT_TIME), result);
}

static void output_mode(xcb_randr_get_screen_resources_current_reply_t *resources,
                          const xcb_randr_get_crtc_info_reply_t *crtc,
                          double *refresh, unsigned *rotation)
{
    *refresh = 0.0; *rotation = 0u;
    if (crtc == NULL) return;
    *rotation = (crtc->rotation & 2u) ? 90u : (crtc->rotation & 4u) ? 180u
        : (crtc->rotation & 8u) ? 270u : 0u;
    if (resources == NULL) return;
    xcb_randr_mode_info_t *modes = xcb_randr_get_screen_resources_current_modes(resources);
    int count = xcb_randr_get_screen_resources_current_modes_length(resources);
    for (int i = 0; i < count; i++) if (modes[i].id == crtc->mode) {
        double total = (double)modes[i].htotal * modes[i].vtotal;
        if (total > 0.0) *refresh = modes[i].dot_clock / total;
        if ((modes[i].mode_flags & XCB_RANDR_MODE_FLAG_INTERLACE) != 0u) *refresh *= 2.0;
        if ((modes[i].mode_flags & XCB_RANDR_MODE_FLAG_DOUBLE_SCAN) != 0u) *refresh /= 2.0;
        break;
    }
}

void ksd_x11_display_list(ksd_x11 *connection, ksd_operation_result *result)
{
    xcb_connection_t *c = connection->connection;
    xcb_window_t root = connection->screen->root;
    const xcb_query_extension_reply_t *extension = xcb_get_extension_data(c, &xcb_randr_id);
    xcb_randr_get_monitors_reply_t *monitors = NULL;
    xcb_randr_get_screen_resources_current_reply_t *resources = NULL;
    if (extension != NULL && extension->present) {
        xcb_randr_query_version_reply_t *version = xcb_randr_query_version_reply(c,
            xcb_randr_query_version(c, 1u, 5u), NULL);
        if (version != NULL && (version->major_version > 1u || version->minor_version >= 5u))
            monitors = xcb_randr_get_monitors_reply(c, xcb_randr_get_monitors(c, root, 1u), NULL);
        free(version);
        resources = xcb_randr_get_screen_resources_current_reply(c,
            xcb_randr_get_screen_resources_current(c, root), NULL);
    }
    ksd_buffer out;
    ksd_buffer_init(&out, KSD_MAX_TEXT_BYTES);
    bool ok = ksd_buffer_bytes(&out, "{\"ok\":true,\"displays\":[", 23u);
    unsigned count = 0u;
    if (monitors != NULL) {
        xcb_randr_monitor_info_iterator_t it = xcb_randr_get_monitors_monitors_iterator(monitors);
        for (; ok && it.rem != 0 && count < 256u; xcb_randr_monitor_info_next(&it)) {
            xcb_randr_monitor_info_t *m = it.data;
            if (m->width == 0u || m->height == 0u) continue;
            xcb_randr_output_t output = m->nOutput == 0u ? 0u : xcb_randr_monitor_info_outputs(m)[0];
            xcb_get_atom_name_reply_t *name = xcb_get_atom_name_reply(c, xcb_get_atom_name(c, m->name), NULL);
            xcb_randr_get_output_info_reply_t *oi = output == 0u ? NULL
                : xcb_randr_get_output_info_reply(c, xcb_randr_get_output_info(c, output, XCB_CURRENT_TIME), NULL);
            xcb_randr_get_crtc_info_reply_t *crtc = oi == NULL || oi->crtc == 0u ? NULL
                : xcb_randr_get_crtc_info_reply(c, xcb_randr_get_crtc_info(c, oi->crtc, XCB_CURRENT_TIME), NULL);
            double refresh = 0.0;
            unsigned rotation = 0u;
            output_mode(resources, crtc, &refresh, &rotation);
            if (count++ != 0u) ok = ksd_buffer_bytes(&out, ",", 1u);
            ok = ok && ksd_buffer_bytes(&out, "{\"name\":", 8u)
                && ksd_buffer_json_string(&out,
                    name != NULL ? xcb_get_atom_name_name(name) : "",
                    name != NULL
                        ? (size_t)xcb_get_atom_name_name_length(name) : 0u,
                    false);
            char data[512];
            int size = snprintf(data, sizeof(data),
                ",\"output\":%u,\"x\":%d,\"y\":%d,\"width\":%u,\"height\":%u,"
                "\"primary\":%s,\"physicalWidth\":%u,\"physicalHeight\":%u,"
                "\"refreshRate\":%.6f,\"orientation\":%u}",
                output, m->x, m->y, m->width, m->height, m->primary ? "true" : "false",
                m->width_in_millimeters, m->height_in_millimeters, refresh, rotation);
            ok = ok && size > 0 && (size_t)size < sizeof(data)
                && ksd_buffer_bytes(&out, data, (size_t)size);
            free(name); free(oi); free(crtc);
        }
    }
    if (count == 0u && resources != NULL) {
        xcb_randr_get_output_primary_reply_t *primary = xcb_randr_get_output_primary_reply(c,
            xcb_randr_get_output_primary(c, root), NULL);
        const xcb_randr_output_t *outputs = xcb_randr_get_screen_resources_current_outputs(resources);
        int output_count = xcb_randr_get_screen_resources_current_outputs_length(resources);
        for (int i = 0; ok && i < output_count && count < 256u; i++) {
            xcb_randr_get_output_info_reply_t *oi = xcb_randr_get_output_info_reply(c,
                xcb_randr_get_output_info(c, outputs[i], resources->config_timestamp), NULL);
            xcb_randr_get_crtc_info_reply_t *crtc = oi == NULL || oi->crtc == 0u ? NULL
                : xcb_randr_get_crtc_info_reply(c, xcb_randr_get_crtc_info(c, oi->crtc, XCB_CURRENT_TIME), NULL);
            if (crtc != NULL && crtc->width != 0u && crtc->height != 0u) {
                double refresh;
                unsigned rotation;
                output_mode(resources, crtc, &refresh, &rotation);
                if (count++ != 0u) ok = ksd_buffer_bytes(&out, ",", 1u);
                ok = ok && ksd_buffer_bytes(&out, "{\"name\":", 8u)
                    && ksd_buffer_json_string(&out,
                        (const char *)xcb_randr_get_output_info_name(oi),
                        (size_t)xcb_randr_get_output_info_name_length(oi),
                        false);
                char data[384];
                int size = snprintf(data, sizeof(data),
                    ",\"output\":%u,\"x\":%d,\"y\":%d,\"width\":%u,\"height\":%u,"
                    "\"primary\":%s,\"physicalWidth\":%u,\"physicalHeight\":%u,"
                    "\"refreshRate\":%.6f,\"orientation\":%u}",
                    outputs[i], crtc->x, crtc->y, crtc->width, crtc->height,
                    primary != NULL && primary->output == outputs[i] ? "true" : "false",
                    oi->mm_width, oi->mm_height, refresh, rotation);
                ok = ok && size > 0 && (size_t)size < sizeof(data)
                    && ksd_buffer_bytes(&out, data, (size_t)size);
            }
            free(oi); free(crtc);
        }
        free(primary);
    }
    if (count == 0u) {
        xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(c, xcb_get_geometry(c, root), NULL);
        char data[256];
        int size = g == NULL ? -1 : snprintf(data, sizeof(data),
            "{\"name\":\"X11-root\",\"output\":0,\"x\":0,\"y\":0,\"width\":%u,"
            "\"height\":%u,\"primary\":true,\"refreshRate\":0,\"orientation\":0}",
            g->width, g->height);
        ok = ok && size > 0 && (size_t)size < sizeof(data)
            && ksd_buffer_bytes(&out, data, (size_t)size);
        free(g);
    }
    free(monitors); free(resources);
    finish(&out, ok && ksd_buffer_bytes(&out, "]}", 2u), result);
}

struct ksd_x11_keyboard_cache {
    struct xkb_context *context;
    struct xkb_keymap *keymap;
    char *text;
    char revision[33];
    int32_t device;
    uint8_t event_base;
};

void ksd_x11_keyboard_clear(ksd_x11 *connection)
{
    struct ksd_x11_keyboard_cache *cache = connection->keyboard;
    if (cache == NULL) return;
    free(cache->text);
    xkb_keymap_unref(cache->keymap);
    xkb_context_unref(cache->context);
    free(cache);
    connection->keyboard = NULL;
}

static bool keyboard_keymap(ksd_x11 *connection)
{
    struct ksd_x11_keyboard_cache *cache = connection->keyboard;
    xcb_generic_event_t *event;
    bool changed = false;
    while ((event = xcb_poll_for_event(connection->connection)) != NULL) {
        if (cache != NULL && (event->response_type & 0x7fu) == cache->event_base) changed = true;
        free(event);
    }
    if (cache != NULL && !changed) return true;
    ksd_x11_keyboard_clear(connection);
    cache = calloc(1u, sizeof(*cache));
    if (cache == NULL) return false;
    connection->keyboard = cache;
    cache->context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (cache->context == NULL || !xkb_x11_setup_xkb_extension(connection->connection,
        XKB_X11_MIN_MAJOR_XKB_VERSION, XKB_X11_MIN_MINOR_XKB_VERSION,
        XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS, NULL, NULL, &cache->event_base, NULL)) goto failed;
    cache->device = xkb_x11_get_core_keyboard_device_id(connection->connection);
    if (cache->device < 0) goto failed;
    uint16_t selected = XCB_XKB_EVENT_TYPE_NEW_KEYBOARD_NOTIFY | XCB_XKB_EVENT_TYPE_MAP_NOTIFY
        | XCB_XKB_EVENT_TYPE_NAMES_NOTIFY | XCB_XKB_EVENT_TYPE_INDICATOR_MAP_NOTIFY;
    xcb_generic_error_t *error = xcb_request_check(connection->connection,
        xcb_xkb_select_events_aux_checked(connection->connection, (uint16_t)cache->device,
            selected, 0u, selected, 0xffu, 0xffu, NULL));
    if (error != NULL) { free(error); goto failed; }
    cache->keymap = xkb_x11_keymap_new_from_device(cache->context, connection->connection,
        cache->device, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (cache->keymap == NULL) goto failed;
    cache->text = xkb_keymap_get_as_string(cache->keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    if (cache->text == NULL) goto failed;
    unsigned char token[16];
    size_t offset = 0u;
    while (offset < sizeof(token)) {
        ssize_t amount = getrandom(token + offset, sizeof(token) - offset, 0);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) goto failed;
        offset += (size_t)amount;
    }
    for (size_t i = 0u; i < sizeof(token); i++)
        (void)snprintf(cache->revision + i * 2u, 3u, "%02x", token[i]);
    return true;
failed:
    ksd_x11_keyboard_clear(connection);
    return false;
}

static uint32_t modifier_mask(struct xkb_keymap *keymap, const xcb_query_keymap_reply_t *keys)
{
    uint32_t result = 0u;
    if (keys == NULL) return result;
    for (xkb_keycode_t code = 8u; code < 256u; code++) {
        if ((keys->keys[code / 8u] & (1u << (code % 8u))) == 0u) continue;
        const xkb_keysym_t *symbols;
        int count = xkb_keymap_key_get_syms_by_level(keymap, code, 0u, 0u, &symbols);
        for (int i = 0; i < count; i++) switch (symbols[i]) {
            case XKB_KEY_Control_L: result |= 0x01u; break;
            case XKB_KEY_Control_R: result |= 0x02u; break;
            case XKB_KEY_Alt_L: result |= 0x04u; break;
            case XKB_KEY_Alt_R: case XKB_KEY_ISO_Level3_Shift: result |= 0x08u; break;
            case XKB_KEY_Shift_L: result |= 0x10u; break;
            case XKB_KEY_Shift_R: result |= 0x20u; break;
            case XKB_KEY_Super_L: result |= 0x40u; break;
            case XKB_KEY_Super_R: result |= 0x80u; break;
            default: break;
        }
    }
    return result;
}

void ksd_x11_keyboard_state_since(ksd_x11 *connection, const uint8_t *revision,
                                   uint32_t revision_length, ksd_operation_result *result)
{
    if (!keyboard_keymap(connection)) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u, "XKB keymap is unavailable");
        return;
    }
    struct ksd_x11_keyboard_cache *cache = connection->keyboard;
    xcb_connection_t *c = connection->connection;
    xcb_query_keymap_cookie_t keys_cookie = xcb_query_keymap(c);
    xcb_get_pointer_mapping_cookie_t mapping_cookie = xcb_get_pointer_mapping(c);
    struct xkb_state *state = xkb_x11_state_new_from_device(cache->keymap, c, cache->device);
    xcb_query_keymap_reply_t *keys = xcb_query_keymap_reply(c, keys_cookie, NULL);
    xcb_get_pointer_mapping_reply_t *mapping = xcb_get_pointer_mapping_reply(c, mapping_cookie, NULL);
    ksd_buffer out;
    ksd_buffer_init(&out, KSD_MAX_TEXT_BYTES);
    bool ok = state != NULL && keys != NULL && mapping != NULL;
    if (ok) {
        char data[320];
        int size = snprintf(data, sizeof(data),
            "{\"ok\":true,\"group\":%u,\"depressed\":%u,\"latched\":%u,\"locked\":%u,"
            "\"modifiers\":%u,\"capsLock\":%s,\"numLock\":%s,\"scrollLock\":%s,\"mapRevision\":\"%s\"",
            xkb_state_serialize_layout(state, XKB_STATE_LAYOUT_EFFECTIVE),
            xkb_state_serialize_mods(state, XKB_STATE_MODS_DEPRESSED),
            xkb_state_serialize_mods(state, XKB_STATE_MODS_LATCHED),
            xkb_state_serialize_mods(state, XKB_STATE_MODS_LOCKED),
            modifier_mask(cache->keymap, keys),
            xkb_state_led_name_is_active(state, XKB_LED_NAME_CAPS) > 0 ? "true" : "false",
            xkb_state_led_name_is_active(state, XKB_LED_NAME_NUM) > 0 ? "true" : "false",
            xkb_state_led_name_is_active(state, XKB_LED_NAME_SCROLL) > 0 ? "true" : "false", cache->revision);
        ok = size > 0 && (size_t)size < sizeof(data)
            && ksd_buffer_bytes(&out, data, (size_t)size);
        if (revision_length != 32u || revision == NULL || memcmp(revision, cache->revision, 32u) != 0)
            ok = ok && ksd_buffer_bytes(&out, ",\"keymap\":", 10u)
                && ksd_buffer_json_string(&out, cache->text,
                                          strlen(cache->text), false);
        ok = ok && ksd_buffer_bytes(&out, ",\"layouts\":[", 12u);
        for (xkb_layout_index_t i = 0u; ok && i < xkb_keymap_num_layouts(cache->keymap); i++) {
            const char *name = xkb_keymap_layout_get_name(cache->keymap, i);
            if (i != 0u) ok = ksd_buffer_bytes(&out, ",", 1u);
            ok = ok && ksd_buffer_json_string(&out, name != NULL ? name : "",
                name != NULL ? strlen(name) : 0u, false);
        }
        ok = ok && ksd_buffer_bytes(&out, "],\"pointerMapping\":[", 20u);
        const uint8_t *buttons = xcb_get_pointer_mapping_map(mapping);
        for (int i = 0; ok && i < xcb_get_pointer_mapping_map_length(mapping); i++) {
            int length = snprintf(data, sizeof(data), i == 0 ? "%u" : ",%u", buttons[i]);
            ok = length > 0 && ksd_buffer_bytes(&out, data, (size_t)length);
        }
        ok = ok && ksd_buffer_bytes(&out, "]}", 2u);
    }
    xkb_state_unref(state);
    free(keys); free(mapping);
    if (!ok) {
        ksd_buffer_clear(&out);
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u, "XKB keyboard state is unavailable");
    } else {
        (void)ksd_result_take_framed_text(&out, result, KSD_STATUS_INTERNAL,
                                          "out of memory");
    }
}
