#include "wl_hypr.h"

#include "protocol.h"
#include "session_environ.h"
#include "transport.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#define KSD_HYPR_SIGNATURE_CAPACITY 128u
#define KSD_HYPR_RESPONSE_CAPACITY 256u
#define KSD_HYPR_CLIENTS_LIMIT (8u * 1024u * 1024u)
#define KSD_HYPR_MAX_CLIENTS 4096u
#define KSD_HYPR_TIMEOUT_MS 500
#define KSD_HYPR_JSON_DEPTH 16

static bool signature_valid(const char *signature)
{
    size_t length;

    if (signature == NULL)
        return false;
    length = strlen(signature);
    if (length == 0u || length >= KSD_HYPR_SIGNATURE_CAPACITY
        || strcmp(signature, ".") == 0 || strcmp(signature, "..") == 0)
        return false;
    for (size_t index = 0u; index < length; index++) {
        unsigned char value = (unsigned char)signature[index];
        if (!isalnum(value) && value != '-' && value != '_' && value != '.')
            return false;
    }
    return true;
}

static bool socket_path(pid_t session_pid, const char *socket, char *path,
                        size_t capacity)
{
    char signature[KSD_HYPR_SIGNATURE_CAPACITY];
    int length;

    if (!ksd_session_environ_value(session_pid,
                                   "HYPRLAND_INSTANCE_SIGNATURE",
                                   signature, sizeof(signature))
        || !signature_valid(signature))
        return false;
    length = snprintf(path, capacity, "/run/user/%lu/hypr/%s/%s",
                      (unsigned long)getuid(), signature, socket);
    return length > 0 && (size_t)length < capacity;
}

static bool socket_is_local(const char *path)
{
    struct stat status;

    return lstat(path, &status) == 0 && S_ISSOCK(status.st_mode)
        && status.st_uid == getuid();
}

#define KSD_HYPR_COMMAND_SOCKET ".socket.sock"
#define KSD_HYPR_EVENT_SOCKET ".socket2.sock"

/* Connects to one of the instance's sockets and checks that the peer is this
 * user, the same checks every request makes. Returns a nonblocking descriptor
 * or -1. */
static int connect_socket(pid_t session_pid, const char *name,
                          uint64_t deadline_ms, pid_t *peer_pid)
{
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    struct sockaddr_un address;
    struct ucred peer;
    socklen_t peer_length = sizeof(peer);
    int socket_error = 0;
    socklen_t error_length = sizeof(socket_error);
    int descriptor;

    if (!socket_path(session_pid, name, path, sizeof(path))
        || !socket_is_local(path))
        return -1;
    descriptor = socket(AF_UNIX,
                        SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (descriptor < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1u);
    if ((connect(descriptor, (const struct sockaddr *)&address,
                 sizeof(address)) != 0
         && (errno != EINPROGRESS
             || !ksd_wait_until(descriptor, POLLOUT, deadline_ms)
             || getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &socket_error,
                           &error_length) != 0
             || socket_error != 0))
        || getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &peer,
                      &peer_length) != 0
        || peer_length != sizeof(peer) || peer.uid != getuid()) {
        close(descriptor);
        return -1;
    }
    if (peer_pid != NULL)
        *peer_pid = peer.pid;
    return descriptor;
}

/* Reads the whole reply into a buffer that grows up to limit bytes. A reply
 * that would exceed the limit is a failure rather than a truncation, because
 * a cut JSON document parses as nothing useful. One deadline covers the whole
 * request, so a reply that trickles in cannot hold the worker. */
static bool hypr_request_alloc(pid_t session_pid, const char *command,
                               size_t limit, char **response, size_t *length)
{
    uint64_t deadline_ms = ksd_monotonic_milliseconds() + KSD_HYPR_TIMEOUT_MS;
    size_t command_length = strlen(command);
    int descriptor = -1;
    size_t capacity = KSD_HYPR_RESPONSE_CAPACITY;
    size_t used = 0u;
    char *buffer = NULL;
    bool success = false;

    *response = NULL;
    *length = 0u;
    buffer = malloc(capacity);
    if (buffer == NULL)
        return false;
    descriptor = connect_socket(session_pid, KSD_HYPR_COMMAND_SOCKET,
                                deadline_ms, NULL);
    if (descriptor < 0
        || ksd_transfer_until(descriptor, (void *)command, command_length,
                              true, deadline_ms)
            != (ssize_t)command_length)
        goto done;
    for (;;) {
        ssize_t count;

        if (used == capacity - 1u) {
            char *grown;

            if (capacity >= limit)
                goto done;
            capacity = capacity * 4u > limit ? limit : capacity * 4u;
            grown = realloc(buffer, capacity);
            if (grown == NULL)
                goto done;
            buffer = grown;
        }
        if (!ksd_wait_until(descriptor, POLLIN, deadline_ms))
            goto done;
        count = recv(descriptor, buffer + used, capacity - 1u - used,
                     MSG_DONTWAIT);
        if (count < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (count < 0)
            goto done;
        if (count == 0)
            break;
        used += (size_t)count;
    }
    buffer[used] = 0;
    success = used != 0u;

done:
    if (descriptor >= 0)
        close(descriptor);
    if (!success) {
        free(buffer);
        return false;
    }
    *response = buffer;
    *length = used;
    return true;
}

static bool hypr_request(pid_t session_pid, const char *command,
                         char response[KSD_HYPR_RESPONSE_CAPACITY])
{
    char *reply;
    size_t length;

    response[0] = 0;
    if (!hypr_request_alloc(session_pid, command, KSD_HYPR_RESPONSE_CAPACITY,
                            &reply, &length))
        return false;
    memcpy(response, reply, length + 1u);
    free(reply);
    return true;
}

static bool parse_integer(const char **cursor, long *value)
{
    char *end;

    while (isspace((unsigned char)**cursor))
        (*cursor)++;
    errno = 0;
    *value = strtol(*cursor, &end, 10);
    if (errno != 0 || end == *cursor || *value < INT32_MIN
        || *value > INT32_MAX)
        return false;
    *cursor = end;
    while (isspace((unsigned char)**cursor))
        (*cursor)++;
    return true;
}

static bool reply_is_ok(const char *response)
{
    const char *start = response;
    const char *end;

    while (isspace((unsigned char)*start))
        start++;
    end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1]))
        end--;
    return (size_t)(end - start) == 2u && memcmp(start, "ok", 2u) == 0;
}

pid_t ksd_wayland_hypr_pid(pid_t session_pid)
{
    pid_t pid = -1;
    int descriptor = connect_socket(session_pid, KSD_HYPR_COMMAND_SOCKET,
        ksd_monotonic_milliseconds() + KSD_HYPR_TIMEOUT_MS, &pid);

    if (descriptor < 0)
        return -1;
    close(descriptor);
    return pid;
}

bool ksd_wayland_hypr_cursor(pid_t session_pid, int32_t *x, int32_t *y)
{
    char response[KSD_HYPR_RESPONSE_CAPACITY];
    const char *cursor = response;
    long parsed_x;
    long parsed_y;

    if (x == NULL || y == NULL
        || !hypr_request(session_pid, "cursorpos", response)
        || !parse_integer(&cursor, &parsed_x) || *cursor++ != ','
        || !parse_integer(&cursor, &parsed_y))
        return false;
    while (isspace((unsigned char)*cursor))
        cursor++;
    if (*cursor != 0)
        return false;
    *x = (int32_t)parsed_x;
    *y = (int32_t)parsed_y;
    return true;
}

/* The syntax that last worked. A worker serves one compositor instance, whose
 * configuration language does not change while it runs. */
static bool dispatch_prefers_legacy;

static bool dispatch_one(pid_t session_pid, const char *expression)
{
    char command[512];
    char response[KSD_HYPR_RESPONSE_CAPACITY];
    int length;

    if (expression == NULL)
        return false;
    length = snprintf(command, sizeof(command), "dispatch %s", expression);
    return length > 0 && (size_t)length < sizeof(command)
        && hypr_request(session_pid, command, response)
        && reply_is_ok(response);
}

bool ksd_wayland_hypr_dispatch(pid_t session_pid, const char *lua,
                               const char *legacy)
{
    const char *first = dispatch_prefers_legacy ? legacy : lua;
    const char *second = dispatch_prefers_legacy ? lua : legacy;

    if (dispatch_one(session_pid, first))
        return true;
    if (!dispatch_one(session_pid, second))
        return false;
    dispatch_prefers_legacy = !dispatch_prefers_legacy;
    return true;
}

bool ksd_wayland_hypr_move(pid_t session_pid, int32_t x, int32_t y)
{
    char lua[96];
    char legacy[64];
    int lua_length = snprintf(lua, sizeof(lua),
                              "hl.dsp.cursor.move({ x = %d, y = %d })", x, y);
    int legacy_length = snprintf(legacy, sizeof(legacy),
                                 "movecursor %d %d", x, y);

    return lua_length > 0 && (size_t)lua_length < sizeof(lua)
        && legacy_length > 0 && (size_t)legacy_length < sizeof(legacy)
        && ksd_wayland_hypr_dispatch(session_pid, lua, legacy);
}

/* A reader for exactly the JSON Hyprland writes: it keeps the few scalar
 * fields a window needs and steps over everything else, however nested. */
typedef struct json_cursor {
    const char *at;
    const char *end;
} json_cursor;

static void json_space(json_cursor *cursor)
{
    while (cursor->at < cursor->end && isspace((unsigned char)*cursor->at))
        cursor->at++;
}

static bool json_take(json_cursor *cursor, char expected)
{
    json_space(cursor);
    if (cursor->at >= cursor->end || *cursor->at != expected)
        return false;
    cursor->at++;
    return true;
}

static bool json_peek(json_cursor *cursor, char expected)
{
    json_space(cursor);
    return cursor->at < cursor->end && *cursor->at == expected;
}

/* Copies a string, dropping escapes to their plain character. A value longer
 * than the destination is reported as truncated so identifiers are never
 * matched on a prefix. */
static bool json_string(json_cursor *cursor, char *out, size_t capacity,
                        bool *truncated)
{
    size_t used = 0u;

    if (truncated != NULL)
        *truncated = false;
    if (!json_take(cursor, '"'))
        return false;
    while (cursor->at < cursor->end && *cursor->at != '"') {
        char value = *cursor->at++;

        if (value == '\\') {
            if (cursor->at >= cursor->end)
                return false;
            value = *cursor->at++;
            if (value == 'u') {
                if (cursor->end - cursor->at < 4)
                    return false;
                cursor->at += 4;
                value = '?';
            } else if (value == 'n' || value == 't' || value == 'r'
                       || value == 'b' || value == 'f')
                value = ' ';
        }
        if (out == NULL)
            continue;
        if (used + 1u < capacity)
            out[used++] = value;
        else if (truncated != NULL)
            *truncated = true;
    }
    if (out != NULL && capacity != 0u)
        out[used] = 0;
    return json_take(cursor, '"');
}

static bool json_integer(json_cursor *cursor, int64_t *value)
{
    char digits[32];
    size_t used = 0u;
    char *end;

    json_space(cursor);
    while (cursor->at < cursor->end && used + 1u < sizeof(digits)
           && (isdigit((unsigned char)*cursor->at) || *cursor->at == '-'
               || *cursor->at == '+' || *cursor->at == '.'
               || *cursor->at == 'e' || *cursor->at == 'E'))
        digits[used++] = *cursor->at++;
    digits[used] = 0;
    if (used == 0u)
        return false;
    errno = 0;
    *value = strtoll(digits, &end, 10);
    /* A fractional or exponent form still yields its integer part. */
    return errno == 0 && end != digits;
}

static bool json_literal(json_cursor *cursor, const char *word)
{
    size_t length = strlen(word);

    json_space(cursor);
    if ((size_t)(cursor->end - cursor->at) < length
        || memcmp(cursor->at, word, length) != 0)
        return false;
    cursor->at += length;
    return true;
}

static bool json_boolean(json_cursor *cursor, bool *value)
{
    if (json_literal(cursor, "true"))
        *value = true;
    else if (json_literal(cursor, "false"))
        *value = false;
    else
        return false;
    return true;
}

static bool json_skip(json_cursor *cursor, int depth)
{
    int64_t ignored;

    if (depth > KSD_HYPR_JSON_DEPTH)
        return false;
    json_space(cursor);
    if (cursor->at >= cursor->end)
        return false;
    switch (*cursor->at) {
        case '"':
            return json_string(cursor, NULL, 0u, NULL);
        case '{':
            cursor->at++;
            if (json_take(cursor, '}'))
                return true;
            do {
                if (!json_string(cursor, NULL, 0u, NULL)
                    || !json_take(cursor, ':') || !json_skip(cursor, depth + 1))
                    return false;
            } while (json_take(cursor, ','));
            return json_take(cursor, '}');
        case '[':
            cursor->at++;
            if (json_take(cursor, ']'))
                return true;
            do {
                if (!json_skip(cursor, depth + 1))
                    return false;
            } while (json_take(cursor, ','));
            return json_take(cursor, ']');
        case 't':
            return json_literal(cursor, "true");
        case 'f':
            return json_literal(cursor, "false");
        case 'n':
            return json_literal(cursor, "null");
        default:
            return json_integer(cursor, &ignored);
    }
}

static bool json_pair(json_cursor *cursor, int32_t *first, int32_t *second)
{
    int64_t a;
    int64_t b;

    if (!json_take(cursor, '[') || !json_integer(cursor, &a)
        || !json_take(cursor, ',') || !json_integer(cursor, &b)
        || !json_take(cursor, ']')
        || a < INT32_MIN || a > INT32_MAX || b < INT32_MIN || b > INT32_MAX)
        return false;
    *first = (int32_t)a;
    *second = (int32_t)b;
    return true;
}

static bool json_workspace(json_cursor *cursor, ksd_hypr_client *client)
{
    char key[16];
    bool truncated;

    if (!json_take(cursor, '{'))
        return false;
    if (json_take(cursor, '}'))
        return true;
    do {
        if (!json_string(cursor, key, sizeof(key), NULL)
            || !json_take(cursor, ':'))
            return false;
        if (strcmp(key, "id") == 0) {
            if (!json_integer(cursor, &client->workspace_id))
                return false;
        } else if (strcmp(key, "name") == 0) {
            if (!json_string(cursor, client->workspace_name,
                             sizeof(client->workspace_name), &truncated))
                return false;
            if (truncated)
                client->workspace_name[0] = 0;
        } else if (!json_skip(cursor, 1))
            return false;
    } while (json_take(cursor, ','));
    return json_take(cursor, '}');
}

static bool address_valid(const char *address)
{
    if (address[0] != '0' || address[1] != 'x' || address[2] == 0)
        return false;
    for (const char *at = address + 2; *at != 0; at++)
        if (!isxdigit((unsigned char)*at))
            return false;
    return true;
}

static bool json_client(json_cursor *cursor, ksd_hypr_client *client)
{
    char key[32];
    bool truncated = false;
    bool ids_truncated = false;
    int64_t number;

    memset(client, 0, sizeof(*client));
    client->mapped = true;
    client->visible = true;
    client->focus_history = INT32_MAX;
    if (!json_take(cursor, '{'))
        return false;
    if (json_take(cursor, '}'))
        return false;
    do {
        bool ok = true;

        if (!json_string(cursor, key, sizeof(key), NULL)
            || !json_take(cursor, ':'))
            return false;
        if (strcmp(key, "address") == 0) {
            ok = json_string(cursor, client->address, sizeof(client->address),
                             &truncated);
            ids_truncated |= truncated;
        } else if (strcmp(key, "stableId") == 0) {
            ok = json_string(cursor, client->stable_id,
                             sizeof(client->stable_id), &truncated);
            ids_truncated |= truncated;
        } else if (strcmp(key, "mapped") == 0)
            ok = json_boolean(cursor, &client->mapped);
        else if (strcmp(key, "hidden") == 0)
            ok = json_boolean(cursor, &client->hidden);
        else if (strcmp(key, "visible") == 0)
            ok = json_boolean(cursor, &client->visible);
        else if (strcmp(key, "floating") == 0)
            ok = json_boolean(cursor, &client->floating);
        else if (strcmp(key, "at") == 0)
            ok = json_pair(cursor, &client->x, &client->y);
        else if (strcmp(key, "size") == 0)
            ok = json_pair(cursor, &client->width, &client->height);
        else if (strcmp(key, "workspace") == 0)
            ok = json_workspace(cursor, client);
        else if (strcmp(key, "pid") == 0)
            ok = json_integer(cursor, &client->pid);
        else if (strcmp(key, "fullscreen") == 0) {
            /* An integer mode on current releases, a boolean on old ones. */
            bool flag = false;

            if (json_peek(cursor, 't') || json_peek(cursor, 'f')) {
                ok = json_boolean(cursor, &flag);
                client->fullscreen = flag ? 1 : 0;
            } else {
                ok = json_integer(cursor, &number);
                client->fullscreen = number > 0 ? 1 : 0;
            }
        } else if (strcmp(key, "focusHistoryID") == 0) {
            ok = json_integer(cursor, &number);
            if (ok)
                client->focus_history = number < 0 || number > INT32_MAX
                    ? INT32_MAX : (int32_t)number;
        } else
            ok = json_skip(cursor, 1);
        if (!ok)
            return false;
    } while (json_take(cursor, ','));
    if (!json_take(cursor, '}'))
        return false;
    return !ids_truncated && client->stable_id[0] != 0
        && address_valid(client->address);
}

bool ksd_wayland_hypr_parse_clients(const char *json, size_t length,
                                    ksd_hypr_client **clients, size_t *count)
{
    json_cursor cursor = { .at = json, .end = json + length };
    ksd_hypr_client *items = NULL;
    size_t used = 0u;
    size_t capacity = 0u;

    *clients = NULL;
    *count = 0u;
    if (json == NULL || !json_take(&cursor, '['))
        return false;
    if (!json_take(&cursor, ']')) {
        do {
            ksd_hypr_client client;
            json_cursor start = cursor;

            if (!json_client(&cursor, &client)) {
                /* An element without the fields needed to address a
                 * client is skipped; only malformed JSON ends the parse. */
                cursor = start;
                if (!json_skip(&cursor, 1))
                    goto fail;
                continue;
            }
            if (used == KSD_HYPR_MAX_CLIENTS)
                continue;
            if (used == capacity) {
                size_t grown_capacity = capacity == 0u ? 16u : capacity * 2u;
                ksd_hypr_client *grown = realloc(items,
                    grown_capacity * sizeof(*items));

                if (grown == NULL)
                    goto fail;
                items = grown;
                capacity = grown_capacity;
            }
            items[used++] = client;
        } while (json_take(&cursor, ','));
        if (!json_take(&cursor, ']'))
            goto fail;
    }
    *clients = items;
    *count = used;
    return true;

fail:
    free(items);
    return false;
}

bool ksd_wayland_hypr_clients(pid_t session_pid, ksd_hypr_client **clients,
                              size_t *count)
{
    char *reply;
    size_t length;
    bool parsed;

    *clients = NULL;
    *count = 0u;
    if (!hypr_request_alloc(session_pid, "j/clients", KSD_HYPR_CLIENTS_LIMIT,
                            &reply, &length))
        return false;
    parsed = ksd_wayland_hypr_parse_clients(reply, length, clients, count);
    free(reply);
    return parsed;
}

bool ksd_wayland_hypr_active_workspace(pid_t session_pid, int64_t *id)
{
    char *reply;
    size_t length;
    json_cursor cursor;
    char key[16];
    bool found = false;

    if (!hypr_request_alloc(session_pid, "j/activeworkspace", 64u * 1024u,
                            &reply, &length))
        return false;
    cursor.at = reply;
    cursor.end = reply + length;
    if (json_take(&cursor, '{') && !json_take(&cursor, '}')) {
        do {
            if (!json_string(&cursor, key, sizeof(key), NULL)
                || !json_take(&cursor, ':'))
                break;
            if (strcmp(key, "id") == 0) {
                found = json_integer(&cursor, id);
                break;
            }
            if (!json_skip(&cursor, 1))
                break;
        } while (json_take(&cursor, ','));
    }
    free(reply);
    return found;
}

bool ksd_wayland_hypr_opacity_text(uint32_t opacity, char *out,
                                   size_t capacity)
{
    int length;

    if (opacity > 255u || out == NULL)
        return false;
    length = snprintf(out, capacity, "%.6f", opacity / 255.0);
    return length > 0 && (size_t)length < capacity;
}

bool ksd_wayland_hypr_parse_opacity(const char *text, size_t length,
                                    uint32_t *opacity)
{
    char value[64];
    const char *start = text;
    const char *end = text + length;
    char *parsed_end;
    double parsed;

    while (start < end && isspace((unsigned char)*start))
        start++;
    while (end > start && isspace((unsigned char)end[-1]))
        end--;
    if (end == start || (size_t)(end - start) >= sizeof(value))
        return false;
    memcpy(value, start, (size_t)(end - start));
    value[end - start] = 0;
    errno = 0;
    parsed = strtod(value, &parsed_end);
    /* NaN fails both comparisons; Hyprland accepts values above 1. */
    if (errno != 0 || *parsed_end != 0 || !(parsed >= 0.0) || parsed > 1e6)
        return false;
    if (parsed > 1.0)
        parsed = 1.0;
    *opacity = (uint32_t)(parsed * 255.0 + 0.5);
    return true;
}

bool ksd_wayland_hypr_parse_opacity_batch(const char *reply, size_t length,
                                          int32_t *values, size_t count)
{
    static const char separator[] = "\n\n\n";
    const char *at = reply;
    const char *end = reply + length;

    for (size_t index = 0u; index < count; index++) {
        const char *next = at;
        uint32_t opacity;

        while (next < end
               && !((size_t)(end - next) >= sizeof(separator) - 1u
                    && memcmp(next, separator, sizeof(separator) - 1u) == 0))
            next++;
        if (at == end && index != 0u)
            return false;
        values[index] = ksd_wayland_hypr_parse_opacity(at, (size_t)(next - at),
                                                       &opacity)
            ? (int32_t)opacity : -1;
        at = next < end ? next + sizeof(separator) - 1u : end;
    }
    return at == end;
}

/* Hyprland reads a batch in one go, so a very long one is split. */
#define KSD_HYPR_OPACITY_BATCH 64u

bool ksd_wayland_hypr_opacities(pid_t session_pid,
                                const char *const *addresses, size_t count,
                                int32_t *values)
{
    for (size_t first = 0u; first < count; first += KSD_HYPR_OPACITY_BATCH) {
        size_t chunk = count - first < KSD_HYPR_OPACITY_BATCH
            ? count - first : KSD_HYPR_OPACITY_BATCH;
        char command[KSD_HYPR_OPACITY_BATCH * (KSD_HYPR_ID_CAPACITY + 32u)
                     + 16u];
        size_t used = 0u;
        char *reply;
        size_t length;
        bool parsed;
        int written = snprintf(command, sizeof(command), "[[BATCH]]");

        if (written <= 0)
            return false;
        used = (size_t)written;
        for (size_t index = 0u; index < chunk; index++) {
            /* A batch separates commands with semicolons, so only a
             * well-formed address may be spliced into one. */
            if (!address_valid(addresses[first + index]))
                return false;
            written = snprintf(command + used, sizeof(command) - used,
                               "%sgetprop address:%s opacity",
                               index == 0u ? "" : ";",
                               addresses[first + index]);
            if (written <= 0 || (size_t)written >= sizeof(command) - used)
                return false;
            used += (size_t)written;
        }
        if (!hypr_request_alloc(session_pid, command, 64u * 1024u, &reply,
                                &length))
            return false;
        parsed = ksd_wayland_hypr_parse_opacity_batch(reply, length,
                                                      values + first, chunk);
        free(reply);
        if (!parsed)
            return false;
    }
    return true;
}

uint32_t ksd_wayland_hypr_signal_owner(pid_t pid,
                                       bool (*still_owner)(void *context,
                                                           pid_t pid),
                                       void *context)
{
    int descriptor;
    long sent;

    /* Hyprland reports -1 for a client whose surface is gone, and a pid of 0
     * or 1, or this worker's own, is never a window's owner to kill. Nor is
     * any pid whose ownership cannot be confirmed. */
    if (pid <= 1 || pid == getpid() || still_owner == NULL)
        return KSD_STATUS_UNAVAILABLE;
    descriptor = (int)syscall(SYS_pidfd_open, pid, 0);
    if (descriptor < 0)
        return errno == ESRCH ? KSD_STATUS_NOT_FOUND : KSD_STATUS_UNAVAILABLE;
    if (!still_owner(context, pid)) {
        close(descriptor);
        return KSD_STATUS_NOT_FOUND;
    }
    sent = syscall(SYS_pidfd_send_signal, descriptor, SIGKILL, NULL, 0);
    close(descriptor);
    return sent == 0 || errno == ESRCH ? KSD_STATUS_OK
                                       : KSD_STATUS_UNAVAILABLE;
}

/* One keyboard entry. Fields arrive in any order. */
static bool json_keyboard(json_cursor *cursor, bool *main,
                          ksd_hypr_layout *layout, bool *complete)
{
    char key[32];
    bool truncated;
    bool has_index = false;
    bool has_name = false;
    int64_t index;

    *main = false;
    *complete = false;
    if (!json_take(cursor, '{'))
        return false;
    if (json_take(cursor, '}'))
        return true;
    do {
        if (!json_string(cursor, key, sizeof(key), NULL)
            || !json_take(cursor, ':'))
            return false;
        if (strcmp(key, "main") == 0) {
            if (!json_boolean(cursor, main))
                return false;
        } else if (strcmp(key, "active_layout_index") == 0) {
            if (json_peek(cursor, '"')) {
                if (!json_skip(cursor, 2))
                    return false;
            } else {
                if (!json_integer(cursor, &index))
                    return false;
                has_index = index >= 0 && index <= (int64_t)UINT32_MAX;
                if (has_index)
                    layout->index = (uint32_t)index;
            }
        } else if (strcmp(key, "active_keymap") == 0) {
            if (!json_string(cursor, layout->name, sizeof(layout->name),
                             &truncated))
                return false;
            has_name = !truncated && layout->name[0] != 0;
        } else if (!json_skip(cursor, 2))
            return false;
    } while (json_take(cursor, ','));
    *complete = has_index && has_name;
    return json_take(cursor, '}');
}

bool ksd_wayland_hypr_parse_active_layout(const char *json, size_t length,
                                          ksd_hypr_layout *layout)
{
    json_cursor cursor = { .at = json, .end = json + length };
    char key[32];
    size_t mains = 0u;
    bool complete_main = false;

    if (json == NULL || !json_take(&cursor, '{') || json_take(&cursor, '}'))
        return false;
    do {
        if (!json_string(&cursor, key, sizeof(key), NULL)
            || !json_take(&cursor, ':'))
            return false;
        if (strcmp(key, "keyboards") != 0) {
            if (!json_skip(&cursor, 1))
                return false;
            continue;
        }
        if (!json_take(&cursor, '['))
            return false;
        if (json_take(&cursor, ']'))
            continue;
        do {
            ksd_hypr_layout entry = { 0 };
            bool main;
            bool complete;

            if (!json_keyboard(&cursor, &main, &entry, &complete))
                return false;
            if (!main)
                continue;
            mains++;
            complete_main = complete;
            *layout = entry;
        } while (json_take(&cursor, ','));
        if (!json_take(&cursor, ']'))
            return false;
    } while (json_take(&cursor, ','));
    /* Two keyboards claiming main is a state no group can be read from. */
    return json_take(&cursor, '}') && mains == 1u && complete_main;
}

bool ksd_wayland_hypr_active_layout(pid_t session_pid,
                                    ksd_hypr_layout *layout)
{
    char *reply;
    size_t length;
    bool parsed;

    if (!hypr_request_alloc(session_pid, "j/devices", 256u * 1024u, &reply,
                            &length))
        return false;
    parsed = ksd_wayland_hypr_parse_active_layout(reply, length, layout);
    free(reply);
    return parsed;
}

static bool json_reserved(json_cursor *cursor, int32_t reserved[4])
{
    int64_t values[4];

    if (!json_take(cursor, '['))
        return false;
    for (size_t index = 0u; index < 4u; index++) {
        if ((index != 0u && !json_take(cursor, ','))
            || !json_integer(cursor, &values[index])
            || values[index] < INT32_MIN || values[index] > INT32_MAX)
            return false;
    }
    if (!json_take(cursor, ']'))
        return false;
    for (size_t index = 0u; index < 4u; index++)
        reserved[index] = (int32_t)values[index];
    return true;
}

/* Returns false for a malformed entry; *usable says whether a well-formed
 * one names an enabled monitor completely. */
static bool json_monitor(json_cursor *cursor, ksd_hypr_monitor *monitor,
                         bool *usable)
{
    char key[32];
    bool truncated = false;
    bool disabled = false;
    bool has_name = false;
    bool has_x = false;
    bool has_y = false;
    bool has_reserved = false;
    int64_t number;

    memset(monitor, 0, sizeof(*monitor));
    *usable = false;
    if (!json_take(cursor, '{'))
        return false;
    if (json_take(cursor, '}'))
        return true;
    do {
        if (!json_string(cursor, key, sizeof(key), NULL)
            || !json_take(cursor, ':'))
            return false;
        if (strcmp(key, "name") == 0) {
            if (!json_string(cursor, monitor->name, sizeof(monitor->name),
                             &truncated))
                return false;
            has_name = !truncated && monitor->name[0] != 0;
        } else if (strcmp(key, "x") == 0 || strcmp(key, "y") == 0) {
            if (!json_integer(cursor, &number) || number < INT32_MIN
                || number > INT32_MAX)
                return false;
            if (key[0] == 'x') {
                monitor->x = (int32_t)number;
                has_x = true;
            } else {
                monitor->y = (int32_t)number;
                has_y = true;
            }
        } else if (strcmp(key, "reserved") == 0) {
            if (!json_reserved(cursor, monitor->reserved))
                return false;
            has_reserved = true;
        } else if (strcmp(key, "focused") == 0) {
            if (!json_boolean(cursor, &monitor->focused))
                return false;
        } else if (strcmp(key, "disabled") == 0) {
            if (!json_boolean(cursor, &disabled))
                return false;
        } else if (!json_skip(cursor, 2))
            return false;
    } while (json_take(cursor, ','));
    *usable = has_name && has_x && has_y && has_reserved && !disabled;
    return json_take(cursor, '}');
}

#define KSD_HYPR_MAX_MONITORS 64u

bool ksd_wayland_hypr_parse_monitors(const char *json, size_t length,
                                     ksd_hypr_monitor **monitors,
                                     size_t *count)
{
    json_cursor cursor = { .at = json, .end = json + length };
    ksd_hypr_monitor *items;
    size_t used = 0u;

    *monitors = NULL;
    *count = 0u;
    if (json == NULL || !json_take(&cursor, '['))
        return false;
    items = calloc(KSD_HYPR_MAX_MONITORS, sizeof(*items));
    if (items == NULL)
        return false;
    if (!json_take(&cursor, ']')) {
        do {
            ksd_hypr_monitor monitor;
            bool usable;

            if (!json_monitor(&cursor, &monitor, &usable))
                goto fail;
            if (usable && used < KSD_HYPR_MAX_MONITORS)
                items[used++] = monitor;
        } while (json_take(&cursor, ','));
        if (!json_take(&cursor, ']'))
            goto fail;
    }
    *monitors = items;
    *count = used;
    return true;

fail:
    free(items);
    return false;
}

bool ksd_wayland_hypr_monitors(pid_t session_pid, ksd_hypr_monitor **monitors,
                               size_t *count)
{
    char *reply;
    size_t length;
    bool parsed;

    *monitors = NULL;
    *count = 0u;
    if (!hypr_request_alloc(session_pid, "j/monitors", 1024u * 1024u, &reply,
                            &length))
        return false;
    parsed = ksd_wayland_hypr_parse_monitors(reply, length, monitors, count);
    free(reply);
    return parsed;
}

int ksd_wayland_hypr_events_open(pid_t session_pid)
{
    return connect_socket(session_pid, KSD_HYPR_EVENT_SOCKET,
                          ksd_monotonic_milliseconds() + KSD_HYPR_TIMEOUT_MS,
                          NULL);
}

void ksd_wayland_hypr_event_reader_init(ksd_hypr_event_reader *reader)
{
    reader->used = 0u;
    reader->discarding = false;
}

static void emit_line(ksd_hypr_event_reader *reader,
                      void (*on_line)(void *, const char *), void *context)
{
    char *separator;

    reader->line[reader->used] = 0;
    separator = strstr(reader->line, ">>");
    if (separator != NULL)
        *separator = 0;
    on_line(context, separator != NULL ? reader->line : "");
}

void ksd_wayland_hypr_event_feed(ksd_hypr_event_reader *reader,
                                 const char *bytes, size_t length,
                                 void (*on_line)(void *context,
                                                 const char *name),
                                 void *context)
{
    for (size_t index = 0u; index < length; index++) {
        char value = bytes[index];

        if (value == '\n') {
            if (reader->discarding)
                on_line(context, "");
            else
                emit_line(reader, on_line, context);
            reader->used = 0u;
            reader->discarding = false;
            continue;
        }
        if (reader->discarding)
            continue;
        if (reader->used == sizeof(reader->line) - 1u) {
            reader->discarding = true;
            continue;
        }
        reader->line[reader->used++] = value;
    }
}

bool ksd_wayland_hypr_event_affects_windows(const char *name)
{
    static const char *const names[] = {
        "openwindow", "closewindow", "windowtitle", "windowtitlev2",
        "activewindow", "activewindowv2", "movewindow", "movewindowv2",
        "changefloatingmode", "fullscreen", "pin", "workspace",
        "workspacev2", "focusedmon", "focusedmonv2", "activespecial",
        "activespecialv2", "createworkspace", "createworkspacev2",
        "destroyworkspace", "destroyworkspacev2", "moveworkspace",
        "moveworkspacev2", "monitoradded", "monitoraddedv2",
        "monitorremoved", "monitorremovedv2", "togglegroup",
        "moveintogroup", "moveoutofgroup", "configreloaded",
    };

    /* A line that could not be read may have been any of these. */
    if (name == NULL || name[0] == 0)
        return true;
    for (size_t index = 0u; index < sizeof(names) / sizeof(names[0]); index++)
        if (strcmp(name, names[index]) == 0)
            return true;
    return false;
}
