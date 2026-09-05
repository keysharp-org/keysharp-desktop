#include "x11_internal.h"

#include "protocol.h"

#include <stdlib.h>
#include <string.h>

/* Every name is asked for in one pass and every answer collected in a second,
 * once for the session-lifetime connection. */
void ksd_x11_load_atoms(xcb_connection_t *c, x11_atoms *atoms)
{
    static const char *const names[] = {
        "_NET_CLIENT_LIST", "_NET_ACTIVE_WINDOW", "_NET_CLOSE_WINDOW",
        "_NET_MOVERESIZE_WINDOW", "_NET_WORKAREA",
        "_NET_CURRENT_DESKTOP", "_NET_WM_DESKTOP", "_NET_WM_NAME",
        "_NET_WM_PID", "WM_STATE", "_NET_WM_STATE", "_NET_WM_STATE_HIDDEN",
        "_NET_WM_STATE_ABOVE", "_NET_WM_STATE_MAXIMIZED_VERT",
        "_NET_WM_STATE_MAXIMIZED_HORZ", "WM_CHANGE_STATE",
        "_NET_FRAME_EXTENTS", "_NET_WM_WINDOW_OPACITY", "_MOTIF_WM_HINTS",
        "UTF8_STRING", "_NET_CLIENT_LIST_STACKING",
    };
    size_t count = sizeof(names) / sizeof(names[0]);
    xcb_intern_atom_cookie_t cookies[sizeof(names) / sizeof(names[0])];
    xcb_atom_t *fields[sizeof(names) / sizeof(names[0])];

    fields[0] = &atoms->client_list;
    fields[1] = &atoms->active_window;
    fields[2] = &atoms->close_window;
    fields[3] = &atoms->moveresize_window;
    fields[4] = &atoms->work_area;
    fields[5] = &atoms->current_desktop;
    fields[6] = &atoms->wm_desktop;
    fields[7] = &atoms->wm_name;
    fields[8] = &atoms->wm_pid;
    fields[9] = &atoms->icccm_wm_state;
    fields[10] = &atoms->wm_state;
    fields[11] = &atoms->state_hidden;
    fields[12] = &atoms->state_above;
    fields[13] = &atoms->state_max_vert;
    fields[14] = &atoms->state_max_horz;
    fields[15] = &atoms->change_state;
    fields[16] = &atoms->frame_extents;
    fields[17] = &atoms->opacity;
    fields[18] = &atoms->motif_hints;
    fields[19] = &atoms->utf8_string;
    fields[20] = &atoms->client_list_stacking;

    for (size_t index = 0u; index < count; index++)
        cookies[index] = xcb_intern_atom(c, 0,
            (uint16_t)strlen(names[index]), names[index]);
    for (size_t index = 0u; index < count; index++) {
        xcb_intern_atom_reply_t *reply =
            xcb_intern_atom_reply(c, cookies[index], NULL);

        *fields[index] = XCB_ATOM_NONE;
        if (reply != NULL) {
            *fields[index] = reply->atom;
            free(reply);
        }
    }
}

xcb_get_property_reply_t *ksd_x11_property(xcb_connection_t *c,
                                           xcb_window_t window,
                                           xcb_atom_t name, xcb_atom_t type,
                                           uint32_t words)
{
    xcb_get_property_reply_t *reply;

    if (name == XCB_ATOM_NONE)
        return NULL;
    reply = xcb_get_property_reply(c,
        xcb_get_property(c, 0, window, name, type, 0u, words), NULL);
    /* A property that is not set answers with a reply whose type is None, not
     * with no reply. Reporting that as an empty value would turn "this server
     * has no window manager" into "this session has no windows". */
    if (reply != NULL && reply->type == XCB_ATOM_NONE) {
        free(reply);
        return NULL;
    }
    return reply;
}

bool ksd_x11_cardinal(xcb_connection_t *c, xcb_window_t window,
                      xcb_atom_t name, uint32_t *value)
{
    xcb_get_property_reply_t *reply = ksd_x11_property(c, window, name,
                                                       XCB_ATOM_ANY, 1u);
    bool ok = false;

    if (reply != NULL) {
        if (xcb_get_property_value_length(reply) >= 4) {
            memcpy(value, xcb_get_property_value(reply), sizeof(*value));
            ok = true;
        }
        free(reply);
    }
    return ok;
}

bool ksd_x11_has_state(xcb_connection_t *c, const x11_atoms *atoms,
                       xcb_window_t window, xcb_atom_t wanted)
{
    xcb_get_property_reply_t *reply;
    bool found = false;

    if (wanted == XCB_ATOM_NONE)
        return false;
    reply = ksd_x11_property(c, window, atoms->wm_state, XCB_ATOM_ATOM, 64u);
    if (reply == NULL)
        return false;
    int length = xcb_get_property_value_length(reply);
    const xcb_atom_t *states = xcb_get_property_value(reply);
    size_t count = length > 0 ? (size_t)length / sizeof(xcb_atom_t) : 0u;
    for (size_t index = 0u; index < count && !found; index++)
        found = states[index] == wanted;
    free(reply);
    return found;
}

static void numeric_result(ksd_operation_result *result,
                           const uint32_t *values, size_t count)
{
    uint8_t encoded[16];

    if (count > 4u) {
        ksd_result_error(result, KSD_STATUS_INTERNAL, 0u, "bad reply");
        return;
    }
    for (size_t index = 0u; index < count; index++)
        ksd_encode_u32(encoded + index * 4u, values[index]);
    if (!ksd_result_copy(result, encoded, (uint32_t)(count * 4u)))
        ksd_result_error(result, KSD_STATUS_INTERNAL, 0u, "out of memory");
}

void ksd_x11_cursor_position(ksd_x11 *connection, ksd_operation_result *result)
{
    xcb_query_pointer_reply_t *reply = xcb_query_pointer_reply(
        connection->connection,
        xcb_query_pointer(connection->connection, connection->screen->root),
        NULL);
    uint32_t values[2];

    if (reply == NULL) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                         "the X server did not report the pointer");
        return;
    }
    /* Widened from int16 and reinterpreted as the wire u32, which is exactly
     * what the compositor providers put on the wire for this opcode. */
    values[0] = (uint32_t)(int32_t)reply->root_x;
    values[1] = (uint32_t)(int32_t)reply->root_y;
    free(reply);
    numeric_result(result, values, 2u);
}

void ksd_x11_work_area(ksd_x11 *connection, ksd_operation_result *result)
{
    const x11_atoms *atoms = &connection->atoms;
    uint32_t desktop = 0u;
    uint32_t values[4];

    (void)ksd_x11_cardinal(connection->connection, connection->screen->root,
                           atoms->current_desktop, &desktop);
    if (desktop > 1024u) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                         "the X server reported an implausible desktop");
        return;
    }

    /* _NET_WORKAREA is four cardinals per desktop. A window manager that does
     * not publish it leaves the screen itself as the answer, which is true on
     * a bare session with no panels rather than a failure. */
    xcb_get_property_reply_t *reply = ksd_x11_property(connection->connection,
        connection->screen->root, atoms->work_area, XCB_ATOM_CARDINAL,
        (desktop + 1u) * 4u);
    int length = reply != NULL ? xcb_get_property_value_length(reply) : 0;
    size_t needed = ((size_t)desktop + 1u) * 4u * sizeof(uint32_t);

    if (reply != NULL && length > 0 && (size_t)length >= needed) {
        const uint32_t *area = xcb_get_property_value(reply);
        memcpy(values, area + (size_t)desktop * 4u, sizeof(values));
    } else {
        values[0] = 0u;
        values[1] = 0u;
        values[2] = connection->screen->width_in_pixels;
        values[3] = connection->screen->height_in_pixels;
    }
    free(reply);
    if (values[2] == 0u || values[3] == 0u) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                         "the X server reported an empty work area");
        return;
    }
    numeric_result(result, values, 4u);
}

xcb_get_property_cookie_t ksd_x11_property_cookie(xcb_connection_t *c,
                                                  xcb_window_t window,
                                                  xcb_atom_t name,
                                                  xcb_atom_t type,
                                                  uint32_t words)
{
    /* An atom the server does not know cannot name a property, but a request
     * still has to be issued so the collect pass stays in step with the issue
     * pass. XCB_ATOM_NONE as the property yields a None-typed reply, which
     * ksd_x11_take_property reports as absent. */
    return xcb_get_property(c, 0, window, name, type, 0u, words);
}

xcb_get_property_reply_t *ksd_x11_take_property(xcb_connection_t *c,
                                                xcb_get_property_cookie_t cookie)
{
    xcb_get_property_reply_t *reply = xcb_get_property_reply(c, cookie, NULL);

    if (reply != NULL && reply->type == XCB_ATOM_NONE) {
        free(reply);
        return NULL;
    }
    return reply;
}

bool ksd_x11_state_has(const xcb_get_property_reply_t *state,
                       xcb_atom_t wanted)
{
    if (state == NULL || wanted == XCB_ATOM_NONE)
        return false;

    int length = xcb_get_property_value_length(state);
    const xcb_atom_t *atoms = xcb_get_property_value(state);
    size_t count = length > 0 ? (size_t)length / sizeof(xcb_atom_t) : 0u;

    for (size_t index = 0u; index < count; index++)
        if (atoms[index] == wanted)
            return true;
    return false;
}

bool ksd_x11_append_text_reply(ksd_buffer *out, xcb_get_property_reply_t *reply)
{
    bool ok;

    if (reply == NULL)
        return ksd_buffer_json_string(out, "", 0u, false);

    int raw = xcb_get_property_value_length(reply);
    const char *value = xcb_get_property_value(reply);
    size_t length = raw > 0 ? (size_t)raw : 0u;

    if (length > KSD_X11_MAX_TEXT)
        length = KSD_X11_MAX_TEXT;
    for (size_t index = 0u; index < length; index++)
        if (value[index] == 0) {
            length = index;
            break;
        }
    bool latin1 = reply->type == XCB_ATOM_STRING && reply->format == 8;
    if (!latin1) {
        /* Only a truncated final UTF-8 codepoint can be repaired by shortening. */
        size_t shortened = 0u;
        while (length != 0u && shortened < 3u
            && !ksd_utf8_valid((const uint8_t *)value, length, false)) {
            length--;
            shortened++;
        }
        if (!ksd_utf8_valid((const uint8_t *)value, length, false)) length = 0u;
    }
    ok = ksd_buffer_json_string(out, value, length, latin1);
    free(reply);
    return ok;
}
