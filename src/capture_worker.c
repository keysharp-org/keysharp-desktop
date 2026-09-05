#include "capture_worker.h"
#include "install_mode.h"

#include "protocol.h"
#include "local_capture.h"
#include "wl_connect.h"
#include "wl_worker.h"
#include "x11_connect.h"
#include "x11_worker.h"
#include "x11_watch.h"
#include "transport.h"
#include "kwin_relay.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <linux/memfd.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define KSD_CAPTURE_WORKER_FD 3
/* v3 carries the backend. v2 did not, and the worker guessed the route from
 * the payload shape instead -- which sent every KWin capture to the X11 path,
 * because an area capture looks identical either way. That path then refused
 * the XWayland server KWin runs, so KWin area capture could never succeed. */
/* v4 carries a persist flag. A worker that persists serves many requests on
 * one display connection, which is where the fork, the exec, the privilege
 * drop and the connect stop being per-query costs. */
#define KSD_CAPTURE_WORKER_VERSION 4u
#define KSD_CAPTURE_WORKER_HEADER_SIZE 32u
#define KSD_CAPTURE_WORKER_MAX_GROUPS 256u
#define KSD_CAPTURE_WORKER_ENV_LIMIT (64u * 1024u)
#define KSD_CAPTURE_WORKER_TIMEOUT_MS 35000u
#define KSD_CAPTURE_WORKER_MAX_REQUEST KSD_MAX_REQUEST_PAYLOAD
#define KSD_CAPTURE_WORKER_BOOTSTRAP_MAX \
    (KSD_CAPTURE_WORKER_HEADER_SIZE \
     + KSD_CAPTURE_WORKER_MAX_GROUPS * sizeof(uint32_t) \
     + KSD_FRAME_HEADER_SIZE + KSD_CAPTURE_WORKER_MAX_REQUEST)
#define KSD_CAPTURE_WORKER_RESPONSE_SIZE \
    (24u + KSD_DIAGNOSTIC_CAPACITY)
#define KSD_CAPTURE_WORKER_MEMORY_LIMIT (384u * 1024u * 1024u)
#ifndef KSD_CAPTURE_WORKER_PATH
#define KSD_CAPTURE_WORKER_PATH "/usr/libexec/keysharp-desktop-capture-worker"
#endif

static const uint8_t worker_magic[4] = { 'K', 'S', 'C', 'W' };
static const uint8_t public_magic[4] = {
    KSD_FRAME_MAGIC_0, KSD_FRAME_MAGIC_1,
    KSD_FRAME_MAGIC_2, KSD_FRAME_MAGIC_3,
};

static bool set_socket_timeout(int descriptor, uint32_t timeout_ms)
{
    struct timeval timeout = {
        .tv_sec = (time_t)(timeout_ms / 1000u),
        .tv_usec = (suseconds_t)((timeout_ms % 1000u) * 1000u),
    };
    return setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO,
                      &timeout, sizeof(timeout)) == 0
        && setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO,
                      &timeout, sizeof(timeout)) == 0;
}

typedef struct worker_environment {
    char runtime[96];
    char bus[192];
    char *values[4];
} worker_environment;

static uint64_t monotonic_milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0u;
    return (uint64_t)now.tv_sec * 1000u
        + (uint64_t)now.tv_nsec / 1000000u;
}

static bool same_identity(const ksp_identity *left,
                          const ksp_identity *right)
{
    return left->uid == right->uid && left->pid == right->pid
        && left->start_time == right->start_time
        && strcmp(left->hash, right->hash) == 0;
}

static bool read_bounded_file(const char *path, uint8_t **contents,
                              size_t *length)
{
    uint8_t *buffer = malloc(KSD_CAPTURE_WORKER_ENV_LIMIT + 1u);
    int descriptor = -1;
    size_t offset = 0u;

    if (buffer == NULL)
        return false;
    descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        goto failed;
    while (offset <= KSD_CAPTURE_WORKER_ENV_LIMIT) {
        ssize_t count = read(descriptor, buffer + offset,
                             KSD_CAPTURE_WORKER_ENV_LIMIT + 1u - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            goto failed;
        if (count == 0)
            break;
        offset += (size_t)count;
    }
    close(descriptor);
    if (offset > KSD_CAPTURE_WORKER_ENV_LIMIT)
        goto failed_closed;
    *contents = buffer;
    *length = offset;
    return true;

failed:
    if (descriptor >= 0)
        close(descriptor);
failed_closed:
    free(buffer);
    return false;
}

static bool environment_value(const uint8_t *environment, size_t length,
                              const char *name, const uint8_t **value,
                              size_t *value_length)
{
    size_t name_length = strlen(name);
    bool found = false;
    size_t offset = 0u;

    while (offset < length) {
        const uint8_t *entry = environment + offset;
        const uint8_t *end = memchr(entry, '\0', length - offset);
        if (end == NULL)
            return false;
        size_t entry_length = (size_t)(end - entry);
        if (entry_length > name_length
            && memcmp(entry, name, name_length) == 0
            && entry[name_length] == '=') {
            if (found)
                return false;
            found = true;
            *value = entry + name_length + 1u;
            *value_length = entry_length - name_length - 1u;
        }
        offset += entry_length + 1u;
    }
    return found;
}

static bool capture_environment(const ksp_identity *identity,
                                worker_environment *result)
{
    char path[64];
    char expected_runtime[64];
    char expected_bus[128];
    uint8_t *environment = NULL;
    size_t environment_length = 0u;
    const uint8_t *runtime;
    const uint8_t *bus;
    size_t runtime_length;
    size_t bus_length;
    ksp_identity verified;

    int path_length = snprintf(path, sizeof(path), "/proc/%ld/environ",
                               (long)identity->pid);
    int runtime_size = snprintf(expected_runtime, sizeof(expected_runtime),
                                "/run/user/%lu",
                                (unsigned long)identity->uid);
    int bus_size = snprintf(expected_bus, sizeof(expected_bus),
                            "unix:path=/run/user/%lu/bus",
                            (unsigned long)identity->uid);
    if (path_length <= 0 || (size_t)path_length >= sizeof(path)
        || runtime_size <= 0 || (size_t)runtime_size >= sizeof(expected_runtime)
        || bus_size <= 0 || (size_t)bus_size >= sizeof(expected_bus)
        || ksp_identity_revalidate(identity, &verified) != 0
        || !same_identity(identity, &verified)
        || !read_bounded_file(path, &environment, &environment_length))
        return false;
    bool valid = environment_value(environment, environment_length,
                                   "XDG_RUNTIME_DIR", &runtime,
                                   &runtime_length)
        && runtime_length == (size_t)runtime_size
        && memcmp(runtime, expected_runtime, runtime_length) == 0;
    bool has_bus = environment_value(environment, environment_length,
                                     "DBUS_SESSION_BUS_ADDRESS", &bus,
                                     &bus_length);
    valid = valid && (!has_bus
        || (bus_length == (size_t)bus_size
            && memcmp(bus, expected_bus, bus_length) == 0));
    free(environment);
    if (!valid || ksp_identity_revalidate(identity, &verified) != 0
        || !same_identity(identity, &verified))
        return false;
    (void)snprintf(result->runtime, sizeof(result->runtime),
                   "XDG_RUNTIME_DIR=%s", expected_runtime);
    (void)snprintf(result->bus, sizeof(result->bus),
                   "DBUS_SESSION_BUS_ADDRESS=%s", expected_bus);
    result->values[0] = result->runtime;
    result->values[1] = result->bus;
    result->values[2] = "PATH=/usr/bin:/bin";
    result->values[3] = NULL;
    return true;
}

static bool capture_groups(const ksp_identity *identity, gid_t **groups,
                           size_t *group_count)
{
    char path[64];
    char line[8192];
    FILE *status = NULL;
    gid_t *values = NULL;
    size_t count = 0u;
    bool parsed_groups = false;
    ksp_identity verified;

    int length = snprintf(path, sizeof(path), "/proc/%ld/status",
                          (long)identity->pid);
    if (length <= 0 || (size_t)length >= sizeof(path)
        || ksp_identity_revalidate(identity, &verified) != 0
        || !same_identity(identity, &verified))
        return false;
    status = fopen(path, "re");
    if (status == NULL)
        return false;
    while (fgets(line, sizeof(line), status) != NULL) {
        if (strncmp(line, "Groups:", 7u) != 0)
            continue;
        char *cursor = line + 7u;
        values = calloc(KSD_CAPTURE_WORKER_MAX_GROUPS, sizeof(*values));
        if (values == NULL)
            break;
        while (*cursor != '\0') {
            while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n')
                cursor++;
            if (*cursor == '\0')
                break;
            if (count >= KSD_CAPTURE_WORKER_MAX_GROUPS)
                goto done;
            errno = 0;
            char *end;
            unsigned long parsed = strtoul(cursor, &end, 10);
            if (errno != 0 || end == cursor || parsed > (unsigned long)UINT_MAX)
                goto done;
            values[count++] = (gid_t)parsed;
            cursor = end;
        }
        parsed_groups = strchr(line, '\n') != NULL;
        break;
    }

done:
    fclose(status);
    if (values == NULL || !parsed_groups
        || ksp_identity_revalidate(identity, &verified) != 0
        || !same_identity(identity, &verified)) {
        free(values);
        return false;
    }
    *groups = values;
    *group_count = count;
    return true;
}

/* What the worker will run, for both the parent that decides whether to fork
 * and the child that decides what to dispatch. One definition, because two
 * that were meant to agree did not, and the disagreement was invisible: the
 * parent simply refused, with a diagnostic about something else. */
bool ksd_capture_worker_request_valid(const ksd_frame *request)
{
    return ksd_local_capture_request_valid(request)
        || ksd_x11_request_valid(request)
        || ksd_wayland_request_valid(request);
}

static bool trusted_self(int descriptor)
{
    struct stat status;
    return descriptor >= 0 && fstat(descriptor, &status) == 0
        && S_ISREG(status.st_mode)
        && ksd_install_owner_trusted(status.st_uid)
        && (status.st_mode & 0777u) == 0700u;
}

static bool create_capture_pipe(int descriptors[2])
{
    /* Owned by whoever this installation belongs to and openable by nobody:
     * the mode is what keeps another process from reaching it through /proc,
     * and the ownership says which party created it. */
    if (pipe2(descriptors, O_CLOEXEC) != 0)
        return false;
    int read_flags = fcntl(descriptors[0], F_GETFL);
    if (fchown(descriptors[0], ksd_install_owner(), ksd_install_group()) != 0
        || fchmod(descriptors[0], 0u) != 0 || read_flags < 0
        || fcntl(descriptors[0], F_SETFL, read_flags | O_NONBLOCK) != 0
        || !ksd_capture_pipe_valid(descriptors)) {
        close(descriptors[0]);
        close(descriptors[1]);
        descriptors[0] = -1;
        descriptors[1] = -1;
        return false;
    }
    return true;
}

static int create_capture_spool(void)
{
    int descriptor = memfd_create("keysharp-desktop-capture-spool",
                                  MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (descriptor < 0
        || fchown(descriptor, ksd_install_owner(), ksd_install_group()) != 0
        || fchmod(descriptor, 0u) != 0
        || ftruncate(descriptor, 0) != 0
        || !ksd_capture_spool_valid(descriptor)) {
        if (descriptor >= 0)
            close(descriptor);
        return -1;
    }
    return descriptor;
}

static bool send_bootstrap(int descriptor, const ksp_identity *identity,
                           gid_t gid, const gid_t *groups, size_t group_count,
                           const ksd_frame *request,
                           const int capture_pipe[2], pid_t session_pid,
                           uint32_t backend, bool persistent)
{
    ksd_buffer frame;
    ksd_buffer message;
    ksd_buffer_init(&frame, KSD_FRAME_HEADER_SIZE
                              + KSD_CAPTURE_WORKER_MAX_REQUEST);
    ksd_buffer_init(&message, KSD_CAPTURE_WORKER_BOOTSTRAP_MAX);
    bool ok = group_count <= KSD_CAPTURE_WORKER_MAX_GROUPS
        && (persistent
            ? request == NULL
            : (request != NULL
               && request->payload_length <= KSD_CAPTURE_WORKER_MAX_REQUEST
               && ksd_frame_pack(request, &frame)))
        && frame.length <= UINT32_MAX
        && ksd_buffer_bytes(&message, worker_magic, sizeof(worker_magic))
        && ksd_buffer_u16(&message, KSD_CAPTURE_WORKER_VERSION)
        && ksd_buffer_u16(&message, (uint16_t)backend)
        && ksd_buffer_u32(&message, (uint32_t)identity->uid)
        && ksd_buffer_u32(&message, (uint32_t)gid)
        && ksd_buffer_u32(&message, (uint32_t)group_count)
        && ksd_buffer_u32(&message, (uint32_t)frame.length)
        && ksd_buffer_u32(&message, (uint32_t)session_pid)
        && ksd_buffer_u32(&message, persistent ? 1u : 0u);
    for (size_t index = 0u; ok && index < group_count; index++)
        ok = ksd_buffer_u32(&message, (uint32_t)groups[index]);
    ok = ok && ksd_buffer_bytes(&message, frame.data, frame.length);
    ssize_t written = -1;
    if (ok) {
        uint8_t control[CMSG_SPACE(sizeof(int) * 2u)] = { 0 };
        struct iovec iov = {
            .iov_base = message.data,
            .iov_len = message.length,
        };
        struct msghdr packet = {
            .msg_iov = &iov,
            .msg_iovlen = 1u,
            .msg_control = control,
            .msg_controllen = sizeof(control),
        };
        struct cmsghdr *header = CMSG_FIRSTHDR(&packet);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int) * 2u);
        memcpy(CMSG_DATA(header), capture_pipe, sizeof(int) * 2u);
        do {
            written = sendmsg(descriptor, &packet, MSG_NOSIGNAL);
        } while (written < 0 && errno == EINTR);
        ok = written == (ssize_t)message.length;
    }
    ksd_buffer_clear(&message);
    ksd_buffer_clear(&frame);
    return ok;
}

static void terminate_worker(pid_t pid)
{
    if (pid <= 0)
        return;
    (void)kill(pid, SIGKILL);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
    }
}

static bool valid_status(uint32_t status)
{
    return status <= KSD_STATUS_REVOKED || status == KSD_STATUS_INTERNAL;
}

static bool sealed_capture_file(int descriptor, uint32_t length)
{
    struct stat status;
    int required = F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
    int seals = fcntl(descriptor, F_GET_SEALS);
    return descriptor >= 0 && length >= 20u && length <= KSD_MAX_CAPTURE_TAIL
        && fstat(descriptor, &status) == 0 && S_ISREG(status.st_mode)
        && status.st_size == (off_t)length && seals >= 0
        && (seals & required) == required;
}

static bool receive_worker_response(int descriptor,
                                    ksd_operation_result *result)
{
    uint8_t message[KSD_CAPTURE_WORKER_RESPONSE_SIZE];
    int payload_fd = -1;
    if (ksd_receive_optional_fd(descriptor, message, sizeof(message),
                                &payload_fd) != 0)
        return false;
    uint32_t status = ksd_decode_u32(message + 8u);
    uint32_t detail = ksd_decode_u32(message + 12u);
    uint32_t tail_length = ksd_decode_u32(message + 16u);
    uint32_t diagnostic_length = ksd_decode_u32(message + 20u);
    bool valid = memcmp(message, worker_magic, sizeof(worker_magic)) == 0
        && ksd_decode_u16(message + 4u) == KSD_CAPTURE_WORKER_VERSION
        && ksd_decode_u16(message + 6u) == 0u && valid_status(status)
        && diagnostic_length < KSD_DIAGNOSTIC_CAPACITY
        && ksd_utf8_valid(message + 24u, diagnostic_length, false);
    if (!valid) {
        if (payload_fd >= 0)
            close(payload_fd);
        return false;
    }
    if (status == KSD_STATUS_OK && tail_length == 0u) {
        /* A control verb answers OK with nothing to carry. Accepting a
         * descriptor here would let a worker attach one to an operation that
         * has no payload, so it is refused rather than closed and ignored. */
        if (diagnostic_length != 0u || payload_fd >= 0) {
            if (payload_fd >= 0)
                close(payload_fd);
            return false;
        }
        return ksd_result_take(result, NULL, 0u);
    }
    if (status == KSD_STATUS_OK) {
        if (diagnostic_length != 0u
            || !sealed_capture_file(payload_fd, tail_length)) {
            if (payload_fd >= 0)
                close(payload_fd);
            return false;
        }
        void *mapped = mmap(NULL, tail_length, PROT_READ, MAP_PRIVATE,
                            payload_fd, 0);
        if (mapped == MAP_FAILED) {
            close(payload_fd);
            return false;
        }
        bool shaped = ksd_capture_tail_valid(mapped, tail_length);
        munmap(mapped, tail_length);
        if (!shaped) {
            close(payload_fd);
            return false;
        }
        return ksd_result_take_fd(result, payload_fd, tail_length);
    }
    if (payload_fd >= 0 || tail_length != 0u) {
        if (payload_fd >= 0)
            close(payload_fd);
        return false;
    }
    char diagnostic[KSD_DIAGNOSTIC_CAPACITY];
    memcpy(diagnostic, message + 24u, diagnostic_length);
    diagnostic[diagnostic_length] = '\0';
    ksd_result_error(result, status, detail, diagnostic_length == 0u
        ? "capture worker failed" : diagnostic);
    return true;
}

static bool send_worker_response(int descriptor,
                                 const ksd_operation_result *result)
{
    uint8_t message[KSD_CAPTURE_WORKER_RESPONSE_SIZE] = { 0 };
    size_t diagnostic_length = result->status == KSD_STATUS_OK
        ? 0u : strnlen(result->diagnostic, KSD_DIAGNOSTIC_CAPACITY);
    if (!valid_status(result->status)
        || diagnostic_length >= KSD_DIAGNOSTIC_CAPACITY)
        return false;
    memcpy(message, worker_magic, sizeof(worker_magic));
    ksd_encode_u16(message + 4u, KSD_CAPTURE_WORKER_VERSION);
    ksd_encode_u32(message + 8u, result->status);
    ksd_encode_u32(message + 12u, result->detail);
    ksd_encode_u32(message + 16u,
                   result->status == KSD_STATUS_OK ? result->tail_length : 0u);
    ksd_encode_u32(message + 20u, (uint32_t)diagnostic_length);
    memcpy(message + 24u, result->diagnostic, diagnostic_length);
    if (result->status != KSD_STATUS_OK || result->tail_length == 0u) {
        ssize_t written;
        if (result->status == KSD_STATUS_OK && result->tail != NULL)
            return false;
        do {
            written = send(descriptor, message, sizeof(message), MSG_NOSIGNAL);
        } while (written < 0 && errno == EINTR);
        return written == (ssize_t)sizeof(message);
    }
    int payload_fd = result->payload_fd;
    bool borrowed = payload_fd >= 0;
    int seals = F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
    bool valid = result->tail_length <= KSD_MAX_CAPTURE_TAIL
        && ((borrowed && result->tail == NULL
             && sealed_capture_file(payload_fd, result->tail_length))
            || (!borrowed && result->tail != NULL));
    if (!borrowed && valid)
        payload_fd = memfd_create("keysharp-desktop-capture",
                                  MFD_CLOEXEC | MFD_ALLOW_SEALING);
    bool written = valid && payload_fd >= 0
        && (borrowed
            || (ksd_write_all(payload_fd, result->tail, result->tail_length)
                && fcntl(payload_fd, F_ADD_SEALS, seals) == 0))
        && ksd_send_with_fd(descriptor, message, sizeof(message), payload_fd);
    if (!borrowed && payload_fd >= 0)
        close(payload_fd);
    return written;
}

#ifdef KSD_CAPTURE_WORKER_TESTING
/* Both halves of the worker response are static, and the property worth
 * pinning is that they agree, so the hook drives one through the other over a
 * socketpair rather than exposing either on its own. */
/* A well-behaved worker never attaches a descriptor to an answer that has no
 * payload, so the message has to be forged to prove the receiver refuses it.
 * Accepting it would let a worker smuggle a descriptor into any control verb. */
bool ksd_capture_worker_test_scalar_ok_with_fd(ksd_operation_result *received)
{
    uint8_t message[KSD_CAPTURE_WORKER_RESPONSE_SIZE] = { 0 };
    int pair[2];
    int payload_fd = memfd_create("keysharp-desktop-test",
                                  MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (payload_fd < 0)
        return false;
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) {
        close(payload_fd);
        return false;
    }
    memcpy(message, worker_magic, sizeof(worker_magic));
    ksd_encode_u16(message + 4u, KSD_CAPTURE_WORKER_VERSION);
    ksd_encode_u32(message + 8u, KSD_STATUS_OK);
    bool sent = ksd_send_with_fd(pair[0], message, sizeof(message),
                                 payload_fd);
    bool accepted = sent && receive_worker_response(pair[1], received);
    close(payload_fd);
    close(pair[0]);
    close(pair[1]);
    return accepted;
}

bool ksd_capture_worker_test_round_trip(const ksd_operation_result *sent,
                                        ksd_operation_result *received)
{
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0)
        return false;
    bool ok = send_worker_response(pair[0], sent)
        && receive_worker_response(pair[1], received);
    close(pair[0]);
    close(pair[1]);
    return ok;
}
#endif

/* Starts a worker that stays. Returns its socket, which the caller drives with
 * a request/response relay; the worker exits when that socket closes, which is
 * also what makes it die with the authority without relying on PR_SET_PDEATHSIG
 * -- that signal is per-THREAD, so a worker forked from a connection thread
 * would be killed the moment that thread ended, which is the wrong lifetime
 * entirely. End-of-file on the socket is thread-agnostic and exact. */
static void *reap_persistent_worker(void *value)
{
    pid_t worker = (pid_t)(intptr_t)value;
    while (waitpid(worker, NULL, 0) < 0 && errno == EINTR) {
    }
    return NULL;
}

static void *watch_authority_connection(void *value)
{
    struct pollfd socket = {
        .fd = (int)(intptr_t)value, .events = POLLRDHUP,
    };
    for (;;) {
        int ready = poll(&socket, 1u, -1);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0 || (socket.revents & (POLLRDHUP | POLLHUP
                                            | POLLERR | POLLNVAL)) != 0)
            _exit(ready < 0 ? 1 : 0);
    }
}

#ifdef KSD_CAPTURE_WORKER_TESTING
void ksd_capture_worker_test_authority_watch(int descriptor)
{
    (void)watch_authority_connection((void *)(intptr_t)descriptor);
}
#endif

int ksd_capture_worker_spawn(const ksp_identity *identity, gid_t gid,
                             pid_t session_pid, uint32_t backend)
{
    worker_environment environment;
    gid_t *groups = NULL;
    size_t group_count = 0u;
    int executable = -1;
    int sockets[2] = { -1, -1 };
    int capture_pipe[2] = { -1, -1 };
    pid_t worker = -1;
    int kept = -1;
    int child_socket = -1;
    int child_executable = -1;

    if (identity == NULL
        || !capture_environment(identity, &environment)
        || !capture_groups(identity, &groups, &group_count))
        goto done;
    executable = open(KSD_CAPTURE_WORKER_PATH,
                      O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (!trusted_self(executable)
        || socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0
        || !set_socket_timeout(sockets[0], 5000u)
        || !set_socket_timeout(sockets[1], 5000u)
        || !create_capture_pipe(capture_pipe))
        goto done;
    child_socket = fcntl(sockets[1], F_DUPFD_CLOEXEC, 10);
    child_executable = fcntl(executable, F_DUPFD_CLOEXEC, 10);
    if (child_socket < 0 || child_executable < 0)
        goto done;
    worker = fork();
    if (worker == 0) {
        if (dup2(child_socket, KSD_CAPTURE_WORKER_FD) < 0
            || dup2(child_executable, 4) < 0)
            _exit(1);
        char *spawn_argv[] = { KSD_CAPTURE_WORKER_PATH, NULL };

        close_range(5, UINT_MAX, 0);
        fexecve(4, spawn_argv, environment.values);
        _exit(1);
    }
    if (worker < 0)
        goto done;
    close(sockets[1]);
    sockets[1] = -1;
    if (!send_bootstrap(sockets[0], identity, gid, groups, group_count, NULL,
                        capture_pipe, session_pid, backend, true))
        goto done;
    uint8_t ready = 0u;
    if (!ksd_read_all(sockets[0], &ready, sizeof(ready)) || ready != 1u)
        goto done;
    pthread_t reaper;
    if (pthread_create(&reaper, NULL, reap_persistent_worker,
                        (void *)(intptr_t)worker) != 0)
        goto done;
    (void)pthread_detach(reaper);
    worker = -1;
    kept = sockets[0];
    sockets[0] = -1;

done:
    if (worker > 0)
        terminate_worker(worker);
    if (child_socket >= 0)
        close(child_socket);
    if (child_executable >= 0)
        close(child_executable);
    free(groups);
    if (executable >= 0)
        close(executable);
    if (capture_pipe[0] >= 0)
        close(capture_pipe[0]);
    if (capture_pipe[1] >= 0)
        close(capture_pipe[1]);
    if (sockets[0] >= 0)
        close(sockets[0]);
    if (sockets[1] >= 0)
        close(sockets[1]);
    return kept;
}

void ksd_capture_worker_execute(const ksp_identity *identity, gid_t gid,
                                const ksd_frame *request,
                                ksd_capture_worker_continue_fn keep_running,
                                void *user_data, pid_t session_pid,
                                uint32_t backend,
                                ksd_operation_result *result)
{
    worker_environment environment = { 0 };
    gid_t *groups = NULL;
    size_t group_count = 0u;
    int executable = -1;
    int sockets[2] = { -1, -1 };
    int capture_pipe[2] = { -1, -1 };
    pid_t worker = -1;
    bool response_received = false;

    if (result == NULL)
        return;
    ksd_result_init(result);
    /* The parent must admit exactly what the child will run. It used to admit
     * only captures, while the child accepts captures, X11 verbs and Wayland
     * verbs -- so every non-capture verb was refused here, before the fork,
     * with a message about the capture worker. The X11 coordinate group had
     * been unreachable that way since it landed, and no test saw it because
     * every test calls the backend functions directly rather than through this
     * path. The two admission sets are now one expression, and a gate pins
     * them equal. */
    if (identity == NULL || request == NULL || keep_running == NULL
        || !ksd_capture_worker_request_valid(request)
        || !capture_environment(identity, &environment)
        || !capture_groups(identity, &groups, &group_count)) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                         "capture worker identity is unavailable");
        goto done;
    }
    executable = open(KSD_CAPTURE_WORKER_PATH,
                      O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (!trusted_self(executable)
        || socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC,
                      0, sockets) != 0
        || !create_capture_pipe(capture_pipe)
        || !set_socket_timeout(sockets[0], KSD_CAPTURE_WORKER_TIMEOUT_MS)
        || !set_socket_timeout(sockets[1], KSD_CAPTURE_WORKER_TIMEOUT_MS)) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                         "capture worker could not be started");
        goto done;
    }
    int child_socket = fcntl(sockets[1], F_DUPFD_CLOEXEC, 10);
    int child_executable = fcntl(executable, F_DUPFD_CLOEXEC, 10);
    if (child_socket < 0 || child_executable < 0) {
        if (child_socket >= 0) close(child_socket);
        if (child_executable >= 0) close(child_executable);
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                         "capture worker descriptors are unavailable");
        goto done;
    }
    worker = fork();
    if (worker == 0) {
        if (dup2(child_socket, KSD_CAPTURE_WORKER_FD) < 0
            || dup2(child_executable, 4) < 0
            || close_range(5u, UINT_MAX, 0) != 0)
            _exit(126);
        char *worker_argv[] = { KSD_CAPTURE_WORKER_PATH, NULL };
        fexecve(4, worker_argv, environment.values);
        _exit(127);
    }
    close(child_socket);
    close(child_executable);
    close(sockets[1]);
    sockets[1] = -1;
    if (worker < 0 || !send_bootstrap(sockets[0], identity, gid,
                                      groups, group_count, request,
                                      capture_pipe, session_pid,
                                      backend, false)) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                         "capture worker bootstrap failed");
        goto done;
    }
    close(capture_pipe[0]);
    close(capture_pipe[1]);
    capture_pipe[0] = -1;
    capture_pipe[1] = -1;
    (void)shutdown(sockets[0], SHUT_WR);
    uint64_t deadline = monotonic_milliseconds()
        + KSD_CAPTURE_WORKER_TIMEOUT_MS;
    for (;;) {
        if (!keep_running(user_data)) {
            ksd_result_error(result, KSD_STATUS_REVOKED, 0u,
                             "permission changed during capture");
            goto done;
        }
        uint64_t now = monotonic_milliseconds();
        if (now == 0u || now >= deadline) {
            ksd_result_error(result, KSD_STATUS_TIMEOUT, 0u,
                             "capture worker timed out");
            goto done;
        }
        int wait_ms = (int)(deadline - now > 250u
            ? 250u : deadline - now);
        struct pollfd item = {
            .fd = sockets[0],
            .events = POLLIN | POLLRDHUP | POLLHUP | POLLERR,
        };
        int ready = poll(&item, 1u, wait_ms);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0 || (ready > 0
            && (item.revents & (POLLERR | POLLNVAL)) != 0)) {
            ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                             "capture worker transport failed");
            goto done;
        }
        if (ready == 0)
            continue;
        if ((item.revents & POLLIN) == 0) {
            ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                             "capture worker exited without a result");
            goto done;
        }
        if (!receive_worker_response(sockets[0], result)) {
            ksd_result_error(result, KSD_STATUS_INTERNAL, 0u,
                             "capture worker returned an invalid result");
            goto done;
        }
        response_received = true;
        break;
    }

done:
    if (sockets[0] >= 0)
        close(sockets[0]);
    if (sockets[1] >= 0)
        close(sockets[1]);
    if (capture_pipe[0] >= 0)
        close(capture_pipe[0]);
    if (capture_pipe[1] >= 0)
        close(capture_pipe[1]);
    if (executable >= 0)
        close(executable);
    free(groups);
    if (worker > 0) {
        int status;
        pid_t waited;
        do {
            waited = waitpid(worker, &status, WNOHANG);
        } while (waited < 0 && errno == EINTR);
        if (waited == 0) {
            terminate_worker(worker);
            if (!response_received)
                ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                                 "capture worker did not terminate");
        } else if (waited != worker || !WIFEXITED(status)
                 || WEXITSTATUS(status) != 0)
            ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                             "capture worker terminated unexpectedly");
    }
}

/* The far end has to be the authority that forked this worker, which means the
 * party this installation belongs to -- root for a system installation, and
 * the one user for a user one. */
static bool worker_peer_is_authority(int descriptor)
{
    struct ucred peer;
    socklen_t length = sizeof(peer);
    return getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED,
                      &peer, &length) == 0
        && length == sizeof(peer)
        && ksd_install_owner_trusted(peer.uid);
}

static bool drop_worker_privileges(uid_t uid, gid_t gid,
                                   const gid_t *groups, size_t group_count)
{
    /* There is nothing to drop to in a user installation: the worker was
     * forked by an authority that is already the client's uid, and the calls
     * below need privileges it does not have. What follows the drop still
     * applies, and the credentials are still checked at the end -- the check
     * is what the caller relies on, not the calls that usually cause it to
     * pass. Refusing to become root covers both: a user installation whose
     * owner were root would be a system one. */
    bool already = uid == getuid() && uid == geteuid()
        && gid == getgid() && gid == getegid();

    if (uid == 0u || prctl(PR_SET_KEEPCAPS, 0) != 0
        || (!already && setgroups(group_count, groups) != 0)
        || (!already && setresgid(gid, gid, gid) != 0)
        || (!already && setresuid(uid, uid, uid) != 0)
        || prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0
        || prctl(PR_SET_DUMPABLE, 0) != 0)
        return false;
    if (getppid() == 1)
        return false;
    uid_t real_uid;
    uid_t effective_uid;
    uid_t saved_uid;
    gid_t real_gid;
    gid_t effective_gid;
    gid_t saved_gid;
    return getresuid(&real_uid, &effective_uid, &saved_uid) == 0
        && getresgid(&real_gid, &effective_gid, &saved_gid) == 0
        && real_uid == uid && effective_uid == uid && saved_uid == uid
        && real_gid == gid && effective_gid == gid && saved_gid == gid;
}

static bool resolve_trusted_kwin_pid(uid_t uid, gid_t gid,
                                    const gid_t *groups, size_t group_count,
                                    pid_t *trusted_pid)
{
    int descriptors[2];
    if (trusted_pid == NULL || pipe2(descriptors, O_CLOEXEC) != 0)
        return false;
    pid_t child = fork();
    if (child == 0) {
        close(descriptors[0]);
        if (dup2(descriptors[1], 3) < 0
            || close_range(4u, UINT_MAX, 0) != 0
            || !drop_worker_privileges(uid, gid, groups, group_count))
            _exit(1);
        pid_t owner = 0;
        if (!ksd_local_capture_kwin_owner_pid(uid, &owner))
            _exit(1);
        ssize_t written;
        do {
            written = write(3, &owner, sizeof(owner));
        } while (written < 0 && errno == EINTR);
        close(3);
        _exit(written == (ssize_t)sizeof(owner) ? 0 : 1);
    }
    close(descriptors[1]);
    if (child < 0) {
        close(descriptors[0]);
        return false;
    }
    pid_t owner = 0;
    bool read_owner = ksd_read_all(descriptors[0], &owner, sizeof(owner));
    close(descriptors[0]);
    int status;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    bool valid = read_owner && waited == child && WIFEXITED(status)
        && WEXITSTATUS(status) == 0
        && ksd_local_capture_kwin_process_trusted(uid, owner);
    if (valid)
        *trusted_pid = owner;
    return valid;
}

static bool prepare_worker_root(bool persistent)
{
    const struct rlimit no_core = { .rlim_cur = 0u, .rlim_max = 0u };
    const struct rlimit memory = {
        .rlim_cur = KSD_CAPTURE_WORKER_MEMORY_LIMIT,
        .rlim_max = KSD_CAPTURE_WORKER_MEMORY_LIMIT,
    };
    const struct rlimit file_size = {
        .rlim_cur = KSD_MAX_CAPTURE_BYTES + 4096u,
        .rlim_max = KSD_MAX_CAPTURE_BYTES + 4096u,
    };
    const struct rlimit open_files = { .rlim_cur = 64u, .rlim_max = 64u };
    const struct rlimit cpu = { .rlim_cur = 40u, .rlim_max = 40u };
    return prctl(PR_SET_PDEATHSIG, persistent ? 0 : SIGKILL) == 0
        && prctl(PR_SET_DUMPABLE, 0) == 0 && getppid() != 1
        && setrlimit(RLIMIT_CORE, &no_core) == 0
        && setrlimit(RLIMIT_AS, &memory) == 0
        && setrlimit(RLIMIT_FSIZE, &file_size) == 0
        && setrlimit(RLIMIT_NOFILE, &open_files) == 0
        && (persistent || setrlimit(RLIMIT_CPU, &cpu) == 0);
}

static ssize_t receive_bootstrap(int descriptor, uint8_t *buffer,
                                 size_t capacity, int capture_pipe[2])
{
    uint8_t control[CMSG_SPACE(sizeof(int) * 4u)] = { 0 };
    struct iovec iov = { .iov_base = buffer, .iov_len = capacity };
    struct msghdr message = {
        .msg_iov = &iov,
        .msg_iovlen = 1u,
        .msg_control = control,
        .msg_controllen = sizeof(control),
    };
    ssize_t count;
    size_t fd_count = 0u;
    bool malformed = false;
    do {
        count = recvmsg(descriptor, &message, MSG_CMSG_CLOEXEC);
    } while (count < 0 && errno == EINTR);
    for (struct cmsghdr *header = CMSG_FIRSTHDR(&message);
         header != NULL; header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level != SOL_SOCKET
            || header->cmsg_type != SCM_RIGHTS
            || header->cmsg_len < CMSG_LEN(0u)) {
            malformed = true;
            continue;
        }
        size_t payload = header->cmsg_len - CMSG_LEN(0u);
        if (payload % sizeof(int) != 0u) {
            malformed = true;
            continue;
        }
        for (size_t offset = 0u; offset < payload; offset += sizeof(int)) {
            int value;
            memcpy(&value, CMSG_DATA(header) + offset, sizeof(value));
            if (fd_count < 2u)
                capture_pipe[fd_count] = value;
            else
                close(value);
            fd_count++;
        }
    }
    int socket_type = 0;
    socklen_t socket_type_size = sizeof(socket_type);
    if (count > 0 && getsockopt(descriptor, SOL_SOCKET, SO_TYPE, &socket_type,
                                &socket_type_size) == 0
        && socket_type == SOCK_STREAM) {
        if ((size_t)count < KSD_CAPTURE_WORKER_HEADER_SIZE) {
            size_t missing = KSD_CAPTURE_WORKER_HEADER_SIZE - (size_t)count;
            if (!ksd_read_all(descriptor, buffer + count, missing))
                malformed = true;
            else
                count += (ssize_t)missing;
        }
        if (!malformed) {
            uint64_t expected = KSD_CAPTURE_WORKER_HEADER_SIZE
                + (uint64_t)ksd_decode_u32(buffer + 16u) * sizeof(uint32_t)
                + ksd_decode_u32(buffer + 20u);
            if (expected > capacity || expected < (uint64_t)count)
                malformed = true;
            else if (expected > (uint64_t)count) {
                if (!ksd_read_all(descriptor, buffer + count,
                                   (size_t)(expected - (uint64_t)count)))
                    malformed = true;
                else
                    count = (ssize_t)expected;
            }
        }
    }
    if (count <= 0 || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0
        || malformed || fd_count != 2u
        || !ksd_capture_pipe_valid(capture_pipe)) {
        if (capture_pipe[0] >= 0)
            close(capture_pipe[0]);
        if (capture_pipe[1] >= 0)
            close(capture_pipe[1]);
        capture_pipe[0] = -1;
        capture_pipe[1] = -1;
        return -1;
    }
    return count;
}

typedef struct worker_bootstrap {
    uid_t uid;
    gid_t gid;
    uint32_t group_count;
    uint32_t frame_length;
    pid_t session_pid;
    /* Which backend the authority resolved. The worker cannot infer this: an
     * area capture is byte-identical whichever backend will serve it. */
    uint32_t backend;
    /* Whether to serve until the socket closes rather than exit after one
     * request. X11 and Wayland captures share the persistent display. */
    bool persistent;
} worker_bootstrap;

/* The whole header is validated before any field is used, because a version
 * that is merely tolerated rather than required would let an older layout be
 * read with this one's offsets: the pid field would land where the group
 * array begins. */
static bool parse_bootstrap_header(const uint8_t *bootstrap, size_t length,
                                   worker_bootstrap *parsed)
{
    if (length < KSD_CAPTURE_WORKER_HEADER_SIZE
        || memcmp(bootstrap, worker_magic, sizeof(worker_magic)) != 0
        || ksd_decode_u16(bootstrap + 4u) != KSD_CAPTURE_WORKER_VERSION
        || ksd_decode_u16(bootstrap + 6u) > KSD_BACKEND_X11
        || ksd_decode_u32(bootstrap + 28u) > 1u)
        return false;
    parsed->uid = (uid_t)ksd_decode_u32(bootstrap + 8u);
    parsed->gid = (gid_t)ksd_decode_u32(bootstrap + 12u);
    parsed->group_count = ksd_decode_u32(bootstrap + 16u);
    parsed->frame_length = ksd_decode_u32(bootstrap + 20u);
    parsed->session_pid = (pid_t)ksd_decode_u32(bootstrap + 24u);
    parsed->backend = ksd_decode_u16(bootstrap + 6u);
    parsed->persistent = ksd_decode_u32(bootstrap + 28u) != 0u;
    size_t groups_length = (size_t)parsed->group_count * sizeof(uint32_t);
    return (uint64_t)parsed->uid == ksd_decode_u32(bootstrap + 8u)
        && (uint64_t)parsed->gid == ksd_decode_u32(bootstrap + 12u)
        && parsed->session_pid > 0
        && (uint64_t)parsed->session_pid == ksd_decode_u32(bootstrap + 24u)
        && parsed->group_count <= KSD_CAPTURE_WORKER_MAX_GROUPS
        /* A persistent worker is started with no request: it exists to serve
         * the ones that follow. A one-shot worker must carry exactly one. */
        && (parsed->persistent
            ? parsed->frame_length == 0u
            : parsed->frame_length >= KSD_FRAME_HEADER_SIZE)
        && parsed->frame_length <= KSD_FRAME_HEADER_SIZE
            + KSD_CAPTURE_WORKER_MAX_REQUEST
        && KSD_CAPTURE_WORKER_HEADER_SIZE + groups_length
            + parsed->frame_length == length;
}

#ifdef KSD_CAPTURE_WORKER_TESTING
bool ksd_capture_worker_test_parse_header(const uint8_t *bootstrap,
                                          size_t length)
{
    worker_bootstrap parsed;
    return parse_bootstrap_header(bootstrap, length, &parsed);
}
#endif

/* One request in, one answer out, until the far end closes.
 *
 * The connection is opened lazily and reopened when it fails, rather than at
 * start: a worker that could not connect at start would be useless for the
 * whole session, while one that reconnects survives a compositor restart. A
 * failed connection is told apart from a failed operation by the execute_on
 * return value, which is why that distinction exists.
 */
#ifdef KSD_CAPTURE_WORKER_TESTING
/* Counts the display connections the persistent server opens. The whole point
 * of a persistent worker is that this stays at one however many requests
 * arrive, and that property is invisible from outside the process: the answers
 * are identical whether the connection was reused or reopened per request, so
 * only a count can tell the two apart. */
unsigned ksd_capture_worker_test_opens;
#define KSD_NOTE_DISPLAY_OPEN() (ksd_capture_worker_test_opens++)
#else
#define KSD_NOTE_DISPLAY_OPEN() ((void)0)
#endif

static bool serve_persistently(uint32_t backend, pid_t session_pid)
{
    struct ksd_x11 *x11_connection = NULL;
    struct ksd_wayland *wayland_connection = NULL;
    bool ok = true;

    for (;;) {
        uint8_t header[KSD_FRAME_HEADER_SIZE];
        uint8_t *body = NULL;
        uint32_t payload_length;
        ksd_frame request;
        ksd_operation_result result;
        ksd_buffer packed;
        ksd_frame answer;
        ksd_buffer payload;
        bool alive = true;

        if (!ksd_read_all(KSD_CAPTURE_WORKER_FD, header, sizeof(header)))
            break;
        payload_length =
            ksd_decode_u32(header + KSD_FRAME_PAYLOAD_LENGTH_OFFSET);
        if (payload_length > KSD_CAPTURE_WORKER_MAX_REQUEST
            || memcmp(header, public_magic, sizeof(public_magic)) != 0
            || ksd_decode_u16(header + KSD_FRAME_MAJOR_OFFSET)
                != KSD_PROTOCOL_MAJOR
            || ksd_decode_u16(header + KSD_FRAME_MINOR_OFFSET)
                != KSD_PROTOCOL_MINOR
            || ksd_decode_u16(header + KSD_FRAME_FLAGS_OFFSET) != 0u
            || ksd_decode_u64(header + KSD_FRAME_REQUEST_ID_OFFSET) == 0u) {
            ok = false;
            break;
        }
        if (payload_length != 0u) {
            body = malloc(payload_length);
            if (body == NULL
                || !ksd_read_all(KSD_CAPTURE_WORKER_FD, body,
                                 payload_length)) {
                free(body);
                ok = false;
                break;
            }
        }
        memset(&request, 0, sizeof(request));
        request.opcode = ksd_decode_u16(header + KSD_FRAME_OPCODE_OFFSET);
        request.request_id =
            ksd_decode_u64(header + KSD_FRAME_REQUEST_ID_OFFSET);
        request.payload = body;
        request.payload_length = payload_length;

        ksd_result_init(&result);
        bool capture = request.opcode == KSD_OP_CAPTURE_AREA
            || request.opcode == KSD_OP_CAPTURE_WINDOW
            || request.opcode == KSD_OP_CAPTURE_DESKTOP;
        /* Display libraries can block inside a synchronous reply even after
         * the authority closes our socket. The process deadline bounds that
         * work without limiting an idle worker's lifetime. */
        (void)alarm(capture ? 40u : 10u);
        if (backend == KSD_BACKEND_X11) {
            if (x11_connection == NULL) {
                ksd_status status = ksd_x11_open_for_session(session_pid,
                                                             &x11_connection);
                if (status == KSD_STATUS_OK)
                    KSD_NOTE_DISPLAY_OPEN();
                else
                    ksd_result_error(&result, status, 0u,
                                     "could not open the X display for this "
                                     "session");
            }
            if (x11_connection != NULL
                && request.opcode == KSD_OP_WINDOW_WATCH
                && request.payload_length == 0u) {
                ok = ksd_x11_watch_run(x11_connection, KSD_CAPTURE_WORKER_FD,
                                       request.request_id);
                (void)alarm(0u);
                ksd_result_clear(&result);
                free(body);
                break;
            }
            if (x11_connection != NULL)
                alive = ksd_x11_execute_on(x11_connection, &request, &result);
        } else {
            if (wayland_connection == NULL) {
                ksd_status status =
                    ksd_wayland_open_for_session(session_pid,
                                                 &wayland_connection);
                if (status == KSD_STATUS_OK)
                    KSD_NOTE_DISPLAY_OPEN();
                else
                    ksd_result_error(&result, status, 0u,
                                     "could not reach the compositor for this "
                                     "session");
            }
            if (wayland_connection != NULL)
                alive = ksd_wayland_execute_on(wayland_connection, &request,
                                               &result);
        }
        (void)alarm(0u);
        /* A dead connection is dropped so the NEXT request reopens. The
         * current answer still goes back: the caller asked one question and
         * gets one answer, rather than silence because the display chose that
         * moment to go. */
        if (!alive) {
            if (x11_connection != NULL) {
                ksd_x11_close(x11_connection);
                x11_connection = NULL;
            }
            if (wayland_connection != NULL) {
                ksd_wayland_close(wayland_connection);
                wayland_connection = NULL;
            }
        }

        bool capture_fd = result.status == KSD_STATUS_OK
            && result.payload_fd >= 0;
        ksd_buffer_init(&payload, capture_fd ? 12u : result.tail_length + 8u);
        ok = ksd_buffer_u32(&payload, result.status)
            && ksd_buffer_u32(&payload, result.detail)
            && (capture_fd ? ksd_buffer_u32(&payload, result.tail_length)
                : result.tail_length == 0u
                || ksd_buffer_bytes(&payload, result.tail,
                                    result.tail_length));
        memset(&answer, 0, sizeof(answer));
        answer.magic[0] = KSD_FRAME_MAGIC_0;
        answer.magic[1] = KSD_FRAME_MAGIC_1;
        answer.magic[2] = KSD_FRAME_MAGIC_2;
        answer.magic[3] = KSD_FRAME_MAGIC_3;
        answer.major = KSD_PROTOCOL_MAJOR;
        answer.minor = KSD_PROTOCOL_MINOR;
        answer.flags = capture_fd ? KSD_RELAY_CAPTURE_FD : 0u;
        answer.request_id = request.request_id;
        answer.payload = payload.data;
        answer.payload_length = (uint32_t)payload.length;
        ksd_buffer_init(&packed, payload.length + KSD_FRAME_HEADER_SIZE + 16u);
        ok = ok && ksd_frame_pack(&answer, &packed)
            && (capture_fd
                ? ksd_send_with_fd(KSD_CAPTURE_WORKER_FD, packed.data,
                                    packed.length, result.payload_fd)
                : ksd_write_all(KSD_CAPTURE_WORKER_FD, packed.data,
                                 packed.length));
        ksd_buffer_clear(&packed);
        ksd_buffer_clear(&payload);
        ksd_result_clear(&result);
        free(body);
        if (!ok)
            break;
    }
    if (x11_connection != NULL)
        ksd_x11_close(x11_connection);
    if (wayland_connection != NULL)
        ksd_wayland_close(wayland_connection);
    return ok;
}

#ifdef KSD_CAPTURE_WORKER_TESTING
bool ksd_capture_worker_test_serve(uint32_t backend, pid_t session_pid)
{
    return serve_persistently(backend, session_pid);
}
#endif

int ksd_capture_worker_main(int argc, char **argv)
{
    uint8_t bootstrap[KSD_CAPTURE_WORKER_BOOTSTRAP_MAX];
    gid_t groups[KSD_CAPTURE_WORKER_MAX_GROUPS];
    ksd_frame request;
    ksd_operation_result result;
    int capture_pipe[2] = { -1, -1 };
    int capture_spool = -1;
    if (argc != 1 || argv == NULL || argv[0] == NULL)
        return 2;
    /* Whole credentials, and an authority on the other end that shares
     * them. A worker whose real and effective ids differ is partway
     * through a setuid it did not make, and the peer check is what says
     * this process was forked by the authority rather than started by
     * somebody who found the binary. */
    if (getuid() != geteuid() || getgid() != getegid()
        || !worker_peer_is_authority(KSD_CAPTURE_WORKER_FD))
        return 1;
    if (close_range(4u, UINT_MAX, 0) != 0)
        return 1;
    ssize_t bootstrap_length = receive_bootstrap(KSD_CAPTURE_WORKER_FD,
                                                  bootstrap,
                                                  sizeof(bootstrap),
                                                  capture_pipe);
    worker_bootstrap header;
    if (bootstrap_length < 0
        || !parse_bootstrap_header(bootstrap, (size_t)bootstrap_length,
                                   &header)
        || !prepare_worker_root(header.persistent))
        goto failed;
    uid_t uid = header.uid;
    gid_t gid = header.gid;
    uint32_t group_count = header.group_count;
    uint32_t frame_length = header.frame_length;
    pid_t trusted_kwin_pid = 0;
    size_t groups_length = (size_t)group_count * sizeof(uint32_t);
    for (uint32_t index = 0u; index < group_count; index++) {
        uint32_t value = ksd_decode_u32(bootstrap
            + KSD_CAPTURE_WORKER_HEADER_SIZE + index * sizeof(uint32_t));
        groups[index] = (gid_t)value;
        if ((uint64_t)groups[index] != value)
            goto failed;
    }
    if (!header.persistent && header.backend == KSD_BACKEND_KWIN
        && !resolve_trusted_kwin_pid(uid, gid, groups, group_count,
                                    &trusted_kwin_pid))
        goto failed;
    capture_spool = create_capture_spool();
    if (capture_spool < 0
        || !drop_worker_privileges(uid, gid, groups, group_count))
        goto failed;
    if (header.persistent) {
        /* No initial request to unpack. Everything this worker will ever do
         * arrives on the socket, and the pipe and spool a capture needs are
         * deliberately not held open across a session. */
        close(capture_pipe[0]);
        close(capture_pipe[1]);
        close(capture_spool);
        capture_pipe[0] = -1;
        capture_pipe[1] = -1;
        capture_spool = -1;
        struct timeval unlimited = { 0 };
        uint8_t ready = 1u;
        pthread_t monitor;
        /* A dedicated hangup observer stops blocked display-library calls as
         * soon as their authority goes away. It never reads request bytes. */
        if (pthread_create(&monitor, NULL, watch_authority_connection,
                            (void *)(intptr_t)KSD_CAPTURE_WORKER_FD) != 0)
            goto failed;
        (void)pthread_detach(monitor);
        if (setsockopt(KSD_CAPTURE_WORKER_FD, SOL_SOCKET, SO_RCVTIMEO,
                        &unlimited, sizeof(unlimited)) != 0
            || !ksd_write_all(KSD_CAPTURE_WORKER_FD, &ready, sizeof(ready)))
            goto failed;
        bool served = serve_persistently(header.backend, header.session_pid);
        close(KSD_CAPTURE_WORKER_FD);
        return served ? 0 : 1;
    }
    const uint8_t *packed = bootstrap + KSD_CAPTURE_WORKER_HEADER_SIZE
        + groups_length;
    bool unpacked = ksd_frame_unpack(packed, frame_length, public_magic,
        KSD_PROTOCOL_MAJOR, KSD_PROTOCOL_MINOR,
        KSD_CAPTURE_WORKER_MAX_REQUEST, true, &request);
    /* Routed by the BACKEND the authority resolved, not by the shape of the
     * payload. Shape cannot tell these apart: an area capture is the same
     * sixteen bytes whoever will serve it, so guessing sent every KWin capture
     * down the X11 path, where it met an XWayland server and was refused. The
     * backend is the only thing that actually decides, and it now travels. */
    bool x11 = unpacked && header.backend == KSD_BACKEND_X11
        && ksd_x11_request_valid(&request);
    bool wayland = unpacked && header.backend == KSD_BACKEND_GENERIC
        && ksd_wayland_request_valid(&request);
    if (!unpacked || request.flags != 0u || request.request_id == 0u
        || !(x11 || wayland || ksd_local_capture_request_valid(&request))) {
        if (unpacked)
            ksd_frame_clear(&request);
        goto failed;
    }
    if (x11 || wayland) {
        /* Neither display backend needs the capture pipe or the spool, and
         * holding them open across a round trip would keep descriptors the
         * operation cannot use. */
        close(capture_pipe[0]);
        close(capture_pipe[1]);
        close(capture_spool);
        if (x11)
            ksd_x11_execute(&request, header.session_pid, &result);
        else
            ksd_wayland_execute(&request, header.session_pid, &result);
    } else {
        ksd_local_capture_execute(&request, capture_pipe[0], capture_pipe[1],
                                  capture_spool, trusted_kwin_pid, &result);
    }
    capture_pipe[0] = -1;
    capture_pipe[1] = -1;
    capture_spool = -1;
    bool written = send_worker_response(KSD_CAPTURE_WORKER_FD, &result);
    ksd_result_clear(&result);
    ksd_frame_clear(&request);
    close(KSD_CAPTURE_WORKER_FD);
    return written ? 0 : 1;

failed:
    if (capture_pipe[0] >= 0)
        close(capture_pipe[0]);
    if (capture_pipe[1] >= 0)
        close(capture_pipe[1]);
    if (capture_spool >= 0)
        close(capture_spool);
    return 1;
}
