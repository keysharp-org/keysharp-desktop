#include "wl_worker.h"

#include "protocol.h"
#include "protocol_io.h"
#include "session_environ.h"
#include "wl_clipboard.h"
#include "wl_connect.h"
#include "wl_windows.h"

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
            return request->payload_length == 0u;
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
        default:
            return false;
    }
}

void ksd_wayland_execute(const ksd_frame *request, pid_t session_pid,
                         ksd_operation_result *result)
{
    char display[KSD_WL_DISPLAY_CAPACITY];
    ksd_wayland *connection = NULL;
    ksd_status status;

    if (!ksd_wayland_request_valid(request)) {
        ksd_result_error(result, KSD_STATUS_INVALID_REQUEST, 0u,
                         "invalid Wayland request");
        return;
    }
    /* From the registered daemon's environment, never the caller's: a client
     * that could name the compositor could point this at one it started. */
    if (!ksd_session_environ_value(session_pid, "WAYLAND_DISPLAY", display,
                                   sizeof(display))) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                         "the session names no Wayland display");
        return;
    }
    if (!display_name_valid(display)) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                         "the session names a display this service will not "
                         "open");
        return;
    }
    status = ksd_wayland_open(display, &connection);
    if (status != KSD_STATUS_OK) {
        ksd_result_error(result, status, 0u,
                         "could not reach the compositor for this session");
        return;
    }
    switch (request->opcode) {
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
        case KSD_OP_WINDOW_LIST:
            ksd_wayland_window_list(connection, result);
            break;
        case KSD_OP_WINDOW_HANDLES:
            ksd_wayland_window_handles(connection, result);
            break;
        default:
            ksd_result_error(result, KSD_STATUS_INVALID_REQUEST, 0u,
                             "invalid Wayland request");
            break;
    }
    ksd_wayland_close(connection);
}
