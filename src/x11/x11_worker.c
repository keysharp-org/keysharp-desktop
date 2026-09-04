#include "x11_worker.h"

#include "protocol.h"
#include "protocol_io.h"
#include "x11_capture.h"
#include "x11_clipboard.h"
#include "x11_connect.h"
#include "x11_display.h"
#include "x11_query.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define KSD_X11_ENVIRON_LIMIT (64u * 1024u)
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

/* Reads one variable out of the environment of the registered session daemon.
 * The daemon is the party the authority authenticated and revalidates on every
 * operation, which is why its environment is the one entitled to name a
 * display; taking that from the calling client would let a client point the
 * broker at a server it started. Runs after privileges are dropped, so the
 * open is done as the user and root never touches a user-named path. */
static bool environ_value(pid_t pid, const char *name, char *destination,
                          size_t capacity)
{
    char path[64];
    char environment[KSD_X11_ENVIRON_LIMIT + 1u];
    size_t offset = 0u;
    size_t name_length = strlen(name);
    int length = snprintf(path, sizeof(path), "/proc/%ld/environ", (long)pid);

    if (pid <= 0 || length <= 0 || (size_t)length >= sizeof(path))
        return false;
    int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return false;
    while (offset < sizeof(environment) - 1u) {
        ssize_t count = read(descriptor, environment + offset,
                             sizeof(environment) - 1u - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        offset += (size_t)count;
    }
    close(descriptor);
    environment[offset] = 0;

    for (size_t index = 0u; index < offset;) {
        const char *entry = environment + index;
        size_t remaining = offset - index;
        size_t entry_length = strnlen(entry, remaining);

        if (entry_length == remaining)
            return false;
        if (entry_length > name_length && entry[name_length] == '='
            && memcmp(entry, name, name_length) == 0) {
            const char *value = entry + name_length + 1u;
            size_t value_length = entry_length - name_length - 1u;

            if (value_length == 0u || value_length >= capacity)
                return false;
            memcpy(destination, value, value_length);
            destination[value_length] = 0;
            return true;
        }
        index += entry_length + 1u;
    }
    return false;
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
    if (!environ_value(session_pid, "DISPLAY", display, sizeof(display))) {
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
    if (environ_value(session_pid, "XAUTHORITY", authority, sizeof(authority))
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
