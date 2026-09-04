#include "x11_worker.h"

#include "protocol.h"
#include "session_environ.h"
#include "protocol_io.h"
#include "x11_capture.h"
#include "x11_clipboard.h"
#include "x11_control.h"
#include "x11_connect.h"
#include "x11_display.h"
#include "x11_query.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define KSD_X11_AUTHORITY_CAPACITY 4096u
#define KSD_X11_HANDLE_DIGITS 10u

/* A window handle on this backend is an XID, which is 32 bits. The provider
 * backends parse the same field as a 64-bit value because a compositor id is
 * one; accepting that range here and truncating it would let two different
 * handles name one window. Rejecting the excess is the point of a separate
 * parser rather than a shared one. */
static bool parse_xid(const uint8_t *bytes, uint32_t length, uint32_t *xid)
{
    uint64_t value = 0u;

    if (length == 0u || length > KSD_X11_HANDLE_DIGITS)
        return false;
    if (length > 1u && bytes[0] == '0')
        return false;
    for (uint32_t index = 0u; index < length; index++) {
        if (bytes[index] < '0' || bytes[index] > '9')
            return false;
        value = value * 10u + (uint64_t)(bytes[index] - '0');
    }
    /* Zero is XCB_WINDOW_NONE, which names no window. */
    if (value == 0u || value > UINT32_MAX)
        return false;
    *xid = (uint32_t)value;
    return true;
}

/* A window handle on the wire is 64 bits because a compositor id is, but on
 * this backend it names an XID, which is 32. A value that does not fit is not
 * a window on this display, and truncating it would aim the verb at whatever
 * window happens to wear the low half. */
static bool handle_is_xid(uint64_t handle)
{
    return handle != 0u && handle <= UINT32_MAX;
}

bool ksd_x11_request_valid(const ksd_frame *request)
{
    ksd_cursor cursor;
    uint32_t include_hidden;
    uint32_t reserved;

    if (request == NULL)
        return false;
    ksd_cursor_init(&cursor, request->payload, request->payload_length);
    switch (request->opcode) {
        case KSD_OP_WINDOW_LIST:
            return ksd_cursor_u32(&cursor, &include_hidden)
                && ksd_cursor_u32(&cursor, &reserved)
                && include_hidden <= 1u && reserved == 0u
                && ksd_cursor_finished(&cursor);
        case KSD_OP_WINDOW_ACTIVE:
        case KSD_OP_WINDOW_HANDLES:
        case KSD_OP_CURSOR_POSITION:
        case KSD_OP_WORK_AREA:
            return request->payload_length == 0u;
        case KSD_OP_CAPTURE_AREA: {
            int32_t x;
            int32_t y;
            uint32_t width;
            uint32_t height;
            return ksd_cursor_i32(&cursor, &x) && ksd_cursor_i32(&cursor, &y)
                && ksd_cursor_u32(&cursor, &width)
                && ksd_cursor_u32(&cursor, &height)
                && ksd_cursor_finished(&cursor);
        }
        case KSD_OP_CAPTURE_WINDOW: {
            uint32_t flags;
            uint32_t length;
            const uint8_t *bytes;
            uint32_t xid;
            return ksd_cursor_u32(&cursor, &flags)
                && ksd_cursor_u32(&cursor, &length)
                && (flags & ~KSD_CAPTURE_WINDOW_INCLUDE_DECORATION) == 0u
                && ksd_cursor_bytes(&cursor, length, &bytes)
                && ksd_cursor_finished(&cursor)
                && parse_xid(bytes, length, &xid);
        }
        case KSD_OP_CLIPBOARD_MIMETYPES:
        case KSD_OP_CLIPBOARD_TEXT:
            return request->payload_length == 0u;
        case KSD_OP_WINDOW_FOCUS:
        case KSD_OP_WINDOW_RAISE:
        case KSD_OP_WINDOW_LOWER:
        case KSD_OP_WINDOW_CLOSE:
        case KSD_OP_WINDOW_KILL: {
            uint64_t handle;
            return ksd_cursor_u64(&cursor, &handle)
                && ksd_cursor_finished(&cursor)
                && handle_is_xid(handle);
        }
        case KSD_OP_WINDOW_MOVE_RESIZE: {
            uint64_t handle;
            int32_t x;
            int32_t y;
            uint32_t width;
            uint32_t height;
            return ksd_cursor_u64(&cursor, &handle)
                && ksd_cursor_i32(&cursor, &x) && ksd_cursor_i32(&cursor, &y)
                && ksd_cursor_u32(&cursor, &width)
                && ksd_cursor_u32(&cursor, &height)
                && ksd_cursor_finished(&cursor)
                && handle_is_xid(handle)
                && width != 0u && height != 0u
                && width <= (uint32_t)INT16_MAX
                && height <= (uint32_t)INT16_MAX
                && x >= INT16_MIN && x <= INT16_MAX
                && y >= INT16_MIN && y <= INT16_MAX;
        }
        case KSD_OP_WINDOW_SET_STATE:
        case KSD_OP_WINDOW_SET_OPACITY:
        case KSD_OP_WINDOW_SET_ABOVE:
        case KSD_OP_WINDOW_SET_DECORATED: {
            uint64_t handle;
            uint32_t value;
            uint32_t tail_reserved;
            uint32_t ceiling = request->opcode == KSD_OP_WINDOW_SET_STATE
                ? 2u : (request->opcode == KSD_OP_WINDOW_SET_OPACITY
                        ? 255u : 1u);
            return ksd_cursor_u64(&cursor, &handle)
                && ksd_cursor_u32(&cursor, &value)
                && ksd_cursor_u32(&cursor, &tail_reserved)
                && ksd_cursor_finished(&cursor)
                && handle_is_xid(handle) && tail_reserved == 0u
                && value <= ceiling;
        }
        case KSD_OP_CLIPBOARD_CONTENT: {
            uint32_t length;
            const uint8_t *bytes;
            /* Validated the same way the providers validate it, so a request
             * this backend refuses is one they would refuse too. */
            return ksd_cursor_u32(&cursor, &length)
                && length != 0u && length <= KSD_MAX_MIMETYPE_BYTES
                && ksd_cursor_bytes(&cursor, length, &bytes)
                && ksd_cursor_finished(&cursor)
                && ksd_utf8_valid(bytes, length, false);
        }
        default:
            return false;
    }
}

void ksd_x11_execute(const ksd_frame *request, pid_t session_pid,
                     ksd_operation_result *result)
{
    char display[256];
    char canonical[KSD_X11_DISPLAY_CAPACITY];
    char authority[KSD_X11_AUTHORITY_CAPACITY];
    const char *authority_value = NULL;
    ksd_x11 *connection = NULL;
    ksd_status status;

    if (!ksd_x11_request_valid(request)) {
        ksd_result_error(result, KSD_STATUS_INVALID_REQUEST, 0u,
                         "invalid X11 request");
        return;
    }
    if (!ksd_session_environ_value(session_pid, "DISPLAY", display, sizeof(display))) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                         "the session names no X display");
        return;
    }
    if (!ksd_x11_display_parse(display, canonical, sizeof(canonical))) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                         "the session names a display this service will not "
                         "open");
        return;
    }
    /* Optional. An absolute path only: a relative one would resolve against
     * whatever directory the worker happens to be in. */
    if (ksd_session_environ_value(session_pid, "XAUTHORITY", authority, sizeof(authority))
        && authority[0] == '/')
        authority_value = authority;

    status = ksd_x11_open(canonical, authority_value, &connection);
    if (status != KSD_STATUS_OK) {
        ksd_result_error(result, status, 0u,
                         "could not open the X display for this session");
        return;
    }

    switch (request->opcode) {
        case KSD_OP_WINDOW_LIST: {
            ksd_cursor cursor;
            uint32_t include_hidden = 0u;
            ksd_cursor_init(&cursor, request->payload,
                            request->payload_length);
            (void)ksd_cursor_u32(&cursor, &include_hidden);
            ksd_x11_window_list(connection, include_hidden != 0u, result);
            break;
        }
        case KSD_OP_WINDOW_ACTIVE:
            ksd_x11_window_active(connection, result);
            break;
        case KSD_OP_WINDOW_HANDLES:
            ksd_x11_window_handles(connection, result);
            break;
        case KSD_OP_CURSOR_POSITION:
            ksd_x11_cursor_position(connection, result);
            break;
        case KSD_OP_WORK_AREA:
            ksd_x11_work_area(connection, result);
            break;
        case KSD_OP_CAPTURE_AREA: {
            ksd_cursor cursor;
            int32_t x = 0;
            int32_t y = 0;
            uint32_t width = 0u;
            uint32_t height = 0u;
            ksd_cursor_init(&cursor, request->payload,
                            request->payload_length);
            (void)ksd_cursor_i32(&cursor, &x);
            (void)ksd_cursor_i32(&cursor, &y);
            (void)ksd_cursor_u32(&cursor, &width);
            (void)ksd_cursor_u32(&cursor, &height);
            ksd_x11_capture_area(connection, x, y, width, height, result);
            break;
        }
        case KSD_OP_CAPTURE_WINDOW: {
            ksd_cursor cursor;
            uint32_t flags = 0u;
            uint32_t length = 0u;
            const uint8_t *bytes = NULL;
            uint32_t xid = 0u;
            ksd_cursor_init(&cursor, request->payload,
                            request->payload_length);
            (void)ksd_cursor_u32(&cursor, &flags);
            (void)ksd_cursor_u32(&cursor, &length);
            (void)ksd_cursor_bytes(&cursor, length, &bytes);
            (void)parse_xid(bytes, length, &xid);
            ksd_x11_capture_window(connection, xid,
                (flags & KSD_CAPTURE_WINDOW_INCLUDE_DECORATION) != 0u, result);
            break;
        }
        case KSD_OP_WINDOW_FOCUS:
        case KSD_OP_WINDOW_RAISE:
        case KSD_OP_WINDOW_LOWER:
        case KSD_OP_WINDOW_CLOSE:
        case KSD_OP_WINDOW_KILL: {
            ksd_cursor cursor;
            uint64_t handle = 0u;
            ksd_cursor_init(&cursor, request->payload,
                            request->payload_length);
            (void)ksd_cursor_u64(&cursor, &handle);
            uint32_t xid = (uint32_t)handle;
            if (request->opcode == KSD_OP_WINDOW_FOCUS)
                ksd_x11_window_focus(connection, xid, result);
            else if (request->opcode == KSD_OP_WINDOW_RAISE)
                ksd_x11_window_raise(connection, xid, true, result);
            else if (request->opcode == KSD_OP_WINDOW_LOWER)
                ksd_x11_window_raise(connection, xid, false, result);
            else if (request->opcode == KSD_OP_WINDOW_CLOSE)
                ksd_x11_window_close(connection, xid, result);
            else
                ksd_x11_window_kill(connection, xid, result);
            break;
        }
        case KSD_OP_WINDOW_MOVE_RESIZE: {
            ksd_cursor cursor;
            uint64_t handle = 0u;
            int32_t x = 0;
            int32_t y = 0;
            uint32_t width = 0u;
            uint32_t height = 0u;
            ksd_cursor_init(&cursor, request->payload,
                            request->payload_length);
            (void)ksd_cursor_u64(&cursor, &handle);
            (void)ksd_cursor_i32(&cursor, &x);
            (void)ksd_cursor_i32(&cursor, &y);
            (void)ksd_cursor_u32(&cursor, &width);
            (void)ksd_cursor_u32(&cursor, &height);
            ksd_x11_window_move_resize(connection, (uint32_t)handle, x, y,
                                       width, height, result);
            break;
        }
        case KSD_OP_WINDOW_SET_STATE:
        case KSD_OP_WINDOW_SET_OPACITY:
        case KSD_OP_WINDOW_SET_ABOVE:
        case KSD_OP_WINDOW_SET_DECORATED: {
            ksd_cursor cursor;
            uint64_t handle = 0u;
            uint32_t value = 0u;
            ksd_cursor_init(&cursor, request->payload,
                            request->payload_length);
            (void)ksd_cursor_u64(&cursor, &handle);
            (void)ksd_cursor_u32(&cursor, &value);
            uint32_t xid = (uint32_t)handle;
            if (request->opcode == KSD_OP_WINDOW_SET_STATE)
                ksd_x11_window_set_state(connection, xid, value, result);
            else if (request->opcode == KSD_OP_WINDOW_SET_OPACITY)
                ksd_x11_window_set_opacity(connection, xid, value, result);
            else if (request->opcode == KSD_OP_WINDOW_SET_ABOVE)
                ksd_x11_window_set_above(connection, xid, value != 0u, result);
            else
                ksd_x11_window_set_decorated(connection, xid, value != 0u,
                                             result);
            break;
        }
        case KSD_OP_CLIPBOARD_MIMETYPES:
            ksd_x11_clipboard_mimetypes(connection, result);
            break;
        case KSD_OP_CLIPBOARD_TEXT:
            ksd_x11_clipboard_text(connection, result);
            break;
        case KSD_OP_CLIPBOARD_CONTENT: {
            ksd_cursor cursor;
            uint32_t length = 0u;
            const uint8_t *bytes = NULL;
            ksd_cursor_init(&cursor, request->payload,
                            request->payload_length);
            (void)ksd_cursor_u32(&cursor, &length);
            (void)ksd_cursor_bytes(&cursor, length, &bytes);
            ksd_x11_clipboard_content(connection, bytes, length, result);
            break;
        }
        default:
            ksd_result_error(result, KSD_STATUS_INVALID_REQUEST, 0u,
                             "invalid X11 request");
            break;
    }
    ksd_x11_close(connection);
}
