#include "wl_hypr.h"

#include "session_environ.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define KSD_HYPR_SIGNATURE_CAPACITY 128u
#define KSD_HYPR_RESPONSE_CAPACITY 256u
#define KSD_HYPR_TIMEOUT_MS 500

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

static bool socket_path(pid_t session_pid, char *path, size_t capacity)
{
    char signature[KSD_HYPR_SIGNATURE_CAPACITY];
    int length;

    if (!ksd_session_environ_value(session_pid,
                                   "HYPRLAND_INSTANCE_SIGNATURE",
                                   signature, sizeof(signature))
        || !signature_valid(signature))
        return false;
    length = snprintf(path, capacity, "/run/user/%lu/hypr/%s/.socket.sock",
                      (unsigned long)getuid(), signature);
    return length > 0 && (size_t)length < capacity;
}

static bool socket_is_local(const char *path)
{
    struct stat status;

    return lstat(path, &status) == 0 && S_ISSOCK(status.st_mode)
        && status.st_uid == getuid();
}

static bool wait_socket(int descriptor, short events)
{
    struct pollfd item = { .fd = descriptor, .events = events };
    int ready;

    do {
        ready = poll(&item, 1u, KSD_HYPR_TIMEOUT_MS);
    } while (ready < 0 && errno == EINTR);
    return ready > 0 && (item.revents & (events | POLLHUP)) != 0
        && (item.revents & (POLLERR | POLLNVAL)) == 0;
}

static bool send_all(int descriptor, const char *command)
{
    size_t length = strlen(command);
    size_t offset = 0u;

    while (offset < length) {
        if (!wait_socket(descriptor, POLLOUT))
            return false;
        ssize_t count = send(descriptor, command + offset, length - offset,
                             MSG_DONTWAIT | MSG_NOSIGNAL);
        if (count < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (count <= 0)
            return false;
        offset += (size_t)count;
    }
    return true;
}

static bool hypr_request(pid_t session_pid, const char *command,
                         char response[KSD_HYPR_RESPONSE_CAPACITY])
{
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    struct sockaddr_un address;
    struct ucred peer;
    socklen_t peer_length = sizeof(peer);
    int descriptor = -1;
    int socket_error = 0;
    socklen_t error_length = sizeof(socket_error);
    size_t used = 0u;
    bool success = false;

    response[0] = 0;
    if (!socket_path(session_pid, path, sizeof(path))
        || !socket_is_local(path))
        return false;
    descriptor = socket(AF_UNIX,
                        SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (descriptor < 0)
        return false;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1u);
    if (connect(descriptor, (const struct sockaddr *)&address,
                sizeof(address)) != 0) {
        if (errno != EINPROGRESS || !wait_socket(descriptor, POLLOUT)
            || getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &socket_error,
                          &error_length) != 0
            || socket_error != 0)
            goto done;
    }
    if (getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &peer,
                   &peer_length) != 0
        || peer_length != sizeof(peer) || peer.uid != getuid()
        || !send_all(descriptor, command))
        goto done;
    for (;;) {
        ssize_t count;

        if (used == KSD_HYPR_RESPONSE_CAPACITY - 1u
            || !wait_socket(descriptor, POLLIN))
            goto done;
        count = recv(descriptor, response + used,
                     KSD_HYPR_RESPONSE_CAPACITY - 1u - used, MSG_DONTWAIT);
        if (count < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (count < 0)
            goto done;
        if (count == 0)
            break;
        used += (size_t)count;
    }
    response[used] = 0;
    success = used != 0u;

done:
    close(descriptor);
    return success;
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

bool ksd_wayland_hypr_available(pid_t session_pid)
{
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];

    return socket_path(session_pid, path, sizeof(path))
        && socket_is_local(path);
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

bool ksd_wayland_hypr_move(pid_t session_pid, int32_t x, int32_t y)
{
    char command[96];
    char response[KSD_HYPR_RESPONSE_CAPACITY];
    int length = snprintf(command, sizeof(command),
                          "dispatch movecursor %d %d", x, y);

    if (length <= 0 || (size_t)length >= sizeof(command)
        || !hypr_request(session_pid, command, response))
        return false;
    char *start = response;
    while (isspace((unsigned char)*start))
        start++;
    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1]))
        end--;
    return (size_t)(end - start) == 2u && memcmp(start, "ok", 2u) == 0;
}
