#include "backend.h"
#include <fcntl.h>

#include "transport.h"

#include "kwin_bus.h"
#include "portal_capture.h"
#include "wl_connect.h"
#include "backend_protocol.h"
#include "install_mode.h"
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
#define KSD_BACKEND_RECHECK_MILLISECONDS 2000

/* An authority socket, and the directory holding it, must belong to the party
 * whose authority it claims to be -- otherwise anyone who could create it
 * could answer in its place.
 *
 * `owner` is root for the system installation and this process's own uid for a
 * user one. Accepting our own is not a weakening: a socket this user created
 * grants this user nothing they did not already have. Accepting somebody
 * else's would be, which is why there is no third case.
 */
static bool owned_path(const char *path, uid_t owner, bool socket_path,
                       unsigned socket_mode)
{
    struct stat status;
    if (lstat(path, &status) != 0 || status.st_uid != owner)
        return false;
    if (socket_path)
        return S_ISSOCK(status.st_mode)
            && (status.st_mode & 0777u) == socket_mode;
    return S_ISDIR(status.st_mode)
        && (status.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

/* A user authority's directory has to be private outright, not merely
 * unwritable by others. The system directory is world-traversable by design --
 * every user has to reach the socket in it -- but nobody except its owner has
 * any reason to see inside a user one. */
static bool private_directory(const char *path, uid_t owner)
{
    struct stat status;
    return lstat(path, &status) == 0 && S_ISDIR(status.st_mode)
        && status.st_uid == owner
        && (status.st_mode & (S_IRWXG | S_IRWXO)) == 0u;
}

/* Connects to one authority and checks that the far end really is the party
 * that owns the socket. `owner` runs the whole way through: it decides which
 * socket is acceptable and which peer is, and the two have to be the same
 * party or the check means nothing. */
static int connect_to(const char *directory, const char *path, uid_t owner,
                      unsigned socket_mode, uint64_t deadline)
{
    struct sockaddr_un address;
    struct ucred peer;
    socklen_t peer_size = sizeof(peer);
    int socket_error = 0;
    socklen_t socket_error_size = sizeof(socket_error);
    size_t length = strlen(path);
    int descriptor;

    if (!owned_path(directory, owner, false, 0u))
        return -1;
    if (length >= sizeof(address.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (!owned_path(path, owner, true, socket_mode))
        return -1;
    descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (descriptor < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, length + 1u);
    if (connect(descriptor, (const struct sockaddr *)&address,
                sizeof(address)) != 0
        && (errno != EINPROGRESS
            || !ksd_wait_until(descriptor, POLLOUT, deadline)))
        goto failed;
    if (getsockopt(descriptor, SOL_SOCKET, SO_ERROR,
                   &socket_error, &socket_error_size) != 0
        || socket_error_size != sizeof(socket_error) || socket_error != 0) {
        if (socket_error != 0)
            errno = socket_error;
        goto failed;
    }
    /* The socket being owned by the right party says who created it, not who
     * is answering on it now. */
    if (getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED,
                  &peer, &peer_size) != 0
        || peer_size != sizeof(peer) || peer.uid != owner)
        goto failed;
    return descriptor;

failed:
    close(descriptor);
    return -1;
}

/* The system authority first, then this user's own.
 *
 * The order is what keeps the fallback honest: where a root authority is
 * running, it is the one that answers, and a user authority cannot displace it
 * by starting first. The fallback is reached only when there is no system
 * installation to fall back from.
 */
static int connect_backend(uint64_t deadline)
{
    char user_socket[108];
    char user_directory[108];
    char *slash;
    int descriptor = connect_to("/run/keysharp-desktop", KSD_SYSTEM_SOCKET,
                                0u, 0666u, deadline);

    if (descriptor >= 0)
        return descriptor;
    /* Never as root: a root client asking a user-owned authority for
     * permission would be taking the answer from a less trusted party than
     * itself. The session daemon already refuses to run as root, and this
     * makes the socket choice refuse it too rather than rely on that. */
    if (geteuid() == 0u || getuid() == 0u
        || !ksd_install_socket_path(user_socket, sizeof(user_socket)))
        return -1;
    if (strlen(user_socket) >= sizeof(user_directory))
        return -1;
    strcpy(user_directory, user_socket);
    slash = strrchr(user_directory, '/');
    if (slash == NULL || slash == user_directory)
        return -1;
    *slash = 0;
    if (!private_directory(user_directory, ksd_install_owner()))
        return -1;
    return connect_to(user_directory, user_socket, ksd_install_owner(),
                      ksd_install_socket_mode(), deadline);
}

/* What this compositor actually advertises, which is not knowable from a
 * static table: two compositors of the same kind differ, and the generic
 * backend is by definition every compositor nobody wrote an extension for.
 * Probed once at registration and reported, so the authority stores the truth
 * rather than the ceiling and a client is told what it can really have.
 *
 * A probe that cannot connect narrows to nothing rather than falling back to
 * the ceiling: claiming capability this daemon could not demonstrate is the
 * one direction the registration mask exists to prevent. */
static uint64_t probe_generic_operations(void)
{
    ksd_wayland *connection = NULL;
    ksd_wayland_features features;
    uint64_t operations = 0u;

    if (ksd_wayland_open(NULL, &connection) != KSD_STATUS_OK)
        return 0u;
    features = ksd_wayland_supported(connection);
    if (features.data_control)
        operations |= KSD_OPERATION_CLIPBOARD_MIMETYPES
            | KSD_OPERATION_CLIPBOARD_CONTENT | KSD_OPERATION_CLIPBOARD_TEXT;
    if (features.toplevel_list)
        operations |= KSD_OPERATION_WINDOW_LIST | KSD_OPERATION_WINDOW_HANDLES
            | KSD_OPERATION_WINDOW_QUERY;
    if (features.toplevel_active)
        operations |= KSD_OPERATION_WINDOW_ACTIVE;
    if (features.toplevel_focus)
        operations |= KSD_OPERATION_WINDOW_FOCUS;
    if (features.toplevel_close)
        operations |= KSD_OPERATION_WINDOW_CLOSE;
    if (features.toplevel_state)
        operations |= KSD_OPERATION_WINDOW_SET_STATE;
    if (features.screencopy)
        operations |= KSD_OPERATION_CAPTURE_AREA;
    if (ksd_portal_capture_available())
        operations |= KSD_OPERATION_CAPTURE_DESKTOP;
    if (features.absolute_pointer)
        operations |= KSD_OPERATION_MOUSE_MOVE_ABSOLUTE;
    if (features.cursor_position)
        operations |= KSD_OPERATION_CURSOR_POSITION;
    if (features.keyboard_keymap)
        operations |= KSD_OPERATION_KEYBOARD_STATE;
    ksd_wayland_close(connection);
    return operations;
}

/* A fresh 32-hex-digit token naming this run of the script. Issued by the
 * daemon and never chosen by the script, because a script that named its own
 * run could name the previous one and answer for its jobs. */
static bool issue_generation(char out[KSD_KWIN_GENERATION_HEX + 1u])
{
    static const char digits[] = "0123456789abcdef";
    unsigned char bytes[KSD_KWIN_GENERATION_HEX / 2u];
    int source = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    size_t filled = 0u;

    if (source < 0)
        return false;
    while (filled < sizeof(bytes)) {
        ssize_t count = read(source, bytes + filled, sizeof(bytes) - filled);

        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            close(source);
            return false;
        }
        filled += (size_t)count;
    }
    close(source);
    for (size_t index = 0u; index < sizeof(bytes); index++) {
        out[index * 2u] = digits[bytes[index] >> 4];
        out[index * 2u + 1u] = digits[bytes[index] & 0x0fu];
    }
    out[KSD_KWIN_GENERATION_HEX] = '\0';
    return true;
}

static bool backend_is_current(ksd_backend backend)
{
    ksd_backend current = ksd_backend_resolve();
    return backend == KSD_BACKEND_GENERIC
        ? current == KSD_BACKEND_NONE : current == backend;
}

static bool probe_keyboard_keymap(void)
{
    ksd_wayland *connection = NULL;
    if (ksd_wayland_open(NULL, &connection) != KSD_STATUS_OK)
        return false;
    bool supported = ksd_wayland_supported(connection).keyboard_keymap;
    ksd_wayland_close(connection);
    return supported;
}

static bool register_backend(int descriptor, ksd_backend backend,
                             uint64_t deadline, int provider_fd,
                             uint64_t *accepted)
{
    uint64_t requested = backend == KSD_BACKEND_GENERIC
        ? probe_generic_operations() : ksd_backend_operations(backend);
    if (backend != KSD_BACKEND_GENERIC && backend != KSD_BACKEND_X11
        && !probe_keyboard_keymap())
        requested &= ~KSD_OPERATION_KEYBOARD_STATE;
    uint8_t message[KSD_BACKEND_REGISTRATION_SIZE] = { 0 };
    uint8_t reply[KSD_BACKEND_REGISTRATION_SIZE] = { 0 };
    memcpy(message, ksd_backend_registration_magic,
           sizeof(ksd_backend_registration_magic));
    ksd_encode_u16(message + 4u, KSD_BACKEND_REGISTRATION_VERSION);
    ksd_encode_u16(message + 6u, provider_fd >= 0
        ? KSD_BACKEND_FLAG_PROVIDER_FD : 0u);
    ksd_encode_u32(message + 8u, backend);
    /* What this daemon believes it can serve. Today that is everything the
     * backend statically supports; a daemon that probes its compositor and
     * finds a capability missing reports less here, and the authority narrows
     * to it. It can never widen. */
    ksd_encode_u64(message + 16u, requested);
    /* The acknowledgement is parsed by the shared codec rather than checked
     * field by field here, so the two ends cannot drift on the layout, and so
     * the withhold-only rule is enforced from this side too. */
    /* The callback socket rides the registration itself rather than following
     * it. A second message would leave a window in which the authority has
     * accepted a backend it cannot call back, and every request arriving in
     * that window would be refused for a reason that is about to stop being
     * true. */
    if (provider_fd >= 0) {
        if (!ksd_send_with_fd(descriptor, message, sizeof(message),
                              provider_fd))
            return false;
    } else if (ksd_transfer_until(descriptor, message, sizeof(message), true,
                                  deadline) != (ssize_t)sizeof(message)) {
        return false;
    }
    return ksd_transfer_until(descriptor, reply, sizeof(reply), false,
                              deadline) == (ssize_t)sizeof(reply)
        && ksd_backend_ack_parse(reply, backend, requested, accepted);
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
                    " session; registering the generic backend, which serves"
                    " what the shared Wayland protocols allow\n"
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
            /* Name the provider because its absence is the usual cause. */
            fputs("keysharp-desktop daemon: waiting for a compositor provider."
                  " On GNOME and Cinnamon this is the keysharp shell"
                  " extension: enable it, then log out and back in. KWin needs"
                  " no extension, but does need a kwin_wayland session.\n",
                  stderr);
        }
        while (nanosleep(&retry, &retry) != 0 && errno == EINTR) {
        }
    }
    uint64_t now = ksd_monotonic_milliseconds();
    uint64_t deadline = now == 0u
        ? 0u : now + KSD_BACKEND_REGISTRATION_TIMEOUT_MS;
    uint64_t accepted = 0u;
    int provider_pair[2] = { -1, -1 };
    /* Only KWin needs one, because only a KWin script cannot be reached on the
     * session bus the way a shell extension can. The authority refuses the
     * flag from any other backend, so offering one would be a rejected
     * registration rather than a harmless extra. */
    if (backend == KSD_BACKEND_KWIN
        && socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0,
                      provider_pair) != 0) {
        provider_pair[0] = -1;
        provider_pair[1] = -1;
    }
    int descriptor = deadline == 0u ? -1 : connect_backend(deadline);
    if (descriptor < 0) {
        if (provider_pair[0] >= 0)
            close(provider_pair[0]);
        if (provider_pair[1] >= 0)
            close(provider_pair[1]);
        fputs("keysharp-desktop daemon: could not connect to or authenticate"
              " the authority socket. Check that the authority socket is"
              " running and owned by the installation's authority user\n",
              stderr);
        return 1;
    }
    if (!register_backend(descriptor, backend, deadline, provider_pair[1],
                          &accepted)) {
        if (provider_pair[0] >= 0)
            close(provider_pair[0]);
        if (provider_pair[1] >= 0)
            close(provider_pair[1]);
        close(descriptor);
        fputs("keysharp-desktop daemon: the authority rejected this session"
              " backend registration. Check the authority journal and make"
              " sure both services use the same installation\n", stderr);
        return 1;
    }
    /* The authority owns its end now. Holding a copy here would keep the
     * socket alive after the authority closed it, and this end would then
     * never see the hangup that says the registration is over. */
    if (provider_pair[1] >= 0)
        close(provider_pair[1]);
    /* Said out loud when it happens. The authority may narrow what this daemon
     * asked to advertise, and a narrowing that goes unreported is the kind of
     * thing that gets diagnosed as "the compositor is broken" from the far end
     * of a client, long after the fact. */
    if (accepted != ksd_backend_operations(backend))
        fprintf(stderr, "keysharp-desktop daemon: the authority accepted"
                " %llu of the operations offered, not all of them\n",
                (unsigned long long)__builtin_popcountll(accepted));
    signal(SIGPIPE, SIG_IGN);
    /* KWin, and only KWin, needs a bus name and a main loop: its script cannot
     * be reached any other way. Every other backend keeps the plain poll below
     * untouched, because a main loop none of them uses is one that can go
     * wrong for them without buying anything.
     *
     * The loop watches the authority socket itself, so the rule is the same
     * one the poll follows: traffic or hangup there ends the daemon, because
     * the registration is its whole reason to be running. */
    if (backend == KSD_BACKEND_KWIN) {
        char generation[KSD_KWIN_GENERATION_HEX + 1u];

        if (issue_generation(generation)) {
            ksd_kwin_queue *queue = ksd_kwin_queue_create(generation);
            ksd_kwin_bus *bus = queue == NULL ? NULL
                : ksd_kwin_bus_start(queue, provider_pair[0]);

            if (bus != NULL) {
                (void)ksd_kwin_bus_run(bus, descriptor);
                ksd_kwin_bus_stop(bus);
                ksd_kwin_queue_destroy(queue);
                close(descriptor);
                return 1;
            }
            ksd_kwin_queue_destroy(queue);
        }
        /* Falling through to the plain loop is deliberate. Without the bus the
         * script has nothing to call, but the registration is still live and
         * capture still works, which is more use than exiting. */
        fputs("keysharp-desktop daemon: could not take the KWin provider"
              " name; window operations will be unavailable\n", stderr);
    }
    for (;;) {
        struct pollfd item = {
            .fd = descriptor,
            .events = POLLIN | POLLRDHUP | POLLHUP | POLLERR,
        };
        int ready = poll(&item, 1u, KSD_BACKEND_RECHECK_MILLISECONDS);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready == 0) {
            if (backend_is_current(backend))
                continue;
            fputs("keysharp-desktop daemon: compositor changed; restarting"
                  " the session backend\n", stderr);
            break;
        }
        if (ready < 0 || (item.revents
            & (POLLIN | POLLRDHUP | POLLHUP | POLLERR | POLLNVAL)) != 0)
            break;
    }
    close(descriptor);
    return 1;
}
