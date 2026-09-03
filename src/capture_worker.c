#include "capture_worker.h"

#include "protocol.h"
#include "local_capture.h"
#include "transport.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <linux/memfd.h>
#include <poll.h>
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
#define KSD_CAPTURE_WORKER_VERSION 2u
#define KSD_CAPTURE_WORKER_HEADER_SIZE 32u
#define KSD_CAPTURE_WORKER_MAX_GROUPS 256u
#define KSD_CAPTURE_WORKER_ENV_LIMIT (64u * 1024u)
#define KSD_CAPTURE_WORKER_TIMEOUT_MS 35000u
#define KSD_CAPTURE_WORKER_MAX_REQUEST (8u + 128u)
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

static bool trusted_self(int descriptor)
{
    struct stat status;
    return descriptor >= 0 && fstat(descriptor, &status) == 0
        && S_ISREG(status.st_mode) && status.st_uid == 0u
        && (status.st_mode & 0777u) == 0700u;
}

static bool create_capture_pipe(int descriptors[2])
{
    if (geteuid() != 0u || pipe2(descriptors, O_CLOEXEC) != 0)
        return false;
    int read_flags = fcntl(descriptors[0], F_GETFL);
    if (fchown(descriptors[0], 0u, 0u) != 0
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
    if (descriptor < 0 || fchown(descriptor, 0u, 0u) != 0
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
                           const int capture_pipe[2], pid_t session_pid)
{
    ksd_buffer frame;
    ksd_buffer message;
    ksd_buffer_init(&frame, KSD_FRAME_HEADER_SIZE
                              + KSD_CAPTURE_WORKER_MAX_REQUEST);
    ksd_buffer_init(&message, KSD_CAPTURE_WORKER_BOOTSTRAP_MAX);
    bool ok = group_count <= KSD_CAPTURE_WORKER_MAX_GROUPS
        && request->payload_length <= KSD_CAPTURE_WORKER_MAX_REQUEST
        && ksd_frame_pack(request, &frame) && frame.length <= UINT32_MAX
        && ksd_buffer_bytes(&message, worker_magic, sizeof(worker_magic))
        && ksd_buffer_u16(&message, KSD_CAPTURE_WORKER_VERSION)
        && ksd_buffer_u16(&message, 0u)
        && ksd_buffer_u32(&message, (uint32_t)identity->uid)
        && ksd_buffer_u32(&message, (uint32_t)gid)
        && ksd_buffer_u32(&message, (uint32_t)group_count)
        && ksd_buffer_u32(&message, (uint32_t)frame.length)
        && ksd_buffer_u32(&message, (uint32_t)session_pid)
        && ksd_buffer_u32(&message, 0u);
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
    int payload_fd = memfd_create("keysharp-desktop-capture",
                                  MFD_CLOEXEC | MFD_ALLOW_SEALING);
    int seals = F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
    bool written = payload_fd >= 0 && result->tail != NULL
        && result->tail_length != 0u
        && result->tail_length <= KSD_MAX_CAPTURE_TAIL
        && ksd_write_all(payload_fd, result->tail, result->tail_length)
        && fcntl(payload_fd, F_ADD_SEALS, seals) == 0
        && ksd_send_with_fd(descriptor, message, sizeof(message), payload_fd);
    if (payload_fd >= 0)
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

void ksd_capture_worker_execute(const ksp_identity *identity, gid_t gid,
                                const ksd_frame *request,
                                ksd_capture_worker_continue_fn keep_running,
                                void *user_data, pid_t session_pid,
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
    if (identity == NULL || request == NULL || keep_running == NULL
        || !ksd_local_capture_request_valid(request)
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
                                      capture_pipe, session_pid)) {
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

static bool worker_peer_is_root(int descriptor)
{
    struct ucred peer;
    socklen_t length = sizeof(peer);
    return getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED,
                      &peer, &length) == 0
        && length == sizeof(peer) && peer.uid == 0u;
}

static bool drop_worker_privileges(uid_t uid, gid_t gid,
                                   const gid_t *groups, size_t group_count)
{
    if (uid == 0u || prctl(PR_SET_KEEPCAPS, 0) != 0
        || setgroups(group_count, groups) != 0
        || setresgid(gid, gid, gid) != 0
        || setresuid(uid, uid, uid) != 0
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

static bool prepare_worker_root(void)
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
    return prctl(PR_SET_PDEATHSIG, SIGKILL) == 0
        && prctl(PR_SET_DUMPABLE, 0) == 0 && getppid() != 1
        && setrlimit(RLIMIT_CORE, &no_core) == 0
        && setrlimit(RLIMIT_AS, &memory) == 0
        && setrlimit(RLIMIT_FSIZE, &file_size) == 0
        && setrlimit(RLIMIT_NOFILE, &open_files) == 0
        && setrlimit(RLIMIT_CPU, &cpu) == 0;
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
        || ksd_decode_u16(bootstrap + 6u) != 0u
        || ksd_decode_u32(bootstrap + 28u) != 0u)
        return false;
    parsed->uid = (uid_t)ksd_decode_u32(bootstrap + 8u);
    parsed->gid = (gid_t)ksd_decode_u32(bootstrap + 12u);
    parsed->group_count = ksd_decode_u32(bootstrap + 16u);
    parsed->frame_length = ksd_decode_u32(bootstrap + 20u);
    parsed->session_pid = (pid_t)ksd_decode_u32(bootstrap + 24u);
    size_t groups_length = (size_t)parsed->group_count * sizeof(uint32_t);
    return (uint64_t)parsed->uid == ksd_decode_u32(bootstrap + 8u)
        && (uint64_t)parsed->gid == ksd_decode_u32(bootstrap + 12u)
        && parsed->session_pid > 0
        && (uint64_t)parsed->session_pid == ksd_decode_u32(bootstrap + 24u)
        && parsed->group_count <= KSD_CAPTURE_WORKER_MAX_GROUPS
        && parsed->frame_length >= KSD_FRAME_HEADER_SIZE
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
    if (getuid() != 0u || geteuid() != 0u
        || !worker_peer_is_root(KSD_CAPTURE_WORKER_FD))
        return 1;
    if (close_range(4u, UINT_MAX, 0) != 0 || !prepare_worker_root())
        return 1;
    ssize_t bootstrap_length = receive_bootstrap(KSD_CAPTURE_WORKER_FD,
                                                  bootstrap,
                                                  sizeof(bootstrap),
                                                  capture_pipe);
    worker_bootstrap header;
    if (bootstrap_length < 0
        || !parse_bootstrap_header(bootstrap, (size_t)bootstrap_length,
                                   &header))
        goto failed;
    uid_t uid = header.uid;
    gid_t gid = header.gid;
    uint32_t group_count = header.group_count;
    uint32_t frame_length = header.frame_length;
    size_t groups_length = (size_t)group_count * sizeof(uint32_t);
    for (uint32_t index = 0u; index < group_count; index++) {
        uint32_t value = ksd_decode_u32(bootstrap
            + KSD_CAPTURE_WORKER_HEADER_SIZE + index * sizeof(uint32_t));
        groups[index] = (gid_t)value;
        if ((uint64_t)groups[index] != value)
            goto failed;
    }
    capture_spool = create_capture_spool();
    if (capture_spool < 0
        || !drop_worker_privileges(uid, gid, groups, group_count))
        goto failed;
    const uint8_t *packed = bootstrap + KSD_CAPTURE_WORKER_HEADER_SIZE
        + groups_length;
    bool unpacked = ksd_frame_unpack(packed, frame_length, public_magic,
        KSD_PROTOCOL_MAJOR, KSD_PROTOCOL_MINOR,
        KSD_CAPTURE_WORKER_MAX_REQUEST, true, &request);
    if (!unpacked || request.flags != 0u || request.request_id == 0u
        || !ksd_local_capture_request_valid(&request)) {
        if (unpacked)
            ksd_frame_clear(&request);
        goto failed;
    }
    ksd_local_capture_execute(&request, capture_pipe[0], capture_pipe[1],
                              capture_spool, &result);
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
