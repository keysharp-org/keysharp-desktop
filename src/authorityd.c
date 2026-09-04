#include "backend.h"
#include "worker_pool.h"
#include "backend_protocol.h"
#include "install_mode.h"
#include "capture_worker.h"
#include "kwin_relay.h"
#include "kwin_wire.h"

/* A display query is bounded by its own deadline rather than the KWin one: a
 * clipboard read waits on another application to answer and is given five
 * seconds by the backends themselves, so a shorter bound here would time out
 * an operation that was going to succeed. */
#define KSD_DISPLAY_DEADLINE_MS 8000u
#include "protocol.h"
#include "operation_result.h"
#include "operation_scope.h"
#include "permission_domain.h"
#include "protocol_io.h"
#include "provider.h"
#include "roles.h"
#include "transport.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <grp.h>
#include <keysharp_permissions/permissions.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define KSD_POLKIT_ACTION "org.keysharp.desktop.grant"
/* Both live in worker_pool.h, beside the admission rules that use them. */
#define KSD_MAX_BACKEND_REGISTRATIONS 128u
#define KSD_BACKEND_REGISTRATION_POLL_MS 1000
#define KSD_GENERIC_REGISTRATION_POLL_MS 30000
#define KSD_MAX_CONCURRENT_CAPTURES 4u
#define KSD_MAX_CAPTURES_PER_UID 2u
/* Every consumer of one desktop shares a uid, so the per-uid cap alone lets a
 * single process hold both of its slots and answer RESOURCE_EXHAUSTED to every
 * other process on that desktop for as long as a capture takes. The per-uid
 * cap stays, because it is the only cross-uid guarantee there is: two slots
 * per uid against four global means a second user can always capture.
 * Re-keying purely on pid would trade a same-user annoyance for a cross-user
 * denial. A process that forks past this is bounded by the per-uid cap. */
#define KSD_MAX_CAPTURES_PER_PID 1u
#define KSD_MAX_AUTHORITY_INFLIGHT_BYTES \
    (KSD_MAX_CONCURRENT_CAPTURES * (size_t)KSD_MAX_CAPTURE_BYTES \
     + 16u * 1024u * 1024u)
#define KSD_MAX_AUTHORITY_ASSEMBLY_BYTES (64u * 1024u * 1024u)
#define KSD_MAX_ASSEMBLIES_PER_UID 4u
#define KSD_MAX_REQUEST_ASSEMBLY_SECONDS 10u
#define KSD_GENERATION_POLL_MS 250
#ifndef KSD_PKCHECK_PATH
#define KSD_PKCHECK_PATH "/usr/bin/pkcheck"
#endif

_Static_assert(KSD_MAX_AUTHORITY_ASSEMBLY_BYTES
                   >= KSD_MAX_REQUEST_TOTAL_PAYLOAD,
               "the assembly budget must admit one chunked request");

typedef struct authority_state {
    pthread_mutex_t mutex;
    size_t workers;
    size_t inflight_bytes;
    size_t assembly_bytes;
    struct {
        uid_t uid;
        size_t count;
    } worker_usage[KSD_MAX_AUTHORITY_WORKERS];
    struct {
        uid_t uid;
        size_t count;
    } assembly_usage[KSD_MAX_AUTHORITY_WORKERS];
    struct {
        uid_t uid;
        size_t count;
    } capture_usage[KSD_MAX_AUTHORITY_WORKERS];
    struct {
        pid_t pid;
        size_t count;
    } capture_pid_usage[KSD_MAX_AUTHORITY_WORKERS];
    /* KWin requests in flight per consumer. Keyed on pid because every
     * consumer of a desktop shares a uid, and on a compositor that runs every
     * script operation on one thread it is the individual consumer that has to
     * be held back. */
    struct {
        pid_t pid;
        size_t count;
    } kwin_inflight[KSD_MAX_AUTHORITY_WORKERS];
    struct {
        bool active;
        uid_t uid;
        int descriptor;
        uint32_t backend;
        /* What this daemon reported it can serve, already narrowed to what the
         * backend statically supports. Read in place of the static table, so a
         * daemon whose compositor lacks a capability can say so. */
        uint64_t advertised;
        /* The socket this daemon handed over for the authority to call back
         * on. Only KWin sends one, because only a KWin script cannot be
         * reached on the session bus. -1 for every other backend. */
        int provider_fd;
        /* Wraps provider_fd once the registration is accepted, so the many
         * connection threads can share one socket without serialising on it. */
        ksd_kwin_relay *relay;
        /* The persistent display worker for this session, and the relay that
         * drives it. Started on the first display request and kept, so the
         * fork, exec, privilege drop and display connect are paid once for the
         * session rather than once per query -- measured at 1.4 ms of process
         * creation alone against 0.38 ms for a full forty-window enumeration.
         *
         * Keyed by the registration, not by the client, because the worker
         * drops to a CLIENT's uid, gid and supplementary groups. Clients of one
         * desktop share those in practice; one whose groups differ is served by
         * a one-shot worker rather than by somebody else's. */
        ksd_kwin_relay *display_relay;
        uid_t display_uid;
        gid_t display_gid;
        ksp_identity identity;
        /* Captured from the compositor bus owner returned by the authority's
         * credential-dropped query. The private provider socket is pinned to
         * this process without attempting a root connection to a user bus that
         * will reject it. Empty for backends without a shell provider. */
        ksp_identity provider_identity;
    } backends[KSD_MAX_BACKEND_REGISTRATIONS];
    ksp_store *store;
} authority_state;

typedef struct authority_session {
    authority_state *state;
    int client_fd;
    ksp_identity identity;
    gid_t gid;
    uint16_t role;
    uint32_t backend;
    uint32_t requested_scopes;
    uint32_t granted_scopes;
    uint64_t generation;
    uint64_t identity_checked_at;
    uint64_t assembly_deadline;
    int descriptor;
    ksd_request_assembly assembly;
} authority_session;

typedef struct authority_client {
    authority_state *state;
    int descriptor;
    struct ucred credentials;
    /* Set when the slot came out of the reserve held for registrations, which
     * an ordinary connection may not keep. */
    bool from_reserve;
} authority_client;
static const uint8_t public_magic[4] = {
    KSD_FRAME_MAGIC_0, KSD_FRAME_MAGIC_1,
    KSD_FRAME_MAGIC_2, KSD_FRAME_MAGIC_3,
};

static bool same_identity(const ksp_identity *left,
                          const ksp_identity *right)
{
    return left->uid == right->uid && left->pid == right->pid
        && left->start_time == right->start_time
        && strcmp(left->hash, right->hash) == 0;
}

static uint64_t monotonic_seconds(void)
{
    struct timespec now;
    return clock_gettime(CLOCK_MONOTONIC, &now) == 0
        ? (uint64_t)now.tv_sec : 0u;
}

static bool decimal_equals(const char *text, unsigned long expected)
{
    char *end = NULL;
    errno = 0;
    unsigned long value = text == NULL ? 0u : strtoul(text, &end, 10);
    return text != NULL && text[0] != '\0' && errno == 0
        && end != NULL && *end == '\0' && value == expected;
}

static int inherited_socket(bool *activation_present)
{
    const char *listen_pid = getenv("LISTEN_PID");
    const char *listen_fds = getenv("LISTEN_FDS");
    const char *listen_names = getenv("LISTEN_FDNAMES");
    struct sockaddr_un address;
    socklen_t address_size = sizeof(address);
    int type = 0;
    int accepting = 0;
    socklen_t option_size = sizeof(int);
    struct stat status;
    char expected[sizeof(address.sun_path)];

    *activation_present = listen_pid != NULL || listen_fds != NULL
        || listen_names != NULL;
    if (!*activation_present)
        return -1;
    if (!decimal_equals(listen_pid, (unsigned long)getpid())
        || !decimal_equals(listen_fds, 1u)
        || listen_names == NULL || strcmp(listen_names, "public") != 0
        || fstat(3, &status) != 0 || !S_ISSOCK(status.st_mode)
        || getsockopt(3, SOL_SOCKET, SO_TYPE, &type, &option_size) != 0
        || option_size != sizeof(type) || type != SOCK_STREAM) {
        errno = EINVAL;
        return -1;
    }
    option_size = sizeof(accepting);
    memset(&address, 0, sizeof(address));
    if (getsockopt(3, SOL_SOCKET, SO_ACCEPTCONN,
                   &accepting, &option_size) != 0
        || option_size != sizeof(accepting) || accepting != 1
        || getsockname(3, (struct sockaddr *)&address, &address_size) != 0
        || address.sun_family != AF_UNIX
        || address_size <= offsetof(struct sockaddr_un, sun_path)
        /* The activated socket has to be the one this installation would have
         * bound itself. A unit that handed over some other socket would have
         * the authority answering on a name it never chose, and in a user
         * installation that name is where the client will look. */
        || !ksd_install_socket_path(expected, sizeof(expected))
        || strncmp(address.sun_path, expected,
                   sizeof(address.sun_path)) != 0
        || fcntl(3, F_SETFD, FD_CLOEXEC) != 0) {
        errno = EINVAL;
        return -1;
    }
    (void)unsetenv("LISTEN_PID");
    (void)unsetenv("LISTEN_FDS");
    (void)unsetenv("LISTEN_FDNAMES");
    return 3;
}

static int create_socket(void)
{
    struct sockaddr_un address;
    char path[sizeof(address.sun_path)];
    int descriptor;
    size_t length;

    /* System-wide when this is root, and under the user's own runtime
     * directory otherwise. Whether a user directory is fit to hold a socket
     * has already been decided against the same standard root gives the system
     * one, so that judgement is not repeated here. */
    if (!ksd_install_socket_path(path, sizeof(path)))
        return -1;
    /* 0755 for a system installation, whose parent every user has to traverse
     * to reach the socket; 0700 for a user one, which nobody else has any
     * business entering. */
    if (ksd_make_parent_directories(path,
                                    ksd_install_is_system() ? 0755 : 0700) != 0)
        return -1;
    if (!ksd_install_is_system()) {
        char directory[sizeof(path)];
        char *slash;

        /* The mode is set rather than assumed, because this directory is
         * usually not created here: the permission store reaches it first and
         * makes it world-readable, which would leave the socket sitting
         * somewhere anyone could list. */
        memcpy(directory, path, strlen(path) + 1u);
        slash = strrchr(directory, '/');
        if (slash == NULL || slash == directory
            || (*slash = 0, chmod(directory, 0700) != 0))
            return -1;
    }
    descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (descriptor < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    length = strlen(path);
    if (length >= sizeof(address.sun_path)) {
        close(descriptor);
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(address.sun_path, path, length + 1u);
    (void)unlink(path);
    if (bind(descriptor, (const struct sockaddr *)&address,
             sizeof(address)) != 0
        || chmod(path, (mode_t)ksd_install_socket_mode()) != 0
        || listen(descriptor, (int)KSD_MAX_AUTHORITY_WORKERS) != 0) {
        close(descriptor);
        return -1;
    }
    return descriptor;
}

static uint64_t monotonic_milliseconds(void)
{
    struct timespec now;
    return clock_gettime(CLOCK_MONOTONIC, &now) == 0
        ? (uint64_t)now.tv_sec * 1000u
            + (uint64_t)now.tv_nsec / 1000000u
        : 0u;
}

static bool set_socket_timeouts(int descriptor, uint32_t seconds)
{
    struct timeval timeout = {
        .tv_sec = (time_t)seconds,
        .tv_usec = 0,
    };
    return setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO,
                      &timeout, sizeof(timeout)) == 0
        && setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO,
                      &timeout, sizeof(timeout)) == 0;
}

static bool wait_until(int descriptor, short events, uint64_t deadline)
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
        if (ready <= 0 || (item.revents & (POLLERR | POLLNVAL)) != 0)
            return false;
        return (item.revents & (events | POLLHUP | POLLRDHUP)) != 0;
    }
}

static bool fixed_io_until(int descriptor, void *data, size_t length,
                           bool write_data, uint64_t deadline)
{
    uint8_t *bytes = data;
    size_t offset = 0u;
    while (offset < length) {
        if (!wait_until(descriptor, write_data ? POLLOUT : POLLIN, deadline))
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

static bool peek_connection_magic(int descriptor, uint8_t magic[4])
{
    uint64_t now = monotonic_milliseconds();
    if (now == 0u)
        return false;
    uint64_t deadline = now + KSD_BACKEND_REGISTRATION_TIMEOUT_MS;
    for (;;) {
        int available = 0;
        if (ioctl(descriptor, FIONREAD, &available) != 0)
            return false;
        if (available >= 4) {
            ssize_t count = recv(descriptor, magic, 4u,
                                 MSG_PEEK | MSG_DONTWAIT);
            return count == 4;
        }
        if (!wait_until(descriptor, POLLIN, deadline))
            return false;
        if (available > 0) {
            struct timespec delay = { .tv_nsec = 1000000L };
            (void)nanosleep(&delay, NULL);
        }
    }
}

static bool trusted_backend_peer(const struct ucred *peer, uint32_t backend,
                                 ksp_identity *identity)
{
    char proc_path[64];
    struct stat authority_status;
    struct stat peer_status;
    int length = snprintf(proc_path, sizeof(proc_path), "/proc/%ld/exe",
                          (long)peer->pid);
    return peer->uid != 0u && peer->pid > 0
        && backend >= KSD_BACKEND_KWIN
        && backend <= KSD_BACKEND_X11
        && length > 0 && (size_t)length < sizeof(proc_path)
        && stat("/proc/self/exe", &authority_status) == 0
        && stat(proc_path, &peer_status) == 0
        && S_ISREG(authority_status.st_mode)
        && ksd_install_owner_trusted(authority_status.st_uid)
        && (authority_status.st_mode & (S_IWGRP | S_IWOTH)) == 0
        && authority_status.st_dev == peer_status.st_dev
        && authority_status.st_ino == peer_status.st_ino
        && ksp_identity_capture(peer->pid, peer->uid, identity) == 0
        && (backend == KSD_BACKEND_GENERIC
            || ksd_backend_resolve_process(peer->pid) == backend);
}

static bool pipe_read_until(int descriptor, void *data, size_t length,
                            uint64_t deadline)
{
    uint8_t *bytes = data;
    size_t offset = 0u;

    while (offset < length) {
        if (!wait_until(descriptor, POLLIN, deadline))
            return false;
        ssize_t count = read(descriptor, bytes + offset, length - offset);
        if (count < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (count <= 0)
            return false;
        offset += (size_t)count;
    }
    return true;
}

/* Executes the bus query with the user's credentials. The session bus closes
 * an authority connection authenticated as root, so the authority cannot
 * perform this lookup in-process. Only async-signal-safe setup occurs between
 * fork and fexecve. */
static pid_t query_provider_pid(uid_t uid, gid_t gid, uint32_t backend,
                                uint64_t deadline)
{
    int executable = -1;
    int child_executable = -1;
    int pipe_descriptors[2] = { -1, -1 };
    int child_output = -1;
    pid_t child = -1;
    pid_t result = -1;
    char backend_text[16];
    char bus_address[160];
    char runtime_directory[96];
    uint8_t encoded[8];
    int status = 0;
    bool already = uid == getuid() && uid == geteuid()
        && gid == getgid() && gid == getegid();

    if ((backend != KSD_BACKEND_GNOME
         && backend != KSD_BACKEND_CINNAMON)
        || uid == 0u
        || snprintf(backend_text, sizeof(backend_text), "%u", backend) <= 0
        || snprintf(bus_address, sizeof(bus_address),
                    "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/%lu/bus",
                    (unsigned long)uid) <= 0
        || snprintf(runtime_directory, sizeof(runtime_directory),
                    "XDG_RUNTIME_DIR=/run/user/%lu", (unsigned long)uid) <= 0)
        goto done;
    executable = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (executable < 0 || pipe2(pipe_descriptors, O_CLOEXEC) != 0)
        goto done;
    child_executable = fcntl(executable, F_DUPFD_CLOEXEC, 10);
    child_output = fcntl(pipe_descriptors[1], F_DUPFD_CLOEXEC, 10);
    if (child_executable < 0 || child_output < 0)
        goto done;
    child = fork();
    if (child == 0) {
        if (dup2(child_output, 3) < 0 || dup2(child_executable, 4) < 0
            || (!already && setgroups(0u, NULL) != 0)
            || (!already && setresgid(gid, gid, gid) != 0)
            || (!already && setresuid(uid, uid, uid) != 0))
            _exit(1);
        char *arguments[] = {
            (char *)"keysharp-desktop", (char *)"session-query",
            backend_text, NULL,
        };
        char *environment[] = {
            bus_address, runtime_directory, (char *)"GIO_USE_VFS=local", NULL,
        };
        close_range(5u, UINT_MAX, 0u);
        fexecve(4, arguments, environment);
        _exit(1);
    }
    if (child < 0)
        goto done;
    close(pipe_descriptors[1]);
    pipe_descriptors[1] = -1;
    close(child_output);
    child_output = -1;
    close(child_executable);
    child_executable = -1;
    bool received = pipe_read_until(pipe_descriptors[0], encoded,
                                    sizeof(encoded), deadline);
    pid_t waited = -1;
    if (received) {
        do {
            waited = waitpid(child, &status, 0);
        } while (waited < 0 && errno == EINTR);
    }
    if (received && waited == child && WIFEXITED(status)
        && WEXITSTATUS(status) == 0) {
        uint64_t value = ksd_decode_u64(encoded);
        if (value > 0u && value <= (uint64_t)INT_MAX)
            result = (pid_t)value;
    }
    if (waited == child)
        child = -1;

done:
    if (child > 0) {
        (void)kill(child, SIGKILL);
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
        }
    }
    if (child_output >= 0)
        close(child_output);
    if (child_executable >= 0)
        close(child_executable);
    if (pipe_descriptors[0] >= 0)
        close(pipe_descriptors[0]);
    if (pipe_descriptors[1] >= 0)
        close(pipe_descriptors[1]);
    if (executable >= 0)
        close(executable);
    return result;
}

static uint64_t registered_operations(authority_state *state, uid_t uid,
                                      uint32_t backend)
{
    uint64_t advertised = 0u;
    bool found = false;
    pthread_mutex_lock(&state->mutex);
    for (size_t index = 0u; index < KSD_MAX_BACKEND_REGISTRATIONS; index++)
        if (state->backends[index].active
            && state->backends[index].uid == uid) {
            advertised = state->backends[index].advertised;
            found = true;
            break;
        }
    pthread_mutex_unlock(&state->mutex);
    return ksd_backend_reported_operations(backend, found, advertised);
}

static bool register_backend(authority_state *state, int descriptor,
                             const struct ucred *peer, uint32_t backend,
                             const ksp_identity *identity,
                             const ksp_identity *provider_identity,
                             uint64_t advertised, int provider_fd)
{
    size_t free_slot = KSD_MAX_BACKEND_REGISTRATIONS;
    bool registered = false;
    pthread_mutex_lock(&state->mutex);
    for (size_t index = 0u; index < KSD_MAX_BACKEND_REGISTRATIONS; index++) {
        if (state->backends[index].active
            && state->backends[index].uid == peer->uid)
            goto done;
        if (!state->backends[index].active
            && free_slot == KSD_MAX_BACKEND_REGISTRATIONS)
            free_slot = index;
    }
    if (free_slot < KSD_MAX_BACKEND_REGISTRATIONS) {
        state->backends[free_slot].active = true;
        state->backends[free_slot].uid = peer->uid;
        state->backends[free_slot].descriptor = descriptor;
        state->backends[free_slot].backend = backend;
        state->backends[free_slot].advertised = advertised;
        /* Owned by the slot from here, and closed when the registration ends.
         * The daemon has already dropped its copy, so this is the only
         * reference and a leak here would hold the socket open past the
         * registration it belongs to. */
        state->backends[free_slot].provider_fd = provider_fd;
        state->backends[free_slot].relay = provider_fd >= 0
            ? ksd_kwin_relay_create(provider_fd) : NULL;
        state->backends[free_slot].identity = *identity;
        if (provider_identity != NULL)
            state->backends[free_slot].provider_identity = *provider_identity;
        registered = true;
    }
done:
    pthread_mutex_unlock(&state->mutex);
    return registered;
}

static void unregister_backend(authority_state *state, uid_t uid,
                               int descriptor)
{
    pthread_mutex_lock(&state->mutex);
    for (size_t index = 0u; index < KSD_MAX_BACKEND_REGISTRATIONS; index++)
        if (state->backends[index].active
            && state->backends[index].uid == uid
            && state->backends[index].descriptor == descriptor) {
            /* Closed with the registration it belongs to. The daemon dropped
             * its copy when it handed this over, so this is the only reference
             * and leaking it would hold the socket open for the life of the
             * authority -- and leave the daemon's end never seeing a hangup. */
            /* Retired, not destroyed: a caller may be inside a call on it
             * right now, holding its own reference. Retiring wakes them onto a
             * closed channel; the memory goes when the last one leaves.
             *
             * The relay owns the provider descriptor once it exists, so only
             * one of those two closes it. The display worker is a separate
             * socket to a separate process and is released on its own: chained
             * onto the provider branch, as it once was, a session holding both
             * would never close the provider. */
            if (state->backends[index].relay != NULL)
                ksd_kwin_relay_retire(state->backends[index].relay);
            else if (state->backends[index].provider_fd >= 0)
                close(state->backends[index].provider_fd);
            /* Retiring closes the socket, which is what tells the worker to
             * exit: it serves until end-of-file and then goes. */
            if (state->backends[index].display_relay != NULL)
                ksd_kwin_relay_retire(state->backends[index].display_relay);
            memset(&state->backends[index], 0,
                   sizeof(state->backends[index]));
            state->backends[index].descriptor = -1;
            state->backends[index].provider_fd = -1;
            break;
        }
    pthread_mutex_unlock(&state->mutex);
}

/* A registration is only honoured while the process still looks like what it
 * registered as. GENERIC is the one exemption, because it means precisely "no
 * compositor this service knows"; X11 is NOT exempt, it is checked against the
 * session type, or a Wayland session could register as X11 and keep it. */
static bool backend_matches_session(uint32_t backend, pid_t pid)
{
    if (backend == KSD_BACKEND_GENERIC)
        return true;
    if (backend == KSD_BACKEND_X11)
        return ksd_session_is_x11_process(pid);
    return ksd_backend_resolve_process(pid) == backend;
}

#ifdef KSD_AUTHORITY_TESTING
bool ksd_authority_test_backend_matches(uint32_t backend, pid_t pid)
{
    return backend_matches_session(backend, pid);
}
#endif

static pid_t registered_backend_pid(authority_state *state, uid_t uid)
{
    pid_t pid = -1;
    pthread_mutex_lock(&state->mutex);
    for (size_t index = 0u; index < KSD_MAX_BACKEND_REGISTRATIONS; index++)
        if (state->backends[index].active
            && state->backends[index].uid == uid) {
            pid = state->backends[index].identity.pid;
            break;
        }
    pthread_mutex_unlock(&state->mutex);
    return pid;
}

static pid_t registered_provider_pid(authority_state *state, uid_t uid,
                                     uint32_t backend)
{
    ksp_identity expected = { 0 };
    int descriptor = -1;

    pthread_mutex_lock(&state->mutex);
    for (size_t index = 0u; index < KSD_MAX_BACKEND_REGISTRATIONS; index++)
        if (state->backends[index].active
            && state->backends[index].uid == uid
            && state->backends[index].backend == backend) {
            expected = state->backends[index].provider_identity;
            descriptor = state->backends[index].descriptor;
            break;
        }
    pthread_mutex_unlock(&state->mutex);
    if (descriptor < 0 || expected.pid <= 0)
        return -1;

    ksp_identity verified;
    if (ksp_identity_revalidate(&expected, &verified) != 0
        || !same_identity(&expected, &verified))
        return -1;

    pid_t result = -1;
    pthread_mutex_lock(&state->mutex);
    for (size_t index = 0u; index < KSD_MAX_BACKEND_REGISTRATIONS; index++)
        if (state->backends[index].active
            && state->backends[index].uid == uid
            && state->backends[index].descriptor == descriptor
            && state->backends[index].backend == backend
            && same_identity(&state->backends[index].provider_identity,
                             &expected)) {
            result = expected.pid;
            break;
        }
    pthread_mutex_unlock(&state->mutex);
    return result;
}

/* The relay for a uid's registered daemon, or NULL. Returned under the lock
 * and used after it: the relay outlives the lock because it is destroyed only
 * by unregister_backend, which the caller's own registration check guards
 * against for the length of one operation. */
static ksd_kwin_relay *registered_relay(authority_state *state, uid_t uid)
{
    ksd_kwin_relay *relay = NULL;

    pthread_mutex_lock(&state->mutex);
    for (size_t index = 0u; index < KSD_MAX_BACKEND_REGISTRATIONS; index++)
        if (state->backends[index].active
            && state->backends[index].uid == uid) {
            relay = state->backends[index].relay;
            /* Referenced while the slot is still active and the lock still
             * held, so it cannot be freed between here and the caller's use
             * of it -- which lasts as long as an operation deadline. */
            ksd_kwin_relay_acquire(relay);
            break;
        }
    pthread_mutex_unlock(&state->mutex);
    return relay;
}

/* The session's display worker, started if this is the first request that
 * needs it. Returns a referenced relay, or NULL to fall back to a one-shot
 * worker -- which is the honest answer when the credentials do not match the
 * worker already running, since a worker holds one set. */
/* Takes a worker that has stopped answering out of its slot, so the next
 * request starts a fresh one.
 *
 * Nothing else clears the field: without this a worker that died -- the
 * compositor restarted, the process was killed -- stays registered for the
 * life of the registration, and every later request on that session pays a
 * failed call before falling back to a one-shot worker. The fallback keeps the
 * answers correct, which is exactly why the leak would go unnoticed.
 */
static void forget_display_relay(authority_state *state,
                                 const ksp_identity *identity,
                                 ksd_kwin_relay *relay)
{
    bool taken = false;

    pthread_mutex_lock(&state->mutex);
    for (size_t index = 0u; index < KSD_MAX_BACKEND_REGISTRATIONS; index++) {
        if (!state->backends[index].active
            || state->backends[index].uid != identity->uid
            || state->backends[index].display_relay != relay)
            continue;
        state->backends[index].display_relay = NULL;
        taken = true;
        break;
    }
    pthread_mutex_unlock(&state->mutex);
    /* Only the thread that took it out of the slot retires it. Several callers
     * can see the same worker fail at the same moment, and the slot's single
     * reference has to be dropped once, not once per witness. */
    if (taken)
        ksd_kwin_relay_retire(relay);
}

static ksd_kwin_relay *display_relay_for(authority_state *state,
                                         const ksp_identity *identity,
                                         gid_t gid, pid_t session_pid,
                                         uint32_t backend)
{
    ksd_kwin_relay *relay = NULL;
    int socket_fd = -1;
    size_t slot = KSD_MAX_BACKEND_REGISTRATIONS;

    pthread_mutex_lock(&state->mutex);
    for (size_t index = 0u; index < KSD_MAX_BACKEND_REGISTRATIONS; index++) {
        if (!state->backends[index].active
            || state->backends[index].uid != identity->uid)
            continue;
        slot = index;
        if (state->backends[index].display_relay != NULL) {
            /* Only reusable by a client with the credentials the worker
             * already dropped to. */
            if (state->backends[index].display_uid == identity->uid
                && state->backends[index].display_gid == gid) {
                relay = state->backends[index].display_relay;
                ksd_kwin_relay_acquire(relay);
            }
            pthread_mutex_unlock(&state->mutex);
            return relay;
        }
        break;
    }
    pthread_mutex_unlock(&state->mutex);
    if (slot == KSD_MAX_BACKEND_REGISTRATIONS)
        return NULL;

    /* Started outside the lock: a fork and an exec are far too long to hold a
     * mutex every other thread needs. Two threads racing here both spawn, and
     * the loser's worker is closed below rather than leaked. */
    socket_fd = ksd_capture_worker_spawn(identity, gid, session_pid, backend);
    if (socket_fd < 0)
        return NULL;
    relay = ksd_kwin_relay_create(socket_fd);
    if (relay == NULL) {
        close(socket_fd);
        return NULL;
    }

    pthread_mutex_lock(&state->mutex);
    if (!state->backends[slot].active
        || state->backends[slot].uid != identity->uid) {
        /* The registration went away while the worker was starting. */
        pthread_mutex_unlock(&state->mutex);
        ksd_kwin_relay_retire(relay);
        return NULL;
    }
    if (state->backends[slot].display_relay != NULL) {
        ksd_kwin_relay *winner = state->backends[slot].display_relay;

        ksd_kwin_relay_acquire(winner);
        pthread_mutex_unlock(&state->mutex);
        ksd_kwin_relay_retire(relay);
        return winner;
    }
    state->backends[slot].display_relay = relay;
    state->backends[slot].display_uid = identity->uid;
    state->backends[slot].display_gid = gid;
    ksd_kwin_relay_acquire(relay);
    pthread_mutex_unlock(&state->mutex);
    return relay;
}

static uint32_t registered_backend(authority_state *state, uid_t uid)
{
    ksp_identity expected = { 0 };
    int descriptor = -1;
    uint32_t backend = KSD_BACKEND_NONE;
    pthread_mutex_lock(&state->mutex);
    for (size_t index = 0u; index < KSD_MAX_BACKEND_REGISTRATIONS; index++)
        if (state->backends[index].active
            && state->backends[index].uid == uid) {
            expected = state->backends[index].identity;
            descriptor = state->backends[index].descriptor;
            backend = state->backends[index].backend;
            break;
        }
    pthread_mutex_unlock(&state->mutex);
    if (descriptor < 0 || backend == KSD_BACKEND_NONE)
        return KSD_BACKEND_NONE;
    ksp_identity verified;
    if (ksp_identity_revalidate(&expected, &verified) != 0
        || !same_identity(&expected, &verified)
        || !backend_matches_session(backend, expected.pid))
        return KSD_BACKEND_NONE;
    pthread_mutex_lock(&state->mutex);
    uint32_t result = KSD_BACKEND_NONE;
    for (size_t index = 0u; index < KSD_MAX_BACKEND_REGISTRATIONS; index++)
        if (state->backends[index].active
            && state->backends[index].uid == uid
            && state->backends[index].descriptor == descriptor
            && state->backends[index].backend == backend
            && same_identity(&state->backends[index].identity, &expected)) {
            result = backend;
            break;
        }
    pthread_mutex_unlock(&state->mutex);
    return result;
}

static bool send_backend_ack(int descriptor, uint16_t status,
                             uint32_t backend, uint64_t accepted,
                             uint64_t deadline)
{
    uint8_t reply[KSD_BACKEND_REGISTRATION_SIZE] = { 0 };
    ksd_backend_ack_encode(reply, status, backend, accepted);
    return fixed_io_until(descriptor, reply, sizeof(reply), true, deadline);
}

/* Six checks on a descriptor a daemon handed over. Each rules out a different
 * way of being handed something other than what was promised, and none of them
 * is paranoia about a hostile daemon so much as about a confused one: a
 * registration that succeeds with the wrong descriptor fails later, somewhere
 * else, as a channel that never answers.
 *
 * It must be a socket, a connected UNIX stream, not a listener, and its peer
 * must be the process that sent it. A listening socket is the interesting one:
 * accepting on it would let anything on the system connect and be taken for
 * the session daemon. */
static bool provider_fd_valid(int provider_fd, const struct ucred *peer)
{
    struct stat info;
    struct ucred credentials;
    socklen_t size;
    int value;

    if (provider_fd < 0 || peer == NULL)
        return false;
    if (fstat(provider_fd, &info) != 0 || !S_ISSOCK(info.st_mode))
        return false;

    size = sizeof(value);
    if (getsockopt(provider_fd, SOL_SOCKET, SO_DOMAIN, &value, &size) != 0
        || value != AF_UNIX)
        return false;
    size = sizeof(value);
    if (getsockopt(provider_fd, SOL_SOCKET, SO_TYPE, &value, &size) != 0
        || value != SOCK_STREAM)
        return false;
    size = sizeof(value);
    if (getsockopt(provider_fd, SOL_SOCKET, SO_ACCEPTCONN, &value, &size) != 0
        || value != 0)
        return false;

    size = sizeof(credentials);
    if (getsockopt(provider_fd, SOL_SOCKET, SO_PEERCRED, &credentials,
                   &size) != 0)
        return false;
    /* The far end must be the daemon that sent it. A socketpair the daemon
     * made has itself on both ends; anything else is a socket it obtained
     * elsewhere, and the authority would then be calling back a process it
     * never authenticated. */
    return credentials.pid == peer->pid && credentials.uid == peer->uid;
}

static void handle_backend_connection(authority_state *state, int descriptor,
                                      const struct ucred *peer)
{
    uint8_t registration[KSD_BACKEND_REGISTRATION_SIZE] = { 0 };
    ksp_identity identity;
    ksp_identity provider_identity = { 0 };
    uint32_t backend = KSD_BACKEND_NONE;
    pid_t provider_pid = -1;
    uint64_t now = monotonic_milliseconds();
    if (now == 0u)
        return;
    uint64_t deadline = now + KSD_BACKEND_REGISTRATION_TIMEOUT_MS;
    int provider_fd = -1;
    /* Received with the record rather than after it, so there is never a
     * moment when a backend is registered and its callback socket is not. */
    bool valid = ksd_receive_fd_until(descriptor, registration,
                                      sizeof(registration), deadline,
                                      &provider_fd) == 0
        && memcmp(registration, ksd_backend_registration_magic,
                  sizeof(ksd_backend_registration_magic)) == 0
        && ksd_decode_u16(registration + 4u)
            == KSD_BACKEND_REGISTRATION_VERSION
        && ksd_decode_u32(registration + 12u) == 0u
        && ksd_decode_u64(registration + 24u) == 0u;
    const char *rejection = valid ? NULL : "malformed registration";
    uint64_t advertised = 0u;
    if (valid) {
        backend = ksd_decode_u32(registration + 8u);
        if (!ksd_backend_registration_mask(backend,
                ksd_decode_u16(registration + 4u),
                ksd_decode_u16(registration + 6u),
                ksd_decode_u64(registration + 16u), &advertised)) {
            valid = false;
            rejection = "invalid backend fields";
        }
    }
    /* The flag and the descriptor must agree in both directions. A daemon that
     * set the flag and sent nothing would leave the authority believing it can
     * call back; one that sent a descriptor without the flag is sending
     * something this service did not ask for and will not use. */
    bool wants_fd = valid
        && (ksd_decode_u16(registration + 6u) & KSD_BACKEND_FLAG_PROVIDER_FD)
            != 0u;
    if (valid && wants_fd != (provider_fd >= 0)) {
        valid = false;
        rejection = "provider descriptor mismatch";
    }
    if (valid && provider_fd >= 0 && !provider_fd_valid(provider_fd, peer)) {
        valid = false;
        rejection = "invalid provider descriptor";
    }
    if (valid && !trusted_backend_peer(peer, backend, &identity)) {
        valid = false;
        rejection = "untrusted session daemon";
    }
    if (valid && (backend == KSD_BACKEND_GNOME
                  || backend == KSD_BACKEND_CINNAMON)) {
        provider_pid = query_provider_pid(peer->uid, peer->gid, backend,
                                          deadline);
        if (provider_pid <= 0
            || ksp_identity_capture(provider_pid, peer->uid,
                                    &provider_identity) != 0) {
            valid = false;
            rejection = "untrusted provider process";
        }
    }
    if (valid && !register_backend(state, descriptor, peer, backend, &identity,
                                   provider_pid > 0 ? &provider_identity : NULL,
                                   advertised, provider_fd)) {
        valid = false;
        rejection = "session already registered";
    }
    if (!valid) {
        fprintf(stderr, "keysharp-desktop authority-daemon: rejected backend"
                " registration: %s\n", rejection);
        if (provider_fd >= 0)
            close(provider_fd);
        (void)send_backend_ack(descriptor, KSD_BACKEND_ACK_REJECTED,
                               KSD_BACKEND_NONE, 0u, deadline);
        return;
    }
    /* The stored mask, not the requested one: the daemon is told what it is
     * actually serving, which is the only way it can notice that the authority
     * narrowed it. */
    if (!send_backend_ack(descriptor, KSD_BACKEND_ACK_ACCEPTED,
                          backend, advertised, deadline)) {
        unregister_backend(state, peer->uid, descriptor);
        return;
    }
    for (;;) {
        struct pollfd item = {
            .fd = descriptor,
            .events = POLLIN | POLLRDHUP | POLLHUP | POLLERR,
        };
        int ready = poll(&item, 1u, backend == KSD_BACKEND_GENERIC
            ? KSD_GENERIC_REGISTRATION_POLL_MS
            : KSD_BACKEND_REGISTRATION_POLL_MS);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0 || registered_backend(state, peer->uid) != backend
            || (ready > 0 && (item.revents
                & (POLLIN | POLLRDHUP | POLLHUP | POLLERR | POLLNVAL)) != 0))
            break;
    }
    unregister_backend(state, peer->uid, descriptor);
}

static bool build_response(const ksd_frame *request, uint32_t status,
                           uint32_t detail, const char *diagnostic,
                           const void *tail, uint32_t tail_length,
                           bool more, ksd_frame *response)
{
    ksd_buffer payload;
    size_t diagnostic_length = diagnostic == NULL ? 0u : strlen(diagnostic);
    bool ok;

    if (request == NULL || response == NULL || request->request_id == 0u
        || diagnostic_length > KSD_MAX_TEXT_BYTES
        || (status == KSD_STATUS_OK && diagnostic_length != 0u)
        || (tail_length != 0u && tail == NULL))
        return false;
    memset(response, 0, sizeof(*response));
    memcpy(response->magic, public_magic, sizeof(response->magic));
    response->major = KSD_PROTOCOL_MAJOR;
    response->minor = KSD_PROTOCOL_MINOR;
    response->opcode = request->opcode;
    response->flags =
        (uint16_t)(KSD_FLAG_RESPONSE | (more ? KSD_FLAG_MORE : 0u));
    response->request_id = request->request_id;
    ksd_buffer_init(&payload, KSD_MAX_CAPTURE_BYTES + 64u);
    ok = ksd_buffer_u32(&payload, status)
        && ksd_buffer_u32(&payload, detail);
    if (ok && status != KSD_STATUS_OK && diagnostic != NULL)
        ok = ksd_buffer_u32(&payload, (uint32_t)diagnostic_length)
            && ksd_buffer_bytes(&payload, diagnostic, diagnostic_length);
    if (ok && status == KSD_STATUS_OK)
        ok = ksd_buffer_bytes(&payload, tail, tail_length);
    if (!ok || payload.length > UINT32_MAX) {
        ksd_buffer_clear(&payload);
        return false;
    }
    response->payload = payload.data;
    response->payload_length = (uint32_t)payload.length;
    return true;
}

static bool forward_public(authority_session *session,
                           const ksd_frame *public_frame)
{
    return session != NULL && public_frame != NULL
        && ksd_frame_write(session->client_fd, public_frame);
}

/* A capture answers with a sealed memfd instead of a payload, so the pixels
 * are never written through the socket and never copied into a response. */
static bool forward_capture(authority_session *session,
                            const ksd_frame *request,
                            const ksd_operation_result *result)
{
    ksd_frame response;
    if (!build_response(request, result->status, result->detail, NULL,
                        NULL, 0u, false, &response))
        return false;
    bool ok = ksd_frame_write_fd(session->client_fd, &response,
                                 result->payload_fd);
    ksd_frame_clear(&response);
    return ok;
}

static bool forward_response(authority_session *session,
                             const ksd_frame *request,
                             uint32_t status, uint32_t detail,
                             const char *diagnostic,
                             const void *tail, uint32_t tail_length,
                             bool more)
{
    ksd_frame response;
    if (!build_response(request, status, detail, diagnostic,
                        tail, tail_length, more, &response))
        return false;
    bool ok = forward_public(session, &response);
    ksd_frame_clear(&response);
    return ok;
}

static bool forward_event(authority_session *session, uint16_t opcode,
                          const void *payload, uint32_t payload_length)
{
    ksd_frame event = {
        .magic = {
            KSD_FRAME_MAGIC_0, KSD_FRAME_MAGIC_1,
            KSD_FRAME_MAGIC_2, KSD_FRAME_MAGIC_3,
        },
        .major = KSD_PROTOCOL_MAJOR,
        .minor = KSD_PROTOCOL_MINOR,
        .opcode = opcode,
        .flags = KSD_FLAG_EVENT,
        .payload_length = payload_length,
        .request_id = 0u,
        .payload = (uint8_t *)payload,
    };
    return forward_public(session, &event);
}

static bool send_revoked(authority_session *session, uint32_t scopes)
{
    uint8_t payload[8] = { 0 };
    ksd_encode_u32(payload, scopes);
    return scopes == 0u
        || forward_event(session, KSD_OP_SESSION_REVOKED,
                         payload, sizeof(payload));
}

static bool session_identity_refresh(authority_session *session)
{
    ksp_identity verified;
    if (ksp_identity_revalidate(&session->identity, &verified) != 0
        || !same_identity(&session->identity, &verified))
        return false;
    session->identity = verified;
    session->identity_checked_at = monotonic_seconds();
    return true;
}

static bool session_refresh(authority_session *session, bool force_identity,
                            bool notify, bool *generation_changed)
{
    uint64_t generation;
    uint32_t allowed = 0u;
    uint32_t revoked;
    bool changed;

    if (generation_changed != NULL)
        *generation_changed = false;
    if (session->requested_scopes == 0u)
        return !force_identity || session_identity_refresh(session);
    if (ksp_store_generation(session->state->store, session->identity.uid,
                             &generation) != 0)
        goto failed;
    if (force_identity || generation != session->generation) {
        if (!session_identity_refresh(session))
            goto failed;
    }
    if (session->requested_scopes != 0u) {
        if (ksp_store_check_at_generation(session->state->store,
                session->identity.uid, session->identity.hash,
                session->requested_scopes, &allowed, &generation) != 0)
            goto failed;
    }
    revoked = session->granted_scopes & ~allowed;
    changed = generation != session->generation;
    session->generation = generation;
    session->granted_scopes = allowed;
    if (generation_changed != NULL)
        *generation_changed = changed;
    return !notify || send_revoked(session, revoked);

failed:
    revoked = session->granted_scopes;
    session->granted_scopes = 0u;
    if (notify)
        (void)send_revoked(session, revoked);
    return false;
}

static bool descriptor_disconnected(int descriptor)
{
    if (descriptor < 0)
        return false;
    struct pollfd item = {
        .fd = descriptor,
        .events = POLLIN | POLLRDHUP | POLLHUP | POLLERR,
    };
    int ready = poll(&item, 1u, 0);
    if (ready < 0)
        return errno != EINTR;
    if (ready == 0)
        return false;
    if ((item.revents & (POLLRDHUP | POLLHUP | POLLERR | POLLNVAL)) != 0)
        return true;
    if ((item.revents & POLLIN) == 0)
        return false;
    uint8_t byte;
    ssize_t count = recv(descriptor, &byte, sizeof(byte),
                         MSG_PEEK | MSG_DONTWAIT);
    return count == 0 || (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK
                          && errno != EINTR);
}

static bool authority_disconnected(void *user_data)
{
    authority_session *session = user_data;
    return descriptor_disconnected(session->client_fd);
}

static bool capture_still_authorized(void *user_data)
{
    authority_session *session = user_data;
    return !descriptor_disconnected(session->client_fd)
        && session_refresh(session, true, true, NULL)
        && (session->granted_scopes & KSP_SCOPE_SCREEN_CAPTURE)
            == KSP_SCOPE_SCREEN_CAPTURE;
}

static uint32_t authorize_scopes(authority_session *session,
                                 uint32_t requested, uint16_t auth_mode,
                                 uint32_t *granted)
{
    uint32_t allowed = 0u;
    uint64_t generation;
    int prompt_lock = -1;
    ksp_identity verified;
    uint32_t status = KSD_STATUS_INTERNAL;

    *granted = session->granted_scopes;
    if (requested == 0u
        || (requested & ~(uint32_t)KSD_DESKTOP_ACCEPTED_SCOPES) != 0u
        || (auth_mode != KSD_AUTH_CHECK && auth_mode != KSD_AUTH_REQUEST))
        return KSD_STATUS_INVALID_REQUEST;
    if (ksp_store_check_at_generation(session->state->store,
            session->identity.uid, session->identity.hash, requested,
            &allowed, &generation) != 0)
        return KSD_STATUS_INTERNAL;
    session->requested_scopes |= requested;
    session->generation = generation;
    session->granted_scopes |= allowed;
    if ((allowed & requested) == requested) {
        *granted = session->granted_scopes;
        return KSD_STATUS_OK;
    }
    if (auth_mode == KSD_AUTH_CHECK) {
        *granted = session->granted_scopes;
        return KSD_STATUS_DENIED;
    }

    prompt_lock = ksp_prompt_lock_acquire(session->state->store,
        session->identity.uid, session->identity.hash,
        authority_disconnected, session);
    if (prompt_lock < 0)
        return authority_disconnected(session)
            ? KSD_STATUS_CANCELLED : KSD_STATUS_INTERNAL;
    if (ksp_identity_revalidate(&session->identity, &verified) != 0
        || !same_identity(&session->identity, &verified)) {
        status = KSD_STATUS_CANCELLED;
        goto done;
    }
    session->identity = verified;
    if (ksp_store_check_at_generation(session->state->store,
            session->identity.uid, session->identity.hash, requested,
            &allowed, &generation) != 0)
        goto done;
    session->generation = generation;
    session->granted_scopes |= allowed;
    uint32_t missing = requested & ~allowed;
    if (missing == 0u) {
        status = KSD_STATUS_OK;
        goto done;
    }
    const ksp_polkit_config polkit = {
        .pkcheck_path = KSD_PKCHECK_PATH,
        .action_id = KSD_POLKIT_ACTION,
        .scope_detail_key = "desktop.scopes",
        .scope_names_detail_key = "desktop.scope-names",
        .allowed_scopes = KSD_DESKTOP_MANAGED_SCOPES,
        .timeout_seconds = 120u,
    };
    ksp_polkit_result decision = ksp_polkit_authorize(&polkit,
        &session->identity, missing, authority_disconnected, session);
    if (decision != KSP_POLKIT_GRANTED) {
        status = decision == KSP_POLKIT_DENIED ? KSD_STATUS_DENIED
            : decision == KSP_POLKIT_CANCELLED
                || decision == KSP_POLKIT_IDENTITY_CHANGED
                ? KSD_STATUS_CANCELLED : KSD_STATUS_UNAVAILABLE;
        goto done;
    }
    if (ksp_identity_revalidate(&session->identity, &verified) != 0
        || !same_identity(&session->identity, &verified)) {
        status = KSD_STATUS_CANCELLED;
        goto done;
    }
    int added = ksp_store_grant_if_generation(session->state->store,
        &verified, missing, generation);
    if (added != 0) {
        status = added > 0 ? KSD_STATUS_REVOKED : KSD_STATUS_INTERNAL;
        goto done;
    }
    session->identity = verified;
    if (ksp_store_check_at_generation(session->state->store,
            session->identity.uid, session->identity.hash, requested,
            &allowed, &session->generation) != 0)
        goto done;
    session->granted_scopes |= allowed;
    status = (allowed & requested) == requested
        ? KSD_STATUS_OK : KSD_STATUS_REVOKED;

done:
    ksp_prompt_lock_release(prompt_lock);
    *granted = session->granted_scopes;
    return status;
}

static void hash_to_raw(const char *hash, uint8_t raw[32])
{
    for (size_t index = 0u; index < 32u; index++) {
        char high = hash[index * 2u];
        char low = hash[index * 2u + 1u];
        unsigned first = high <= '9' ? (unsigned)(high - '0')
            : (unsigned)(high - 'a' + 10);
        unsigned second = low <= '9' ? (unsigned)(low - '0')
            : (unsigned)(low - 'a' + 10);
        raw[index] = (uint8_t)((first << 4u) | second);
    }
}

static void raw_to_hash(const uint8_t raw[32], char hash[65])
{
    static const char digits[] = "0123456789abcdef";
    for (size_t index = 0u; index < 32u; index++) {
        hash[index * 2u] = digits[raw[index] >> 4u];
        hash[index * 2u + 1u] = digits[raw[index] & 0x0fu];
    }
    hash[64] = '\0';
}

static size_t safe_path(const char *source, char destination[KSP_PATH_CAPACITY])
{
    ksp_sanitize_display_text(source, destination, KSP_PATH_CAPACITY);
    size_t length = strlen(destination);
    if (!ksd_utf8_valid((const uint8_t *)destination, length, false)) {
        for (size_t index = 0u; index < length; index++)
            if ((uint8_t)destination[index] >= 0x80u)
                destination[index] = '?';
    }
    return length;
}

static bool permissions_list(authority_session *session,
                             const ksd_frame *request)
{
    ksp_permission_entry *entries = NULL;
    size_t count = 0u;
    if (request->payload_length != 0u)
        return forward_response(session, request, KSD_STATUS_INVALID_REQUEST,
                                0u, "permissions list payload must be empty",
                                NULL, 0u, false);
    if (ksp_store_list(session->state->store, session->identity.uid,
                       &entries, &count) != 0)
        return forward_response(session, request, KSD_STATUS_INTERNAL, 0u,
                                "could not list permissions",
                                NULL, 0u, false);
    bool ok = true;
    for (size_t index = 0u; ok && index < count; index++) {
        uint32_t scopes = entries[index].scopes
            & KSD_DESKTOP_MANAGED_SCOPES;
        if (scopes == 0u)
            continue;
        char path[KSP_PATH_CAPACITY];
        size_t path_length = safe_path(entries[index].executable, path);
        uint8_t hash[32];
        ksd_buffer tail;
        hash_to_raw(entries[index].app_hash, hash);
        ksd_buffer_init(&tail, KSP_PATH_CAPACITY + 48u);
        ok = path_length <= UINT32_MAX
            && ksd_buffer_u32(&tail, scopes)
            && ksd_buffer_u32(&tail, (uint32_t)path_length)
            && ksd_buffer_u64(&tail, entries[index].granted_at_utc)
            && ksd_buffer_bytes(&tail, hash, sizeof(hash))
            && ksd_buffer_bytes(&tail, path, path_length)
            && forward_response(session, request, KSD_STATUS_OK, 0u,
                                NULL, tail.data, (uint32_t)tail.length, true);
        ksd_buffer_clear(&tail);
    }
    ksp_store_list_free(entries);
    return ok && forward_response(session, request, KSD_STATUS_OK, 0u,
                                  NULL, NULL, 0u, false);
}

static bool bytes_are_zero(const uint8_t *bytes, size_t length)
{
    for (size_t index = 0u; index < length; index++)
        if (bytes[index] != 0u)
            return false;
    return true;
}

static bool permissions_revoke(authority_session *session,
                               const ksd_frame *request)
{
    ksd_cursor cursor;
    uint32_t kind;
    uint32_t scopes;
    uint64_t pid;
    const uint8_t *raw_hash;
    int result = -1;

    ksd_cursor_init(&cursor, request->payload, request->payload_length);
    if (request->payload_length != 48u
        || !ksd_cursor_u32(&cursor, &kind)
        || !ksd_cursor_u32(&cursor, &scopes)
        || !ksd_cursor_u64(&cursor, &pid)
        || !ksd_cursor_bytes(&cursor, 32u, &raw_hash)
        || !ksd_cursor_finished(&cursor)
        || scopes == 0u
        || (scopes & ~(uint32_t)KSD_DESKTOP_MANAGED_SCOPES) != 0u)
        return forward_response(session, request, KSD_STATUS_INVALID_REQUEST,
                                0u, "invalid permissions revoke payload",
                                NULL, 0u, false);
    if (kind == KSD_PERMISSION_TARGET_HASH) {
        if (pid != 0u)
            goto invalid;
        char hash[65];
        raw_to_hash(raw_hash, hash);
        result = ksp_store_revoke(session->state->store,
            session->identity.uid, hash, scopes);
    } else if (kind == KSD_PERMISSION_TARGET_PID) {
        if (!bytes_are_zero(raw_hash, 32u) || pid == 0u || pid > INT_MAX)
            goto invalid;
        ksp_identity target;
        if (ksp_identity_capture((pid_t)pid, session->identity.uid,
                                 &target) != 0)
            return forward_response(session, request, KSD_STATUS_NOT_FOUND,
                                    0u, "target process was not found",
                                    NULL, 0u, false);
        result = ksp_store_revoke(session->state->store,
            session->identity.uid, target.hash, scopes);
    } else if (kind == KSD_PERMISSION_TARGET_ALL) {
        if (pid != 0u || !bytes_are_zero(raw_hash, 32u))
            goto invalid;
        result = ksp_store_revoke_uid(session->state->store,
                                     session->identity.uid, scopes);
    } else {
        goto invalid;
    }
    if (result != 0)
        return forward_response(session, request, KSD_STATUS_INTERNAL, 0u,
                                "could not revoke permissions",
                                NULL, 0u, false);
    if (!session_refresh(session, true, true, NULL))
        return false;
    return forward_response(session, request, KSD_STATUS_OK, 0u,
                            NULL, NULL, 0u, false);

invalid:
    return forward_response(session, request, KSD_STATUS_INVALID_REQUEST,
                            0u, "invalid permissions revoke selector",
                            NULL, 0u, false);
}

static bool handle_authorize(authority_session *session,
                             const ksd_frame *request)
{
    ksd_cursor cursor;
    uint16_t mode;
    uint16_t reserved0;
    uint32_t scopes;
    uint64_t reserved1;
    uint32_t granted;
    uint8_t tail[8] = { 0 };

    ksd_cursor_init(&cursor, request->payload, request->payload_length);
    if (request->payload_length != 16u
        || !ksd_cursor_u16(&cursor, &mode)
        || !ksd_cursor_u16(&cursor, &reserved0)
        || !ksd_cursor_u32(&cursor, &scopes)
        || !ksd_cursor_u64(&cursor, &reserved1)
        || !ksd_cursor_finished(&cursor)
        || reserved0 != 0u || reserved1 != 0u)
        return forward_response(session, request, KSD_STATUS_INVALID_REQUEST,
                                0u, "invalid AUTHORIZE payload",
                                NULL, 0u, false);
    uint32_t status = authorize_scopes(session, scopes, mode, &granted);
    if (status != KSD_STATUS_OK)
        return forward_response(session, request, status, 0u,
            status == KSD_STATUS_DENIED ? "permission is not granted"
            : status == KSD_STATUS_CANCELLED ? "authorization was cancelled"
            : status == KSD_STATUS_REVOKED
                ? "permission changed during authorization"
                : status == KSD_STATUS_INVALID_REQUEST
                    ? "invalid authorization scope or mode"
                    : "authorization service unavailable",
            NULL, 0u, false);
    ksd_encode_u32(tail, granted);
    return forward_response(session, request, KSD_STATUS_OK, 0u,
                            NULL, tail, sizeof(tail), false);
}

typedef struct watch_context {
    authority_session *session;
    uint32_t scope;
    uint32_t backend;
} watch_context;

static bool watch_emit(uint16_t opcode, const void *payload,
                       uint32_t payload_length, void *user_data)
{
    watch_context *context = user_data;
    if (registered_backend(context->session->state,
                           context->session->identity.uid)
            != context->backend
        || !session_refresh(context->session, true, true, NULL)
        || (context->session->granted_scopes & context->scope)
            != context->scope)
        return false;
    return forward_event(context->session, opcode, payload, payload_length);
}

static bool watch_cancelled(void *user_data)
{
    watch_context *context = user_data;
    authority_session *session = context->session;
    struct pollfd descriptors[1] = {
        { .fd = session->client_fd,
          .events = POLLIN | POLLRDHUP | POLLHUP | POLLERR },
    };
    int ready = poll(descriptors, 1u, 0);
    if (ready < 0 && errno != EINTR)
        return true;
    if (ready > 0)
        for (nfds_t index = 0u; index < 1u; index++)
            if ((descriptors[index].revents
                 & (POLLIN | POLLRDHUP | POLLHUP | POLLERR | POLLNVAL)) != 0)
                return true;
    bool force_identity = monotonic_seconds() != session->identity_checked_at;
    if (!session_refresh(session, force_identity, true, NULL))
        return true;
    return registered_backend(session->state, session->identity.uid)
            != context->backend
        || (session->granted_scopes & context->scope) != context->scope;
}

static bool start_watch(authority_session *session,
                        const ksd_frame *request, uint32_t scope)
{
    if (session->role != KSD_ROLE_EVENT_STREAM
        || request->payload_length != 0u)
        return forward_response(session, request, KSD_STATUS_INVALID_REQUEST,
                                0u, "invalid event subscription",
                                NULL, 0u, false);
    if (!forward_response(session, request, KSD_STATUS_OK, 0u,
                          NULL, NULL, 0u, false))
        return false;
    watch_context context = {
        .session = session,
        .scope = scope,
        .backend = session->backend,
    };
    char diagnostic[KSD_DIAGNOSTIC_CAPACITY];
    pid_t provider_pid = registered_provider_pid(session->state,
        session->identity.uid, session->backend);
    (void)ksd_provider_watch(session->identity.uid, provider_pid,
        session->backend,
        request->opcode == KSD_OP_CLIPBOARD_WATCH,
        watch_emit, watch_cancelled, &context,
        diagnostic, sizeof(diagnostic));
    return false;
}

static bool reserve_kwin_slot(authority_state *state, pid_t pid)
{
    size_t slot = KSD_MAX_AUTHORITY_WORKERS;
    size_t inflight = 0u;
    bool reserved = false;
    pthread_mutex_lock(&state->mutex);
    for (size_t index = 0u; index < KSD_MAX_AUTHORITY_WORKERS; index++) {
        if (state->kwin_inflight[index].count != 0u
            && state->kwin_inflight[index].pid == pid) {
            slot = index;
            inflight = state->kwin_inflight[index].count;
            break;
        }
        if (slot == KSD_MAX_AUTHORITY_WORKERS
            && state->kwin_inflight[index].count == 0u)
            slot = index;
    }
    if (slot < KSD_MAX_AUTHORITY_WORKERS
        && ksd_authority_admit_kwin(inflight)) {
        state->kwin_inflight[slot].pid = pid;
        state->kwin_inflight[slot].count++;
        reserved = true;
    }
    pthread_mutex_unlock(&state->mutex);
    return reserved;
}

static void release_kwin_slot(authority_state *state, pid_t pid)
{
    pthread_mutex_lock(&state->mutex);
    for (size_t index = 0u; index < KSD_MAX_AUTHORITY_WORKERS; index++) {
        if (state->kwin_inflight[index].count != 0u
            && state->kwin_inflight[index].pid == pid) {
            state->kwin_inflight[index].count--;
            break;
        }
    }
    pthread_mutex_unlock(&state->mutex);
}

#ifdef KSD_AUTHORITY_TESTING
int ksd_authority_test_kwin_slot(unsigned int pid, int reserve)
{
    static authority_state kwin_state = { .mutex = PTHREAD_MUTEX_INITIALIZER };
    if (reserve == 0) {
        release_kwin_slot(&kwin_state, (pid_t)pid);
        return 1;
    }
    return reserve_kwin_slot(&kwin_state, (pid_t)pid) ? 1 : 0;
}
#endif

static bool reserve_capture_memory(authority_state *state, uid_t uid,
                                   pid_t pid)
{
    size_t slot = KSD_MAX_AUTHORITY_WORKERS;
    size_t pid_slot = KSD_MAX_AUTHORITY_WORKERS;
    bool reserved = false;
    pthread_mutex_lock(&state->mutex);
    for (size_t index = 0u; index < KSD_MAX_AUTHORITY_WORKERS; index++) {
        if (state->capture_usage[index].count != 0u
            && state->capture_usage[index].uid == uid) {
            slot = index;
            break;
        }
        if (slot == KSD_MAX_AUTHORITY_WORKERS
            && state->capture_usage[index].count == 0u)
            slot = index;
    }
    for (size_t index = 0u; index < KSD_MAX_AUTHORITY_WORKERS; index++) {
        if (state->capture_pid_usage[index].count != 0u
            && state->capture_pid_usage[index].pid == pid) {
            pid_slot = index;
            break;
        }
        if (pid_slot == KSD_MAX_AUTHORITY_WORKERS
            && state->capture_pid_usage[index].count == 0u)
            pid_slot = index;
    }
    if (state->inflight_bytes <= KSD_MAX_AUTHORITY_INFLIGHT_BYTES
            - KSD_MAX_CAPTURE_BYTES
        && slot < KSD_MAX_AUTHORITY_WORKERS
        && pid_slot < KSD_MAX_AUTHORITY_WORKERS
        && state->capture_usage[slot].count < KSD_MAX_CAPTURES_PER_UID
        && state->capture_pid_usage[pid_slot].count
            < KSD_MAX_CAPTURES_PER_PID) {
        state->inflight_bytes += KSD_MAX_CAPTURE_BYTES;
        state->capture_usage[slot].uid = uid;
        state->capture_usage[slot].count++;
        state->capture_pid_usage[pid_slot].pid = pid;
        state->capture_pid_usage[pid_slot].count++;
        reserved = true;
    }
    pthread_mutex_unlock(&state->mutex);
    return reserved;
}

static void release_capture_memory(authority_state *state, uid_t uid,
                                   pid_t pid)
{
    pthread_mutex_lock(&state->mutex);
    if (state->inflight_bytes >= KSD_MAX_CAPTURE_BYTES)
        state->inflight_bytes -= KSD_MAX_CAPTURE_BYTES;
    for (size_t index = 0u; index < KSD_MAX_AUTHORITY_WORKERS; index++) {
        if (state->capture_usage[index].count != 0u
            && state->capture_usage[index].uid == uid) {
            state->capture_usage[index].count--;
            break;
        }
    }
    for (size_t index = 0u; index < KSD_MAX_AUTHORITY_WORKERS; index++) {
        if (state->capture_pid_usage[index].count != 0u
            && state->capture_pid_usage[index].pid == pid) {
            state->capture_pid_usage[index].count--;
            break;
        }
    }
    pthread_mutex_unlock(&state->mutex);
}

static bool reserve_assembly_memory(authority_state *state, uid_t uid)
{
    size_t slot = KSD_MAX_AUTHORITY_WORKERS;
    bool reserved = false;
    pthread_mutex_lock(&state->mutex);
    for (size_t index = 0u; index < KSD_MAX_AUTHORITY_WORKERS; index++) {
        if (state->assembly_usage[index].count != 0u
            && state->assembly_usage[index].uid == uid) {
            slot = index;
            break;
        }
        if (slot == KSD_MAX_AUTHORITY_WORKERS
            && state->assembly_usage[index].count == 0u)
            slot = index;
    }
    if (state->assembly_bytes <= KSD_MAX_AUTHORITY_ASSEMBLY_BYTES
            - KSD_MAX_REQUEST_TOTAL_PAYLOAD
        && slot < KSD_MAX_AUTHORITY_WORKERS
        && state->assembly_usage[slot].count < KSD_MAX_ASSEMBLIES_PER_UID) {
        state->assembly_bytes += KSD_MAX_REQUEST_TOTAL_PAYLOAD;
        state->assembly_usage[slot].uid = uid;
        state->assembly_usage[slot].count++;
        reserved = true;
    }
    pthread_mutex_unlock(&state->mutex);
    return reserved;
}

static void release_assembly_memory(authority_state *state, uid_t uid)
{
    pthread_mutex_lock(&state->mutex);
    if (state->assembly_bytes >= KSD_MAX_REQUEST_TOTAL_PAYLOAD)
        state->assembly_bytes -= KSD_MAX_REQUEST_TOTAL_PAYLOAD;
    for (size_t index = 0u; index < KSD_MAX_AUTHORITY_WORKERS; index++) {
        if (state->assembly_usage[index].count != 0u
            && state->assembly_usage[index].uid == uid) {
            state->assembly_usage[index].count--;
            break;
        }
    }
    pthread_mutex_unlock(&state->mutex);
}

static bool execute_operation(authority_session *session,
                              const ksd_frame *request)
{
    uint32_t scope = ksd_operation_scope(request->opcode);
    uint64_t bit = ksd_operation_bit(request->opcode);
    session->backend = registered_backend(session->state,
                                          session->identity.uid);
    uint64_t available = registered_operations(session->state,
                                               session->identity.uid,
                                               session->backend);
    bool generation_changed = false;

    if (bit == 0u)
        return forward_response(session, request, KSD_STATUS_UNSUPPORTED, 0u,
                                "unknown desktop operation",
                                NULL, 0u, false);
    if (session->role != KSD_ROLE_RPC
        && session->role != KSD_ROLE_EVENT_STREAM)
        return forward_response(session, request, KSD_STATUS_INVALID_REQUEST,
                                0u, "operation is invalid for this role",
                                NULL, 0u, false);
    if (scope == 0u && !ksd_operation_scope_free(request->opcode))
        return forward_response(session, request, KSD_STATUS_INTERNAL, 0u,
                                "operation carries no permission scope",
                                NULL, 0u, false);
    if (scope == 0u ? !session_identity_refresh(session)
                    : !session_refresh(session, true, true,
                                       &generation_changed))
        return false;
    if (scope != 0u && (session->granted_scopes & scope) != scope)
        return forward_response(session, request, KSD_STATUS_DENIED, 0u,
                                "required permission is not granted",
                                NULL, 0u, false);
    if ((available & bit) == 0u)
        return forward_response(session, request, KSD_STATUS_UNAVAILABLE, 0u,
                                "operation is unavailable on this backend",
                                NULL, 0u, false);
    if (request->opcode == KSD_OP_WINDOW_WATCH
        || request->opcode == KSD_OP_CLIPBOARD_WATCH)
        return start_watch(session, request, scope);
    if (session->role != KSD_ROLE_RPC)
        return forward_response(session, request, KSD_STATUS_INVALID_REQUEST,
                                0u, "event streams only accept subscriptions",
                                NULL, 0u, false);
    bool capture = request->opcode == KSD_OP_CAPTURE_AREA
        || request->opcode == KSD_OP_CAPTURE_WINDOW;
    /* Rule F1. A KWin operation that reaches the script is held to four per
     * consumer, refused before the provider call rather than queued, because
     * BUSY that provably never reached the compositor is always safe to retry
     * and a queue entry would not be. */
    bool kwin_script = session->backend == KSD_BACKEND_KWIN && !capture;
    if (kwin_script
        && !reserve_kwin_slot(session->state, session->identity.pid))
        return forward_response(session, request, KSD_STATUS_BUSY, 0u,
                                "too many concurrent requests for this"
                                " compositor", NULL, 0u, false);
    if (capture && !reserve_capture_memory(session->state,
                                           session->identity.uid,
                                           session->identity.pid))
        return forward_response(session, request,
                                KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                                "capture memory budget is busy",
                                NULL, 0u, false);
    uint64_t before = session->generation;
    uint32_t before_backend = session->backend;
    ksd_operation_result result;
    /* Three routes. KWin capture, every X11 verb, and every generic Wayland
     * verb run in the forked, privilege-dropped worker; everything else goes
     * to a compositor provider over the session bus. The generic backend has
     * no provider to go to at all -- that is what makes it generic. */
    if (kwin_script) {
        /* Everything KWin serves that is not a capture goes to the script, and
         * the only way to reach it is the socket its daemon handed over. */
        ksd_kwin_relay *relay = registered_relay(session->state,
                                                 session->identity.uid);

        if (relay == NULL) {
            ksd_result_init(&result);
            ksd_result_error(&result, KSD_STATUS_UNAVAILABLE, 0u,
                             "this compositor has no script channel");
        } else {
            uint64_t now = monotonic_milliseconds();

            (void)ksd_kwin_relay_call(relay, request,
                                      now + KSD_KWIN_OP_DEADLINE_MS, &result);
            ksd_kwin_relay_release(relay);
        }
    } else if (session->backend == KSD_BACKEND_KWIN
        || session->backend == KSD_BACKEND_X11
        || session->backend == KSD_BACKEND_GENERIC) {
        pid_t session_pid = registered_backend_pid(session->state,
                                                   session->identity.uid);
        ksd_kwin_relay *display = NULL;

        /* Captures keep the one-shot worker. A capture is rare and expensive
         * enough that per-request isolation is worth more than the setup it
         * saves, and it needs the pipe and spool a persistent worker does not
         * hold. Everything else goes to the session's worker, which has its
         * display already open. */
        if (!capture && session->backend != KSD_BACKEND_KWIN)
            display = display_relay_for(session->state, &session->identity,
                                        session->gid, session_pid,
                                        session->backend);
        if (display != NULL) {
            uint64_t now = monotonic_milliseconds();
            bool gone;

            (void)ksd_kwin_relay_call(display, request,
                                      now + KSD_DISPLAY_DEADLINE_MS, &result);
            /* A worker that has gone is not a failed operation. Falling back
             * to a one-shot worker answers the request the caller actually
             * made, and dropping the dead one here is what makes the next
             * request start a fresh persistent worker rather than find this
             * corpse and fall back again. */
            gone = result.status == KSD_STATUS_UNAVAILABLE;
            if (gone) {
                ksd_result_clear(&result);
                forget_display_relay(session->state, &session->identity,
                                     display);
            }
            ksd_kwin_relay_release(display);
            if (gone)
                display = NULL;
        }
        if (display == NULL)
            ksd_capture_worker_execute(&session->identity, session->gid,
                                       request, capture_still_authorized,
                                       session, session_pid,
                                       session->backend, &result);
    } else {
        pid_t provider_pid = registered_provider_pid(session->state,
            session->identity.uid, session->backend);
        ksd_provider_execute(session->identity.uid, session->identity.pid,
                             provider_pid, session->backend, request, &result);
    }
    bool valid = scope == 0u
        ? session_identity_refresh(session)
            && registered_backend(session->state, session->identity.uid)
                == before_backend
        : session_refresh(session, true, true, &generation_changed)
            && !generation_changed && session->generation == before
            && (session->granted_scopes & scope) == scope;
    bool ok;
    if (!valid)
        ok = forward_response(session, request,
                              scope == 0u ? KSD_STATUS_CANCELLED
                                          : KSD_STATUS_REVOKED,
                              0u, scope == 0u
                                ? "application identity or backend changed"
                                : "permission changed during the operation",
                              NULL, 0u, false);
    else if (result.payload_fd >= 0)
        ok = forward_capture(session, request, &result);
    else
        ok = forward_response(session, request, result.status, result.detail,
            result.status == KSD_STATUS_OK || result.diagnostic[0] == '\0'
                ? NULL : result.diagnostic,
            result.tail, result.tail_length, false);
    ksd_result_clear(&result);
    if (capture)
        release_capture_memory(session->state, session->identity.uid,
                               session->identity.pid);
    if (kwin_script)
        release_kwin_slot(session->state, session->identity.pid);
    return ok;
}

static bool handle_public_request(authority_session *session,
                                  const ksd_frame *request)
{
    if (!ksd_frame_is_request(request))
        return false;
    if (request->opcode == KSD_OP_PING) {
        if (request->payload_length != 0u)
            return request->request_id != 0u
                && forward_response(session, request,
                    KSD_STATUS_INVALID_REQUEST, 0u,
                    "PING payload must be empty", NULL, 0u, false);
        if (!session_refresh(session, true, true, NULL))
            return false;
        return request->request_id == 0u
            || forward_response(session, request, KSD_STATUS_OK, 0u,
                                NULL, NULL, 0u, false);
    }
    if (request->request_id == 0u)
        return false;
    if (request->opcode == KSD_OP_HELLO)
        return forward_response(session, request, KSD_STATUS_INVALID_REQUEST,
                                0u, "HELLO may only be sent once",
                                NULL, 0u, false);
    if (request->opcode == KSD_OP_AUTHORIZE)
        return handle_authorize(session, request);
    if (request->opcode == KSD_OP_PERMISSIONS_LIST) {
        if (session->role != KSD_ROLE_RPC)
            return forward_response(session, request,
                                    KSD_STATUS_INVALID_REQUEST, 0u,
                                    "permissions administration requires RPC",
                                    NULL, 0u, false);
        return permissions_list(session, request);
    }
    if (request->opcode == KSD_OP_PERMISSIONS_REVOKE) {
        if (session->role != KSD_ROLE_RPC)
            return forward_response(session, request,
                                    KSD_STATUS_INVALID_REQUEST, 0u,
                                    "permissions administration requires RPC",
                                    NULL, 0u, false);
        return permissions_revoke(session, request);
    }
    return execute_operation(session, request);
}

static bool assembly_within_deadline(const authority_session *session)
{
    uint64_t now = monotonic_seconds();
    return now != 0u && now <= session->assembly_deadline;
}

static void end_assembly(authority_session *session)
{
    ksd_request_assembly_clear(&session->assembly);
    (void)set_socket_timeouts(session->descriptor, 130u);
}

static bool may_begin_assembly(authority_session *session,
                               const ksd_frame *frame)
{
    uint64_t now = monotonic_seconds();
    if (session->role != KSD_ROLE_RPC || now == 0u
        || !ksd_request_chunk_admissible(frame->opcode, frame->flags,
                                         frame->request_id))
        return false;
    if (!set_socket_timeouts(session->descriptor,
                             (uint32_t)KSD_MAX_REQUEST_ASSEMBLY_SECONDS))
        return false;
    session->assembly_deadline = now + KSD_MAX_REQUEST_ASSEMBLY_SECONDS;
    return true;
}

static bool handle_public_frame(authority_session *session,
                                const ksd_frame *frame)
{
    bool starting = !ksd_request_assembly_active(&session->assembly);

    if (starting && (frame->flags & KSD_FLAG_MORE) == 0u)
        return handle_public_request(session, frame);
    if (starting && !may_begin_assembly(session, frame))
        return false;
    if (!assembly_within_deadline(session))
        return false;
    ksd_assembly_result accepted =
        ksd_request_assembly_accept(&session->assembly, frame);
    if (accepted == KSD_ASSEMBLY_INVALID)
        return false;
    if (starting && !reserve_assembly_memory(session->state,
                                             session->identity.uid)) {
        end_assembly(session);
        (void)forward_response(session, frame,
                               KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                               "assembly budget exhausted", NULL, 0u, false);
        return false;
    }
    if (accepted == KSD_ASSEMBLY_PENDING)
        return true;
    ksd_frame assembled;
    bool taken = ksd_request_assembly_take(&session->assembly, &assembled);
    end_assembly(session);
    release_assembly_memory(session->state, session->identity.uid);
    if (!taken)
        return false;
    bool ok = handle_public_request(session, &assembled);
    ksd_frame_clear(&assembled);
    return ok;
}

static bool parse_hello(const ksd_frame *request, uint16_t *role,
                        uint16_t *auth_mode, uint32_t *scopes)
{
    ksd_cursor cursor;
    uint64_t reserved;

    ksd_cursor_init(&cursor, request->payload, request->payload_length);
    return request->opcode == KSD_OP_HELLO && request->flags == 0u
        && request->request_id != 0u && request->payload_length == 16u
        && ksd_cursor_u16(&cursor, role)
        && ksd_cursor_u16(&cursor, auth_mode)
        && ksd_cursor_u32(&cursor, scopes)
        && ksd_cursor_u64(&cursor, &reserved)
        && ksd_cursor_finished(&cursor) && reserved == 0u
        && (*role == KSD_ROLE_RPC || *role == KSD_ROLE_EVENT_STREAM
            || *role == KSD_ROLE_AUTHORIZATION_LEASE)
        && (*auth_mode == KSD_AUTH_CHECK || *auth_mode == KSD_AUTH_REQUEST)
        && (*scopes & ~(uint32_t)KSD_DESKTOP_ACCEPTED_SCOPES) == 0u;
}

static const char *status_diagnostic(uint32_t status)
{
    switch (status) {
        case KSD_STATUS_DENIED: return "permission is not granted";
        case KSD_STATUS_INVALID_REQUEST: return "invalid authorization scope";
        case KSD_STATUS_UNAVAILABLE: return "authorization service unavailable";
        case KSD_STATUS_CANCELLED: return "authorization was cancelled";
        case KSD_STATUS_REVOKED: return "permission changed";
        default: return "internal service error";
    }
}

static bool start_public_session(authority_session *session)
{
    ksd_frame hello;
    uint16_t role;
    uint16_t auth_mode;
    uint32_t scopes;
    uint32_t granted = 0u;
    uint32_t status = KSD_STATUS_INVALID_REQUEST;
    uint8_t tail[24] = { 0 };

    int received = ksd_frame_read(session->client_fd, public_magic,
        KSD_PROTOCOL_MAJOR, KSD_PROTOCOL_MINOR, KSD_MAX_REQUEST_PAYLOAD,
        true, &hello);
    if (received != 1)
        return false;
    if (!parse_hello(&hello, &role, &auth_mode, &scopes)) {
        bool replied = hello.request_id != 0u
            && forward_response(session, &hello, KSD_STATUS_INVALID_REQUEST,
                0u, "HELLO must be first", NULL, 0u, false);
        ksd_frame_clear(&hello);
        (void)replied;
        return false;
    }
    session->role = role;
    status = KSD_STATUS_OK;
    if (scopes != 0u)
        status = authorize_scopes(session, scopes, auth_mode, &granted);
    bool replied;
    if (status == KSD_STATUS_OK) {
        ksd_encode_u32(tail, session->granted_scopes);
        ksd_encode_u64(tail + 8u,
                       registered_operations(session->state,
                                             session->identity.uid,
                                             session->backend));
        ksd_encode_u32(tail + 16u, session->backend);
        replied = forward_response(session, &hello, KSD_STATUS_OK, 0u,
                                   NULL, tail, sizeof(tail), false);
    } else {
        replied = forward_response(session, &hello, status, 0u,
                                   status_diagnostic(status),
                                   NULL, 0u, false);
    }
    ksd_frame_clear(&hello);
    return replied && status == KSD_STATUS_OK;
}

static void handle_public_connection(authority_state *state, int descriptor,
                                     const struct ucred *peer)
{
    authority_session session = {
        .state = state,
        .client_fd = descriptor,
        .gid = peer->gid,
    };
    if (peer->uid == 0u
        || ksp_identity_capture(peer->pid, peer->uid, &session.identity) != 0)
        return;
    session.identity_checked_at = monotonic_seconds();
    session.backend = registered_backend(state, peer->uid);
    if (!start_public_session(&session))
        return;
    session.descriptor = descriptor;
    ksd_request_assembly_init(&session.assembly);
    for (;;) {
        struct pollfd item = {
            .fd = descriptor,
            .events = POLLIN | POLLRDHUP | POLLHUP | POLLERR,
        };
        int ready = poll(&item, 1u, KSD_GENERATION_POLL_MS);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0)
            break;
        if (ready == 0) {
            if (ksd_request_assembly_active(&session.assembly)
                && !assembly_within_deadline(&session))
                break;
            bool force_identity =
                monotonic_seconds() != session.identity_checked_at;
            if (!session_refresh(&session, force_identity, true, NULL))
                break;
            continue;
        }
        if ((item.revents & POLLIN) != 0) {
            ksd_frame request;
            int received = ksd_frame_read(descriptor, public_magic,
                KSD_PROTOCOL_MAJOR, KSD_PROTOCOL_MINOR,
                KSD_MAX_REQUEST_PAYLOAD, true, &request);
            if (received != 1)
                break;
            bool ok = handle_public_frame(&session, &request);
            ksd_frame_clear(&request);
            if (!ok)
                break;
        }
        if ((item.revents
             & (POLLRDHUP | POLLHUP | POLLERR | POLLNVAL)) != 0)
            break;
    }
    if (ksd_request_assembly_active(&session.assembly)) {
        end_assembly(&session);
        release_assembly_memory(state, session.identity.uid);
    }
}

static void *connection_worker(void *argument)
{
    authority_client *client = argument;
    uint8_t magic[4];
    if (peek_connection_magic(client->descriptor, magic)) {
        bool registration = memcmp(magic, ksd_backend_registration_magic,
                                   sizeof(magic)) == 0;
        /* The kind is only known now, so this is where a slot drawn from the
         * registration reserve is kept or given back. An ordinary connection
         * on a reserved slot is the starvation the reserve exists to prevent,
         * and it is closed before it can say anything. */
        if (!ksd_authority_worker_keeps_slot(client->from_reserve,
                                             registration)) {
            registration = false;
        } else if (registration) {
            handle_backend_connection(client->state, client->descriptor,
                                      &client->credentials);
        } else if (memcmp(magic, public_magic, sizeof(magic)) == 0) {
            if (set_socket_timeouts(client->descriptor, 130u))
                handle_public_connection(client->state, client->descriptor,
                                         &client->credentials);
        }
    }
    close(client->descriptor);
    pthread_mutex_lock(&client->state->mutex);
    client->state->workers--;
    for (size_t index = 0u; index < KSD_MAX_AUTHORITY_WORKERS; index++)
        if (client->state->worker_usage[index].count != 0u
            && client->state->worker_usage[index].uid
                == client->credentials.uid) {
            client->state->worker_usage[index].count--;
            break;
        }
    pthread_mutex_unlock(&client->state->mutex);
    free(client);
    return NULL;
}

static bool reserve_worker(authority_state *state, uid_t uid,
                           bool *from_reserve)
{
    size_t slot = KSD_MAX_AUTHORITY_WORKERS;
    size_t uid_workers = 0u;
    bool reserved = false;
    pthread_mutex_lock(&state->mutex);
    for (size_t index = 0u; index < KSD_MAX_AUTHORITY_WORKERS; index++) {
        if (state->worker_usage[index].count != 0u
            && state->worker_usage[index].uid == uid) {
            slot = index;
            uid_workers = state->worker_usage[index].count;
            break;
        }
        if (slot == KSD_MAX_AUTHORITY_WORKERS
            && state->worker_usage[index].count == 0u)
            slot = index;
    }
    if (slot < KSD_MAX_AUTHORITY_WORKERS
        && ksd_authority_admit_worker(state->workers, uid_workers,
                                      from_reserve)) {
        state->workers++;
        state->worker_usage[slot].uid = uid;
        state->worker_usage[slot].count++;
        reserved = true;
    }
    pthread_mutex_unlock(&state->mutex);
    return reserved;
}

int ksd_authority_main(int argc, char **argv)
{
    authority_state state = { .mutex = PTHREAD_MUTEX_INITIALIZER };
    ksp_store_config store_config;
    int listener;
    bool activation_present = false;
    (void)argv;

    if (argc != 1) {
        fputs("usage: keysharp-desktop authority-daemon\n", stderr);
        return 2;
    }
    /* Either wholly root or not root at all. A process with mismatched real
     * and effective ids is a setuid binary partway through something, and
     * which of its two identities the ownership rule should follow has no
     * good answer -- so it is refused rather than guessed at. */
    if (getuid() != geteuid() || getgid() != getegid()) {
        fputs("keysharp-desktop authority-daemon: refusing mismatched "
              "credentials\n", stderr);
        return 1;
    }
    if (!ksd_install_is_system()) {
        /* A user installation serves the one user running it, and its grant
         * store is writable by that user -- so a grant records what this
         * user allowed rather than bounding what they may do. Said once, at
         * startup, because it changes what the service promises. */
        fputs("keysharp-desktop authority-daemon: user installation. "
              "Grants live in this user's own directory and can be edited "
              "by any process running as this user, so they are not "
              "enforceable against it. Install as root for that.\n",
              stderr);
        if (ksd_install_persistent_directory() == NULL
            || ksd_install_runtime_directory() == NULL) {
            fputs("keysharp-desktop authority-daemon: no private "
                  "directory to keep grants in\n", stderr);
            return 1;
        }
    }
    ksp_store_config_init(&store_config, KSD_DESKTOP_MANAGED_SCOPES);
    store_config.read_scopes = KSD_DESKTOP_ACCEPTED_SCOPES;
    /* NULL in a system installation, which leaves the library's own
     * system-wide defaults exactly where they were. */
    if (!ksd_install_is_system()) {
        store_config.persistent_directory = ksd_install_persistent_directory();
        store_config.runtime_directory = ksd_install_runtime_directory();
        store_config.owner_uid = ksd_install_owner();
    }
    if (ksp_store_create(&state.store, &store_config) != 0
        || ksp_store_prepare(state.store) != 0) {
        fputs("keysharp-desktop authority-daemon: permission store unavailable\n",
              stderr);
        ksp_store_destroy(state.store);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    listener = inherited_socket(&activation_present);
    if (listener < 0 && !activation_present)
        listener = create_socket();
    if (listener < 0) {
        perror("keysharp-desktop authority-daemon: listen");
        ksp_store_destroy(state.store);
        return 1;
    }

    for (;;) {
        int connection = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
        if (connection < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS
                || errno == ENOMEM) {
                struct timespec delay = {
                    .tv_sec = 0,
                    .tv_nsec = 100000000L,
                };
                (void)nanosleep(&delay, NULL);
                continue;
            }
            break;
        }
        if (!set_socket_timeouts(connection, 5u)) {
            close(connection);
            continue;
        }
        struct ucred credentials;
        socklen_t credentials_size = sizeof(credentials);
        authority_client *client = calloc(1u, sizeof(*client));
        bool from_reserve = false;
        if (client == NULL
            || getsockopt(connection, SOL_SOCKET, SO_PEERCRED,
                          &credentials, &credentials_size) != 0
            || credentials_size != sizeof(credentials)
            || credentials.uid == 0u
            || !reserve_worker(&state, credentials.uid, &from_reserve)) {
            free(client);
            close(connection);
            continue;
        }
        client->state = &state;
        client->descriptor = connection;
        client->credentials = credentials;
        client->from_reserve = from_reserve;
        pthread_t worker;
        if (pthread_create(&worker, NULL, connection_worker, client) != 0) {
            pthread_mutex_lock(&state.mutex);
            state.workers--;
            for (size_t index = 0u; index < KSD_MAX_AUTHORITY_WORKERS;
                 index++)
                if (state.worker_usage[index].count != 0u
                    && state.worker_usage[index].uid == credentials.uid) {
                    state.worker_usage[index].count--;
                    break;
                }
            pthread_mutex_unlock(&state.mutex);
            close(connection);
            free(client);
            continue;
        }
        pthread_detach(worker);
    }
    close(listener);
    ksp_store_destroy(state.store);
    pthread_mutex_destroy(&state.mutex);
    return 1;
}

#ifdef KSD_AUTHORITY_TESTING
int ksd_authority_test_capture_budget(unsigned int uid, unsigned int pid,
                                      int reserve)
{
    static authority_state capture_state = { .mutex = PTHREAD_MUTEX_INITIALIZER };
    if (reserve == 0) {
        release_capture_memory(&capture_state, (uid_t)uid, (pid_t)pid);
        return 1;
    }
    return reserve_capture_memory(&capture_state, (uid_t)uid, (pid_t)pid)
        ? 1 : 0;
}

int ksd_authority_test_assembly_budget(unsigned int uid, int reserve)
{
    static authority_state budget_state = { .mutex = PTHREAD_MUTEX_INITIALIZER };
    if (reserve == 0) {
        release_assembly_memory(&budget_state, (uid_t)uid);
        return 1;
    }
    return reserve_assembly_memory(&budget_state, (uid_t)uid) ? 1 : 0;
}


int ksd_authority_test_generic_session(int descriptor,
                                       const struct ucred *peer,
                                       const char *persistent_directory,
                                       const char *runtime_directory)
{
    authority_state state = { .mutex = PTHREAD_MUTEX_INITIALIZER };
    ksp_store_config store_config;
    ksp_identity identity;

    if (peer == NULL || peer->uid == 0u
        || ksp_identity_capture(peer->pid, peer->uid, &identity) != 0)
        return -1;
    ksp_store_config_init(&store_config, KSD_DESKTOP_MANAGED_SCOPES);
    store_config.read_scopes = KSD_DESKTOP_ACCEPTED_SCOPES;
    store_config.persistent_directory = persistent_directory;
    store_config.runtime_directory = runtime_directory;
    store_config.owner_uid = peer->uid;
    if (ksp_store_create(&state.store, &store_config) != 0
        || ksp_store_prepare(state.store) != 0) {
        ksp_store_destroy(state.store);
        pthread_mutex_destroy(&state.mutex);
        return -1;
    }
    state.backends[0].active = true;
    state.backends[0].uid = peer->uid;
    state.backends[0].descriptor = descriptor;
    state.backends[0].backend = KSD_BACKEND_GENERIC;
    state.backends[0].identity = identity;
    handle_public_connection(&state, descriptor, peer);
    ksp_store_destroy(state.store);
    pthread_mutex_destroy(&state.mutex);
    return 0;
}
#endif
