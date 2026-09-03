#include "x11_worker.h"

#include "protocol.h"
#include "protocol_io.h"
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
        default:
            ksd_result_error(result, KSD_STATUS_INVALID_REQUEST, 0u,
                             "invalid X11 request");
            break;
    }
    ksd_x11_close(connection);
}
