#include "backend.h"
#include "backend_protocol.h"
#include "protocol_io.h"
#include "roles.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define KSD_BACKEND_STARTUP_RETRY_SECONDS 5
#define KSD_BACKEND_STARTUP_ATTEMPTS 24u

static bool root_owned_path(const char *path, bool socket_path)
{
    struct stat status;
    if (lstat(path, &status) != 0 || status.st_uid != 0u)
        return false;
    if (socket_path)
        return S_ISSOCK(status.st_mode)
            && (status.st_mode & 0777u) == 0666u;
    return S_ISDIR(status.st_mode)
        && (status.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

static uint64_t monotonic_milliseconds(void)
{
    struct timespec now;
    return clock_gettime(CLOCK_MONOTONIC, &now) == 0
        ? (uint64_t)now.tv_sec * 1000u
            + (uint64_t)now.tv_nsec / 1000000u
        : 0u;
}

static bool wait_for(int descriptor, short events, uint64_t deadline)
{
    for (;;) {
        uint64_t now = monotonic_milliseconds();
        if (now == 0u || now >= deadline) {
            errno = ETIMEDOUT;
            return false;
        }
        uint64_t remaining = deadline - now;
        struct pollfd item = { .fd = descriptor, .events = events };
        int ready = poll(&item, 1u, remaining > (uint64_t)INT_MAX
            ? INT_MAX : (int)remaining);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready == 0)
            errno = ETIMEDOUT;
        if (ready <= 0 || (item.revents & (POLLERR | POLLNVAL)) != 0)
            return false;
        return (item.revents & (events | POLLHUP | POLLRDHUP)) != 0;
    }
}

static bool transfer_fixed(int descriptor, void *data, size_t length,
                           bool write_data, uint64_t deadline)
{
    uint8_t *bytes = data;
    size_t offset = 0u;
    while (offset < length) {
        if (!wait_for(descriptor, write_data ? POLLOUT : POLLIN, deadline))
            return false;
        ssize_t count = write_data
            ? send(descriptor, bytes + offset, length - offset,
                   MSG_DONTWAIT | MSG_NOSIGNAL)
            : recv(descriptor, bytes + offset, length - offset,
                   MSG_DONTWAIT);
        if (count < 0 && (errno == EINTR || errno == EAGAIN
                          || errno == EWOULDBLOCK))
            continue;
        if (count <= 0)
            return false;
        offset += (size_t)count;
    }
    return true;
}

static int connect_backend(uint64_t deadline)
{
    struct sockaddr_un address;
    struct ucred peer;
    socklen_t peer_size = sizeof(peer);
    int socket_error = 0;
    socklen_t socket_error_size = sizeof(socket_error);
    int descriptor = socket(AF_UNIX,
                            SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);

    if (descriptor < 0 || !root_owned_path("/run/keysharp-desktop", false))
        goto failed;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (strlen(KSD_SYSTEM_SOCKET) >= sizeof(address.sun_path)) {
        errno = ENAMETOOLONG;
        goto failed;
    }
    memcpy(address.sun_path, KSD_SYSTEM_SOCKET,
           sizeof(KSD_SYSTEM_SOCKET));
    if (!root_owned_path(KSD_SYSTEM_SOCKET, true))
        goto failed;
    if (connect(descriptor, (const struct sockaddr *)&address,
                sizeof(address)) != 0
        && (errno != EINPROGRESS
            || !wait_for(descriptor, POLLOUT, deadline)))
        goto failed;
    if (getsockopt(descriptor, SOL_SOCKET, SO_ERROR,
                   &socket_error, &socket_error_size) != 0
        || socket_error_size != sizeof(socket_error) || socket_error != 0) {
        if (socket_error != 0)
            errno = socket_error;
        goto failed;
    }
    if (getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED,
                  &peer, &peer_size) != 0
        || peer_size != sizeof(peer) || peer.uid != 0u || peer.gid != 0u)
        goto failed;
    return descriptor;

failed:
    if (descriptor >= 0)
        close(descriptor);
    return -1;
}

static bool register_backend(int descriptor, ksd_backend backend,
                             uint64_t deadline)
{
    uint8_t message[KSD_BACKEND_REGISTRATION_SIZE] = { 0 };
    uint8_t reply[KSD_BACKEND_REGISTRATION_SIZE] = { 0 };
    memcpy(message, ksd_backend_registration_magic,
           sizeof(ksd_backend_registration_magic));
    ksd_encode_u16(message + 4u, KSD_BACKEND_REGISTRATION_VERSION);
    ksd_encode_u32(message + 8u, backend);
    return transfer_fixed(descriptor, message, sizeof(message), true, deadline)
        && transfer_fixed(descriptor, reply, sizeof(reply), false, deadline)
        && memcmp(reply, ksd_backend_ack_magic,
                  sizeof(ksd_backend_ack_magic)) == 0
        && ksd_decode_u16(reply + 4u)
            == KSD_BACKEND_REGISTRATION_VERSION
        && ksd_decode_u16(reply + 6u) == KSD_BACKEND_ACK_ACCEPTED
        && ksd_decode_u32(reply + 8u) == backend
        && ksd_decode_u32(reply + 12u) == 0u;
}

int ksd_daemon_main(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        fputs("usage: keysharp-desktop daemon\n", stderr);
        return 2;
    }
    if (getuid() == 0u || getuid() != geteuid() || getgid() != getegid()) {
        fputs("keysharp-desktop daemon: refusing elevated credentials\n",
              stderr);
        return 1;
    }
    ksd_backend backend;
    bool waiting = false;
    unsigned attempts = 0u;
    while ((backend = ksd_backend_resolve()) == KSD_BACKEND_NONE) {
        struct timespec retry = {
            .tv_sec = KSD_BACKEND_STARTUP_RETRY_SECONDS,
        };
        bool unsupported = ksd_backend_session_unsupported();
        if (unsupported || attempts >= KSD_BACKEND_STARTUP_ATTEMPTS) {
            backend = KSD_BACKEND_GENERIC;
            fputs(unsupported
                  ? "keysharp-desktop daemon: no supported compositor in this"
                    " session; registering the generic backend, which"
                    " advertises no operations\n"
                  : "keysharp-desktop daemon: no provider appeared;"
                    " registering the generic backend. Enable the shell"
                    " extension and run systemctl --user restart"
                    " keysharp-desktop.service\n",
                  stderr);
            break;
        }
        attempts++;
        if (!waiting) {
            waiting = true;
            fputs("keysharp-desktop daemon: waiting for a supported"
                  " compositor; KWin capture needs a kwin_wayland session\n",
                  stderr);
        }
        while (nanosleep(&retry, &retry) != 0 && errno == EINTR) {
        }
    }
    uint64_t now = monotonic_milliseconds();
    uint64_t deadline = now == 0u
        ? 0u : now + KSD_BACKEND_REGISTRATION_TIMEOUT_MS;
    int descriptor = deadline == 0u ? -1 : connect_backend(deadline);
    if (descriptor < 0 || !register_backend(descriptor, backend, deadline)) {
        if (descriptor >= 0)
            close(descriptor);
        fputs("keysharp-desktop daemon: authority unavailable. The root"
              " authority socket is not running: enable it with"
              " systemctl enable --now keysharp-desktop-authority.socket\n",
              stderr);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    for (;;) {
        struct pollfd item = {
            .fd = descriptor,
            .events = POLLIN | POLLRDHUP | POLLHUP | POLLERR,
        };
        int ready = poll(&item, 1u, -1);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0 || (item.revents
            & (POLLIN | POLLRDHUP | POLLHUP | POLLERR | POLLNVAL)) != 0)
            break;
    }
    close(descriptor);
    return 1;
}
