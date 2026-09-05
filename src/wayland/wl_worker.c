#include "wl_worker.h"

#include "protocol.h"
#include "protocol_io.h"
#include "portal_capture.h"
#include "session_environ.h"
#include "wl_clipboard.h"
#include "wl_connect.h"
#include "wl_capture.h"
#include "wl_pointer.h"
#include "wl_hypr.h"
#include "wl_windows.h"
#include "wl_keyboard.h"

#include <string.h>

#define KSD_WL_DISPLAY_CAPACITY 256u

/* A Wayland display name is a socket name inside XDG_RUNTIME_DIR, or an
 * absolute path. Neither may contain a separator when it is a name, because a
 * name with one in it would resolve somewhere other than the runtime
 * directory: the same class of rule the X11 display grammar enforces, for the
 * same reason. An absolute path is accepted because the protocol allows one
 * and some session managers use it. */
static bool display_name_valid(const char *name)
{
    size_t length;

    if (name == NULL)
        return false;
    length = strlen(name);
    if (length == 0u || length >= KSD_WL_DISPLAY_CAPACITY)
        return false;
    if (name[0] == '/')
        return true;
    return strchr(name, '/') == NULL && strcmp(name, ".") != 0
        && strcmp(name, "..") != 0;
}
bool ksd_wayland_request_valid(const ksd_frame *request)
{
    ksd_cursor cursor;

    if (request == NULL)
        return false;
    ksd_cursor_init(&cursor, request->payload, request->payload_length);
    switch (request->opcode) {
        case KSD_OP_CLIPBOARD_MIMETYPES:
        case KSD_OP_CLIPBOARD_TEXT:
        case KSD_OP_CAPTURE_DESKTOP:
            return request->payload_length == 0u;
        case KSD_OP_KEYBOARD_STATE: {
            uint32_t length;
            const uint8_t *revision;
            return request->payload_length == 0u
                || (ksd_cursor_u32(&cursor, &length) && length <= 64u
                    && ksd_cursor_bytes(&cursor, length, &revision)
                    && ksd_cursor_finished(&cursor)
                    && ksd_utf8_valid(revision, length, false));
        }
        case KSD_OP_CLIPBOARD_CONTENT: {
            uint32_t length;
            const uint8_t *bytes;
            return ksd_cursor_u32(&cursor, &length)
                && length != 0u && length <= KSD_MAX_MIMETYPE_BYTES
                && ksd_cursor_bytes(&cursor, length, &bytes)
                && ksd_cursor_finished(&cursor)
                && ksd_utf8_valid(bytes, length, false);
        }
        case KSD_OP_WINDOW_HANDLES:
            return request->payload_length == 0u;
        case KSD_OP_WINDOW_ACTIVE:
        case KSD_OP_CURSOR_POSITION:
            return request->payload_length == 0u;
        case KSD_OP_WINDOW_FOCUS:
        case KSD_OP_WINDOW_CLOSE: {
            uint64_t handle;
            return ksd_cursor_u64(&cursor, &handle)
                && ksd_cursor_finished(&cursor) && handle != 0u;
        }
        case KSD_OP_WINDOW_QUERY: {
            uint64_t handle;
            return ksd_cursor_u64(&cursor, &handle)
                && ksd_cursor_finished(&cursor) && handle != 0u;
        }
        case KSD_OP_WINDOW_SET_STATE: {
            uint64_t handle;
            uint32_t value;
            uint32_t reserved;
            return ksd_cursor_u64(&cursor, &handle)
                && ksd_cursor_u32(&cursor, &value)
                && ksd_cursor_u32(&cursor, &reserved)
                && ksd_cursor_finished(&cursor) && handle != 0u
                && value <= 2u && reserved == 0u;
        }
        case KSD_OP_WINDOW_LIST: {
            uint32_t include_hidden;
            uint32_t reserved;
            /* The argument is accepted and then ignored, and saying so is
             * better than pretending. This protocol reports every toplevel the
             * compositor chooses to expose and offers no way to ask for more
             * or fewer, so there is no hidden set to include. */
            return ksd_cursor_u32(&cursor, &include_hidden)
                && ksd_cursor_u32(&cursor, &reserved)
                && include_hidden <= 1u && reserved == 0u
                && ksd_cursor_finished(&cursor);
        }
        case KSD_OP_CAPTURE_AREA: {
            int32_t x;
            int32_t y;
            uint32_t width;
            uint32_t height;
            return ksd_cursor_i32(&cursor, &x)
                && ksd_cursor_i32(&cursor, &y)
                && ksd_cursor_u32(&cursor, &width)
                && ksd_cursor_u32(&cursor, &height)
                && ksd_cursor_finished(&cursor)
                && width != 0u && height != 0u
                && width <= KSD_MAX_CAPTURE_DIMENSION
                && height <= KSD_MAX_CAPTURE_DIMENSION
                && (uint64_t)width * height <= KSD_MAX_CAPTURE_PIXELS
                && (int64_t)x + width <= INT32_MAX
                && (int64_t)y + height <= INT32_MAX;
        }
        case KSD_OP_MOUSE_MOVE_ABSOLUTE: {
            int32_t x;
            int32_t y;
            return ksd_cursor_i32(&cursor, &x)
                && ksd_cursor_i32(&cursor, &y)
                && ksd_cursor_finished(&cursor);
        }
        default:
            return false;
    }
}

ksd_status ksd_wayland_open_for_session(pid_t session_pid,
                                        struct ksd_wayland **connection)
{
    char display[KSD_WL_DISPLAY_CAPACITY];

    if (connection == NULL)
        return KSD_STATUS_INVALID_REQUEST;
    *connection = NULL;
    /* From the registered daemon's environment, never the caller's: a client
     * that could name the compositor could point this at one it started. */
    if (!ksd_session_environ_value(session_pid, "WAYLAND_DISPLAY", display,
                                   sizeof(display)))
        return KSD_STATUS_UNAVAILABLE;
    if (!display_name_valid(display))
        return KSD_STATUS_UNAVAILABLE;
    ksd_status status = ksd_wayland_open(display, connection);
    if (status == KSD_STATUS_OK)
        ksd_wayland_set_session_pid(*connection, session_pid);
    return status;
}

bool ksd_wayland_execute_on(struct ksd_wayland *connection,
                            const ksd_frame *request,
                            ksd_operation_result *result)
{
    if (connection == NULL || !ksd_wayland_request_valid(request)) {
        ksd_result_error(result, KSD_STATUS_INVALID_REQUEST, 0u,
                         "invalid Wayland request");
        return true;
    }
    switch (request->opcode) {
        case KSD_OP_KEYBOARD_STATE:
            ksd_wayland_keyboard_state_since(connection,
                request->payload_length == 0u ? NULL : request->payload + 4u,
                request->payload_length == 0u ? 0u
                    : ksd_decode_u32(request->payload), result);
            break;
        case KSD_OP_CLIPBOARD_MIMETYPES:
            ksd_wayland_clipboard_mimetypes(connection, result);
            break;
        case KSD_OP_CLIPBOARD_TEXT:
            ksd_wayland_clipboard_text(connection, result);
            break;
        case KSD_OP_CLIPBOARD_CONTENT: {
            ksd_cursor cursor;
            uint32_t length = 0u;
            const uint8_t *bytes = NULL;
            ksd_cursor_init(&cursor, request->payload,
                            request->payload_length);
            (void)ksd_cursor_u32(&cursor, &length);
            (void)ksd_cursor_bytes(&cursor, length, &bytes);
            ksd_wayland_clipboard_content(connection, bytes, length, result);
            break;
        }
        case KSD_OP_CAPTURE_DESKTOP:
            ksd_portal_capture_desktop(result);
            break;
        case KSD_OP_WINDOW_LIST:
        {
            ksd_cursor cursor;
            uint32_t include_hidden = 0u;
            ksd_cursor_init(&cursor, request->payload,
                            request->payload_length);
            (void)ksd_cursor_u32(&cursor, &include_hidden);
            ksd_wayland_window_list(connection, include_hidden != 0u,
                                     result);
            break;
        }
        case KSD_OP_WINDOW_HANDLES:
            ksd_wayland_window_handles(connection, result);
            break;
        case KSD_OP_WINDOW_QUERY:
            ksd_wayland_window_query(connection,
                ksd_decode_u64(request->payload), result);
            break;
        case KSD_OP_WINDOW_ACTIVE:
            ksd_wayland_active_window(connection, result);
            break;
        case KSD_OP_WINDOW_FOCUS:
        case KSD_OP_WINDOW_CLOSE: {
            ksd_cursor cursor;
            uint64_t handle = 0u;
            ksd_cursor_init(&cursor, request->payload,
                            request->payload_length);
            (void)ksd_cursor_u64(&cursor, &handle);
            ksd_wayland_window_action(connection, request->opcode, handle, 0u,
                                       result);
            break;
        }
        case KSD_OP_WINDOW_SET_STATE: {
            ksd_cursor cursor;
            uint64_t handle = 0u;
            uint32_t value = 0u;
            ksd_cursor_init(&cursor, request->payload,
                            request->payload_length);
            (void)ksd_cursor_u64(&cursor, &handle);
            (void)ksd_cursor_u32(&cursor, &value);
            ksd_wayland_window_action(connection, request->opcode, handle,
                                       value, result);
            break;
        }
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
            ksd_wayland_capture_area(connection, x, y, width, height,
                                     result);
            break;
        }
        case KSD_OP_MOUSE_MOVE_ABSOLUTE: {
            ksd_cursor cursor;
            int32_t x = 0;
            int32_t y = 0;
            ksd_cursor_init(&cursor, request->payload,
                            request->payload_length);
            (void)ksd_cursor_i32(&cursor, &x);
            (void)ksd_cursor_i32(&cursor, &y);
            ksd_wayland_move_absolute(connection, x, y, result);
            break;
        }
        case KSD_OP_CURSOR_POSITION: {
            int32_t x;
            int32_t y;
            uint8_t tail[8];
            if (!ksd_wayland_hypr_cursor(
                    ksd_wayland_session_pid(connection), &x, &y)) {
                ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                                 "the compositor does not expose cursor "
                                 "position");
                break;
            }
            ksd_encode_u32(tail, (uint32_t)x);
            ksd_encode_u32(tail + 4u, (uint32_t)y);
            if (!ksd_result_copy(result, tail, sizeof(tail)))
                ksd_result_error(result, KSD_STATUS_INTERNAL, 0u,
                                 "could not return cursor position");
            break;
        }
        default:
            ksd_result_error(result, KSD_STATUS_INVALID_REQUEST, 0u,
                             "invalid Wayland request");
            break;
    }
    return !ksd_wayland_connection_failed(connection);
}
