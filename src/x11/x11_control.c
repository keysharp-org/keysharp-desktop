#include "x11_control.h"

#include "protocol.h"
#include "x11_internal.h"

#include <stdlib.h>
#include <string.h>

/* _NET_WM_STATE actions, from the EWMH specification. */
#define KSD_NET_WM_STATE_REMOVE 0u
#define KSD_NET_WM_STATE_ADD 1u

/* The source indication every EWMH message carries. Two means a pager or
 * other direct agent rather than the application itself, which is what this
 * service is: a window manager is entitled to treat the two differently, and
 * claiming to be the application would be a lie it might act on. */
#define KSD_EWMH_SOURCE_PAGER 2u

/* WM_CHANGE_STATE carries an ICCCM window state, not an EWMH one. */
#define KSD_ICCCM_ICONIC_STATE 3u

/* _MOTIF_WM_HINTS is not a standard at all: it is the hint Motif defined,
 * which every window manager then implemented because it is the only way to
 * ask for a window without a frame. Five 32-bit values, of which this sets
 * two: a flags word saying "the decorations field is meaningful", and the
 * decorations field itself. */
#define KSD_MOTIF_HINTS_WORDS 5u
#define KSD_MOTIF_HINTS_FLAG_DECORATIONS 2u

typedef struct control_atoms {
    xcb_atom_t active_window;
    xcb_atom_t close_window;
    xcb_atom_t moveresize_window;
    xcb_atom_t wm_state;
    xcb_atom_t state_above;
    xcb_atom_t state_max_vert;
    xcb_atom_t state_max_horz;
    xcb_atom_t change_state;
    xcb_atom_t opacity;
    xcb_atom_t motif_hints;
} control_atoms;

static xcb_atom_t intern(xcb_connection_t *connection, const char *name)
{
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(connection,
        xcb_intern_atom(connection, 0, (uint16_t)strlen(name), name), NULL);
    xcb_atom_t atom = XCB_ATOM_NONE;

    if (reply != NULL) {
        atom = reply->atom;
        free(reply);
    }
    return atom;
}

/* Interned in one round trip rather than ten. Every request goes out before
 * any reply is collected, which is the whole reason this backend uses xcb. */
static bool load_atoms(xcb_connection_t *c, control_atoms *atoms)
{
    static const char *const names[] = {
        "_NET_ACTIVE_WINDOW", "_NET_CLOSE_WINDOW", "_NET_MOVERESIZE_WINDOW",
        "_NET_WM_STATE", "_NET_WM_STATE_ABOVE", "_NET_WM_STATE_MAXIMIZED_VERT",
        "_NET_WM_STATE_MAXIMIZED_HORZ", "WM_CHANGE_STATE",
        "_NET_WM_WINDOW_OPACITY", "_MOTIF_WM_HINTS",
    };
    xcb_intern_atom_cookie_t cookies[10];
    xcb_atom_t *fields = &atoms->active_window;

    for (size_t index = 0u; index < 10u; index++)
        cookies[index] = xcb_intern_atom(c, 0,
            (uint16_t)strlen(names[index]), names[index]);
    for (size_t index = 0u; index < 10u; index++) {
        xcb_intern_atom_reply_t *reply =
            xcb_intern_atom_reply(c, cookies[index], NULL);

        if (reply == NULL)
            return false;
        fields[index] = reply->atom;
        free(reply);
    }
    (void)intern;
    return true;
}

/* Whether the window still exists. Every verb checks first, so a window that
 * went away is NOT_FOUND rather than a request sent into nothing and reported
 * as success. The check is a race by nature -- the window can close between
 * this and the message -- but the alternative is reporting success for a
 * window that was already gone when the call was made. */
static bool window_exists(xcb_connection_t *c, xcb_window_t window)
{
    xcb_get_window_attributes_reply_t *attributes;

    if (window == XCB_WINDOW_NONE)
        return false;
    attributes = xcb_get_window_attributes_reply(c,
        xcb_get_window_attributes(c, window), NULL);
    if (attributes == NULL)
        return false;
    free(attributes);
    return true;
}

static void applied(ksd_operation_result *result)
{
    (void)ksd_result_take(result, NULL, 0u);
}

static void missing(ksd_operation_result *result)
{
    ksd_result_error(result, KSD_STATUS_NOT_FOUND, 0u,
                     "that window is not on this display");
}

/* An EWMH request is a client message to the ROOT window, not to the window it
 * names. The window manager selects for substructure events on the root, and
 * that is where it is listening; sending to the window itself would reach the
 * application, which is not who is being asked. */
static void send_to_root(xcb_connection_t *c, const xcb_screen_t *screen,
                         xcb_window_t window, xcb_atom_t type,
                         const uint32_t data[5])
{
    xcb_client_message_event_t event;

    memset(&event, 0, sizeof(event));
    event.response_type = XCB_CLIENT_MESSAGE;
    event.format = 32;
    event.window = window;
    event.type = type;
    for (size_t index = 0u; index < 5u; index++)
        event.data.data32[index] = data[index];
    xcb_send_event(c, 0, screen->root,
                   XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY
                       | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
                   (const char *)&event);
    xcb_flush(c);
}

/* Adds or removes one _NET_WM_STATE atom. Two may be given, because maximized
 * is two atoms -- vertical and horizontal -- and a window maximized in only
 * one direction is a different state, not a half-done one. */
static void change_state(xcb_connection_t *c, const xcb_screen_t *screen,
                         const control_atoms *atoms, xcb_window_t window,
                         uint32_t action, xcb_atom_t first, xcb_atom_t second)
{
    uint32_t data[5] = {
        action, (uint32_t)first, (uint32_t)second, KSD_EWMH_SOURCE_PAGER, 0u,
    };

    send_to_root(c, screen, window, atoms->wm_state, data);
}

#define CONTROL_PROLOGUE(connection, window, atoms, result)                   \
    xcb_connection_t *c = (connection)->connection;                           \
    if (!load_atoms(c, &(atoms))) {                                           \
        ksd_result_error((result), KSD_STATUS_INTERNAL, 0u,                   \
                         "could not name the window protocol on this "        \
                         "display");                                          \
        return;                                                               \
    }                                                                         \
    if (!window_exists(c, (window))) {                                        \
        missing(result);                                                      \
        return;                                                               \
    }

void ksd_x11_window_focus(ksd_x11 *connection, uint32_t window,
                          ksd_operation_result *result)
{
    control_atoms atoms;
    CONTROL_PROLOGUE(connection, window, atoms, result)
    /* Timestamp zero rather than a made-up one. A window manager compares it
     * against the last user interaction to decide whether to focus or merely
     * flag the window, and inventing a current time would defeat that
     * deliberately -- this service has no user interaction to report. */
    uint32_t data[5] = { KSD_EWMH_SOURCE_PAGER, 0u, 0u, 0u, 0u };
    send_to_root(c, connection->screen, window, atoms.active_window, data);
    applied(result);
}

void ksd_x11_window_raise(ksd_x11 *connection, uint32_t window, bool raise,
                          ksd_operation_result *result)
{
    control_atoms atoms;
    CONTROL_PROLOGUE(connection, window, atoms, result)
    /* Restacking is one of the few verbs a client may do directly, and doing
     * it directly is what makes raise and lower work with no window manager
     * running. A reparenting manager will have made the frame the child of the
     * root, so the request is issued against the frame when there is one. */
    xcb_query_tree_reply_t *tree = xcb_query_tree_reply(c,
        xcb_query_tree(c, window), NULL);
    xcb_window_t target = window;
    uint32_t values[] = {
        raise ? (uint32_t)XCB_STACK_MODE_ABOVE : (uint32_t)XCB_STACK_MODE_BELOW,
    };

    if (tree != NULL) {
        if (tree->parent != XCB_WINDOW_NONE
            && tree->parent != connection->screen->root)
            target = tree->parent;
        free(tree);
    }
    xcb_configure_window(c, target, XCB_CONFIG_WINDOW_STACK_MODE, values);
    xcb_flush(c);
    applied(result);
}

void ksd_x11_window_close(ksd_x11 *connection, uint32_t window,
                          ksd_operation_result *result)
{
    control_atoms atoms;
    CONTROL_PROLOGUE(connection, window, atoms, result)
    /* A close REQUEST. The application is entitled to refuse it, or to put up
     * a dialog, and this service reports that the ask was delivered rather
     * than that the window went away. */
    uint32_t data[5] = { 0u, KSD_EWMH_SOURCE_PAGER, 0u, 0u, 0u };
    send_to_root(c, connection->screen, window, atoms.close_window, data);
    applied(result);
}

void ksd_x11_window_kill(ksd_x11 *connection, uint32_t window,
                         ksd_operation_result *result)
{
    control_atoms atoms;
    CONTROL_PROLOGUE(connection, window, atoms, result)
    /* Not a request. This severs the client's connection to the server, and
     * every other window that client owns dies with it. That is what killing
     * means here, and it is why it is a separate verb from close rather than
     * a fallback for one. */
    xcb_kill_client(c, window);
    xcb_flush(c);
    applied(result);
}

void ksd_x11_window_move_resize(ksd_x11 *connection, uint32_t window,
                                int32_t x, int32_t y, uint32_t width,
                                uint32_t height,
                                ksd_operation_result *result)
{
    control_atoms atoms;
    CONTROL_PROLOGUE(connection, window, atoms, result)
    uint32_t extents[4] = { 0u };
    xcb_get_property_reply_t *frame = ksd_x11_property(c, window,
        intern(c, "_NET_FRAME_EXTENTS"), XCB_ATOM_CARDINAL, 4u);
    if (frame != NULL && frame->format == 32
        && xcb_get_property_value_length(frame) >= (int)sizeof(extents))
        memcpy(extents, xcb_get_property_value(frame), sizeof(extents));
    free(frame);
    for (size_t i = 0u; i < 4u; i++)
        if (extents[i] > 32768u) extents[i] = 0u;
    uint32_t horizontal = extents[0] + extents[1];
    uint32_t vertical = extents[2] + extents[3];
    if (width <= horizontal || height <= vertical) {
        ksd_result_error(result, KSD_STATUS_INVALID_REQUEST, 0u,
                         "window bounds must leave a positive client area");
        return;
    }
    /* The public rectangle includes decorations; X11 resize requests size the client. */
    width -= horizontal;
    height -= vertical;
    /* The flags word says which fields are meaningful and who is asking. The
     * low byte is the gravity, left at zero to mean the window's own; bits 8
     * to 11 mark x, y, width and height as supplied; bits 12 and 13 carry the
     * source indication. All four are always supplied here, because the verb
     * takes all four. */
    uint32_t flags = (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11)
        | (KSD_EWMH_SOURCE_PAGER << 12);
    uint32_t data[5] = {
        flags, (uint32_t)x, (uint32_t)y, width, height,
    };

    send_to_root(c, connection->screen, window, atoms.moveresize_window, data);
    /* Also configured directly, so the verb does something on a bare display
     * where no window manager will act on the message above. A managed window
     * has its configure request redirected to the manager, which is exactly
     * the behaviour wanted: the manager decides, and this is ignored. */
    uint32_t values[] = { (uint32_t)x, (uint32_t)y, width, height };
    xcb_configure_window(c, window,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y
                             | XCB_CONFIG_WINDOW_WIDTH
                             | XCB_CONFIG_WINDOW_HEIGHT, values);
    xcb_flush(c);
    applied(result);
}

void ksd_x11_window_set_state(ksd_x11 *connection, uint32_t window,
                              uint32_t state, ksd_operation_result *result)
{
    control_atoms atoms;

    if (state > 2u) {
        ksd_result_error(result, KSD_STATUS_INVALID_REQUEST, 0u,
                         "that is not a window state this service sets");
        return;
    }
    {
        CONTROL_PROLOGUE(connection, window, atoms, result)
        if (state == 1u) {
            /* Minimize is ICCCM, not EWMH: WM_CHANGE_STATE to IconicState.
             * There is no _NET_WM_STATE atom for it -- _NET_WM_STATE_HIDDEN
             * describes a window that IS minimized and is set by the manager,
             * not a request to minimize one. */
            uint32_t data[5] = { KSD_ICCCM_ICONIC_STATE, 0u, 0u, 0u, 0u };
            send_to_root(c, connection->screen, window, atoms.change_state,
                         data);
        } else if (state == 2u) {
            change_state(c, connection->screen, &atoms, window,
                         KSD_NET_WM_STATE_ADD, atoms.state_max_vert,
                         atoms.state_max_horz);
        } else {
            /* Restoring is both: dropping the maximized atoms, and mapping the
             * window, which is what un-minimizes it. A window that was neither
             * is unharmed by either. */
            change_state(c, connection->screen, &atoms, window,
                         KSD_NET_WM_STATE_REMOVE, atoms.state_max_vert,
                         atoms.state_max_horz);
            xcb_map_window(c, window);
            xcb_flush(c);
        }
        applied(result);
    }
}

void ksd_x11_window_set_opacity(ksd_x11 *connection, uint32_t window,
                                uint32_t opacity,
                                ksd_operation_result *result)
{
    control_atoms atoms;

    if (opacity > 255u) {
        ksd_result_error(result, KSD_STATUS_INVALID_REQUEST, 0u,
                         "opacity runs from 0 to 255");
        return;
    }
    {
        CONTROL_PROLOGUE(connection, window, atoms, result)
        /* The property is a 32-bit value where 0xffffffff is opaque, while
         * every backend of this service reports and takes 0 to 255. The byte
         * is replicated across all four rather than shifted into the top one,
         * so that 255 becomes exactly 0xffffffff and not 0xff000000 -- the
         * latter is a hair under fully opaque, and a compositor will composite
         * a window it should leave alone. Reading back the top byte, which is
         * what the window list does, returns the byte that was written. */
        uint32_t value = opacity * 0x01010101u;

        xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, atoms.opacity,
                            XCB_ATOM_CARDINAL, 32, 1u, &value);
        xcb_flush(c);
        applied(result);
    }
}

void ksd_x11_window_set_above(ksd_x11 *connection, uint32_t window,
                              bool above, ksd_operation_result *result)
{
    control_atoms atoms;
    CONTROL_PROLOGUE(connection, window, atoms, result)
    change_state(c, connection->screen, &atoms, window,
                 above ? KSD_NET_WM_STATE_ADD : KSD_NET_WM_STATE_REMOVE,
                 atoms.state_above, XCB_ATOM_NONE);
    applied(result);
}

void ksd_x11_window_set_decorated(ksd_x11 *connection, uint32_t window,
                                  bool decorated,
                                  ksd_operation_result *result)
{
    control_atoms atoms;
    CONTROL_PROLOGUE(connection, window, atoms, result)
    /* flags, functions, decorations, input_mode, status. Only the first and
     * third are set: the flags word marks the decorations field meaningful,
     * and the decorations field is all-or-nothing here. */
    uint32_t hints[KSD_MOTIF_HINTS_WORDS] = {
        KSD_MOTIF_HINTS_FLAG_DECORATIONS, 0u, decorated ? 1u : 0u, 0u, 0u,
    };

    xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, atoms.motif_hints,
                        atoms.motif_hints, 32, KSD_MOTIF_HINTS_WORDS, hints);
    xcb_flush(c);
    applied(result);
}
