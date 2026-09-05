#include "install_mode.h"
#include "local_capture.h"
#include "operation_result.h"
#include "protocol.h"
#include "protocol_io.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/memfd.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static uint64_t deadline_after(uint32_t milliseconds)
{
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (uint64_t)now.tv_sec * 1000u
        + (uint64_t)now.tv_nsec / 1000000u + milliseconds;
}

static void make_capture_pipe(int descriptors[2])
{
    assert(pipe2(descriptors, O_CLOEXEC) == 0);
    assert(fchown(descriptors[0], 0u, 0u) == 0);
    assert(fchmod(descriptors[0], 0u) == 0);
    int read_flags = fcntl(descriptors[0], F_GETFL);
    assert(read_flags >= 0);
    assert(fcntl(descriptors[0], F_SETFL,
                 read_flags | O_NONBLOCK) == 0);
    assert(ksd_capture_pipe_valid(descriptors));
}

static int make_capture_spool(void)
{
    int descriptor = memfd_create("capture-spool-test",
                                  MFD_CLOEXEC | MFD_ALLOW_SEALING);
    assert(descriptor >= 0);
    assert(fchown(descriptor, 0u, 0u) == 0);
    assert(fchmod(descriptor, 0u) == 0);
    assert(ksd_capture_spool_valid(descriptor));
    return descriptor;
}

static void verify_large_pipe_drain(void)
{
    int capture[2];
    make_capture_pipe(capture);
    int spool = make_capture_spool();
    int pipe_capacity = fcntl(capture[0], F_GETPIPE_SZ);
    assert(pipe_capacity > 0);
    uint32_t byte_count = (uint32_t)pipe_capacity * 4u;
    assert(byte_count > (uint32_t)pipe_capacity
        && byte_count < KSD_MAX_CAPTURE_BYTES);

    pid_t writer = fork();
    assert(writer >= 0);
    if (writer == 0) {
        close(capture[0]);
        close(spool);
        uint8_t bytes[65536];
        memset(bytes, 0x5au, sizeof(bytes));
        uint32_t remaining = byte_count;
        while (remaining != 0u) {
            size_t requested = remaining < sizeof(bytes)
                ? remaining : sizeof(bytes);
            ssize_t written = write(capture[1], bytes, requested);
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0)
                _exit(81);
            remaining -= (uint32_t)written;
        }
        close(capture[1]);
        _exit(0);
    }

    close(capture[1]);
    uint32_t drained = 0u;
    assert(ksd_capture_pipe_drain_until(capture[0], spool,
                                        deadline_after(5000u), &drained));
    assert(drained == byte_count);
    int status;
    assert(waitpid(writer, &status, 0) == writer);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    struct stat spool_status;
    assert(fstat(spool, &spool_status) == 0
        && spool_status.st_size == (off_t)byte_count);
    assert(lseek(spool, 0, SEEK_SET) == 0);
    uint8_t bytes[65536];
    uint32_t remaining = byte_count;
    while (remaining != 0u) {
        size_t requested = remaining < sizeof(bytes)
            ? remaining : sizeof(bytes);
        ssize_t count = read(spool, bytes, requested);
        assert(count > 0);
        for (ssize_t index = 0; index < count; index++)
            assert(bytes[index] == 0x5au);
        remaining -= (uint32_t)count;
    }
    close(capture[0]);
    close(spool);
}

static bool yama_scope_is_one(void)
{
    char value[3] = { 0 };
    int descriptor = open("/proc/sys/kernel/yama/ptrace_scope",
                          O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return false;
    ssize_t length = read(descriptor, value, sizeof(value));
    close(descriptor);
    return (length == 1 || (length == 2 && value[1] == '\n'))
        && value[0] == '1';
}

static void drop_to(uid_t uid, gid_t gid)
{
    if (setgroups(0u, NULL) != 0 || setresgid(gid, gid, gid) != 0
        || setresuid(uid, uid, uid) != 0)
        _exit(90);
}

static void verify_call_child_layout(uid_t uid, gid_t gid)
{
    int capture[2];
    int metadata[2];
    make_capture_pipe(capture);
    int spool = make_capture_spool();
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC,
                      0, metadata) == 0);
    int child_write = fcntl(capture[1], F_DUPFD_CLOEXEC, 10);
    int child_metadata = fcntl(metadata[1], F_DUPFD_CLOEXEC, 10);
    assert(child_write >= 10 && child_metadata >= 10);

    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        (void)close(0);
        (void)close(1);
        (void)close(2);
        if (dup2(child_write, 3) < 0 || dup2(child_metadata, 4) < 0)
            _exit(91);
        if (close_range(5u, UINT_MAX, 0) != 0)
            _exit(91);
        drop_to(uid, gid);
        if (!ksd_capture_child_endpoints_valid(3, 4)
            || prctl(PR_SET_PTRACER, (unsigned long)getppid(), 0, 0, 0) != 0
            || prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) != 0
            || !ksd_capture_child_endpoints_valid(3, 4))
            _exit(92);
        _exit(prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) == 0 ? 0 : 93);
    }

    close(child_write);
    close(child_metadata);
    close(capture[0]);
    close(capture[1]);
    close(spool);
    close(metadata[0]);
    close(metadata[1]);
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

bool ksd_capture_worker_test_round_trip(const ksd_operation_result *sent,
                                        ksd_operation_result *received);
bool ksd_capture_worker_test_scalar_ok_with_fd(
    ksd_operation_result *received);

/* A control verb answers OK with nothing to carry. Before this existed every
 * OK demanded a sealed descriptor of at least twenty bytes, so no operation
 * without a payload could be routed through the worker at all. */
bool ksd_capture_worker_test_parse_header(const uint8_t *bootstrap,
                                          size_t length);

static void encode_bootstrap(uint8_t header[32], uint16_t version,
                             uint32_t group_count, uint32_t frame_length,
                             uint32_t session_pid)
{
    memset(header, 0, 32u);
    memcpy(header, "KSCW", 4u);
    ksd_encode_u16(header + 4u, version);
    /* Backend zero is KSD_BACKEND_NONE, which is in range. The field is at
     * offset 6, where v2 kept a reserved zero. */
    ksd_encode_u16(header + 6u, 0u);
    ksd_encode_u32(header + 8u, 1000u);
    ksd_encode_u32(header + 12u, 1000u);
    ksd_encode_u32(header + 16u, group_count);
    ksd_encode_u32(header + 20u, frame_length);
    ksd_encode_u32(header + 24u, session_pid);
}

/* The header carries the registered daemon pid at an offset that, under the
 * original 24-byte layout, is where the group array starts. A version that is
 * tolerated rather than required would therefore read a group id as a pid.
 *
 * v3 added the backend at offset 6, where v2 kept a reserved zero. That field
 * is why the version had to move: the worker used to guess its route from the
 * payload shape, which cannot distinguish a KWin area capture from an X11 one,
 * and sent every KWin capture to the X11 path. */
/* The capture pipe is checked twice on its way through a capture: once by the
 * worker, and once inside the forked capture child. Between those two checks
 * the worker calls setresuid to become the client, so anything the two
 * validators disagree about becomes a capture that one half accepts and the
 * other refuses.
 *
 * They did disagree. ksd_capture_pipe_valid asked whether the owner was the
 * current effective uid, while ksd_capture_child_endpoints_valid asked whether
 * it was root -- the same question only up until the drop that always happens
 * between them. A system installation failed the first check and a user
 * installation the second, so local and KWin captures were broken in both, at
 * different lines.
 *
 * This runs unprivileged deliberately: the pipe is built exactly as the
 * authority builds it, owned by the installation owner, and both validators
 * must accept it. Neither may hold an opinion about ownership that the other
 * does not share.
 */
static void check_pipe_validators_agree(void)
{
    int descriptors[2];
    int metadata[2];
    int child_write;
    int child_metadata;
    int read_flags;
    int status;
    pid_t child;

    assert(pipe2(descriptors, O_CLOEXEC) == 0);
    /* Exactly what the authority's create_capture_pipe produces: owned by the
     * installation owner, openable by nobody. */
    assert(fchown(descriptors[0], ksd_install_owner(),
                  ksd_install_group()) == 0);
    assert(fchmod(descriptors[0], 0u) == 0);
    read_flags = fcntl(descriptors[0], F_GETFL);
    assert(read_flags >= 0);
    assert(fcntl(descriptors[0], F_SETFL, read_flags | O_NONBLOCK) == 0);
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0,
                      metadata) == 0);
    child_write = fcntl(descriptors[1], F_DUPFD_CLOEXEC, 10);
    child_metadata = fcntl(metadata[1], F_DUPFD_CLOEXEC, 10);
    assert(child_write >= 10 && child_metadata >= 10);

    /* The worker's side of the pair. */
    assert(ksd_capture_pipe_valid(descriptors));

    /* The child's side has to be asked in a child, because it also insists on
     * a bare descriptor table -- it is written for the moment right after
     * close_range, and asking it anywhere else fails for a reason that has
     * nothing to do with ownership. */
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        (void)close(0);
        (void)close(1);
        (void)close(2);
        if (dup2(child_write, 3) < 0 || dup2(child_metadata, 4) < 0)
            _exit(91);
        if (close_range(5u, UINT_MAX, 0) != 0)
            _exit(91);
        _exit(ksd_capture_child_endpoints_valid(3, 4) ? 0 : 92);
    }

    close(child_write);
    close(child_metadata);
    close(descriptors[0]);
    close(descriptors[1]);
    close(metadata[0]);
    close(metadata[1]);
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status));
    /* 92 is the disagreement this exists to catch: the parent accepted the
     * pipe and the child refused the same one. */
    assert(WEXITSTATUS(status) == 0);
}

/* The owner is read once and does not move afterwards. A live read would
 * change under the worker's own privilege drop, which is what broke the pipe
 * checks above -- so the property is worth pinning on its own, not only
 * through the validators that depend on it. */
static void check_owner_is_latched(void)
{
    uid_t first = ksd_install_owner();
    gid_t first_group = ksd_install_group();

    ksd_install_identity_latch();
    assert(ksd_install_owner() == first);
    assert(ksd_install_group() == first_group);
    assert(first == geteuid());
}

static void check_bootstrap_header_parse(void)
{
    uint8_t header[32];
    size_t frame_length = KSD_FRAME_HEADER_SIZE;
    size_t total = 32u + frame_length;

    encode_bootstrap(header, 4u, 0u, (uint32_t)frame_length, 4321u);
    assert(ksd_capture_worker_test_parse_header(header, total));

    /* Both older layouts must be refused outright, not reinterpreted. v2 is
     * the dangerous one now: it is the same 32 bytes with a different meaning
     * at offset 6, so tolerating it would read a reserved zero as a backend
     * and route every request as KSD_BACKEND_NONE. */
    encode_bootstrap(header, 3u, 0u, (uint32_t)frame_length, 4321u);
    assert(!ksd_capture_worker_test_parse_header(header, total));
    encode_bootstrap(header, 2u, 0u, (uint32_t)frame_length, 4321u);
    assert(!ksd_capture_worker_test_parse_header(header, total));
    encode_bootstrap(header, 1u, 0u, (uint32_t)frame_length, 4321u);
    assert(!ksd_capture_worker_test_parse_header(header, total));

    /* A future one likewise. */
    encode_bootstrap(header, 5u, 0u, (uint32_t)frame_length, 4321u);
    assert(!ksd_capture_worker_test_parse_header(header, total));

    /* Every backend the service knows is accepted, and nothing past it. A
     * value out of range would select no route at all and the request would be
     * refused for a reason that names the wrong thing. */
    for (uint16_t backend = 0u; backend <= KSD_BACKEND_X11; backend++) {
        encode_bootstrap(header, 4u, 0u, (uint32_t)frame_length, 4321u);
        ksd_encode_u16(header + 6u, backend);
        assert(ksd_capture_worker_test_parse_header(header, total));
    }
    encode_bootstrap(header, 4u, 0u, (uint32_t)frame_length, 4321u);
    ksd_encode_u16(header + 6u, (uint16_t)(KSD_BACKEND_X11 + 1u));
    assert(!ksd_capture_worker_test_parse_header(header, total));

    /* No pid means no authenticated party to take a display from. */
    encode_bootstrap(header, 4u, 0u, (uint32_t)frame_length, 0u);
    assert(!ksd_capture_worker_test_parse_header(header, total));

    /* The declared lengths must account for the whole message exactly. */
    encode_bootstrap(header, 4u, 0u, (uint32_t)frame_length, 4321u);
    assert(!ksd_capture_worker_test_parse_header(header, total + 1u));
    assert(!ksd_capture_worker_test_parse_header(header, total - 1u));

    /* Offset 28 is the persist flag now, so 1 is legal -- but a persistent
     * worker carries NO initial request, because everything it will do arrives
     * on the socket afterwards. A persist flag with a request attached is two
     * contradictory instructions, and the header is refused rather than one of
     * them being picked. */
    encode_bootstrap(header, 4u, 0u, (uint32_t)frame_length, 4321u);
    ksd_encode_u32(header + 28u, 1u);
    assert(!ksd_capture_worker_test_parse_header(header, total));
    encode_bootstrap(header, 4u, 0u, 0u, 4321u);
    ksd_encode_u32(header + 28u, 1u);
    assert(ksd_capture_worker_test_parse_header(header, 32u));
    /* And a one-shot worker must carry one: a worker with neither a request
     * nor a reason to stay has nothing to do. */
    encode_bootstrap(header, 4u, 0u, 0u, 4321u);
    assert(!ksd_capture_worker_test_parse_header(header, 32u));
    /* Anything but zero or one there is a flag this service does not define.
     * The request is left empty so that the range check is the only rule that
     * can reject this: with a request attached, the persist/request coupling
     * above rejects it first and the range check goes untested. */
    encode_bootstrap(header, 4u, 0u, 0u, 4321u);
    ksd_encode_u32(header + 28u, 2u);
    assert(!ksd_capture_worker_test_parse_header(header, 32u));
}

static void check_scalar_ok_round_trip(void)
{
    ksd_operation_result sent;
    ksd_operation_result received;
    ksd_result_init(&sent);
    ksd_result_init(&received);
    assert(ksd_result_take(&sent, NULL, 0u));
    assert(ksd_capture_worker_test_round_trip(&sent, &received));
    assert(received.status == KSD_STATUS_OK);
    assert(received.tail == NULL && received.tail_length == 0u);
    assert(received.payload_fd < 0);
    ksd_result_clear(&sent);
    ksd_result_clear(&received);

    /* A display backend already writes capture pixels into its own sealed
     * memfd. The worker forwards that descriptor directly; copying it into a
     * second memfd would double peak capture memory and used to reject this
     * otherwise valid result outright. */
    uint8_t capture[24] = { 0 };
    ksd_encode_u16(capture, KSD_CAPTURE_FORMAT_BGRA8_PREMULTIPLIED);
    ksd_encode_u32(capture + 4u, 1u);
    ksd_encode_u32(capture + 8u, 1u);
    ksd_encode_u32(capture + 12u, 4u);
    ksd_encode_u32(capture + 16u, 4u);
    capture[20u] = 1u;
    capture[21u] = 2u;
    capture[22u] = 3u;
    capture[23u] = 0xffu;
    int capture_fd = memfd_create("capture-worker-forward-test",
                                  MFD_CLOEXEC | MFD_ALLOW_SEALING);
    int seals = F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
    assert(capture_fd >= 0);
    assert(write(capture_fd, capture, sizeof(capture))
           == (ssize_t)sizeof(capture));
    assert(fcntl(capture_fd, F_ADD_SEALS, seals) == 0);
    ksd_result_init(&sent);
    ksd_result_init(&received);
    assert(ksd_result_take_fd(&sent, capture_fd, sizeof(capture)));
    assert(ksd_capture_worker_test_round_trip(&sent, &received));
    assert(received.status == KSD_STATUS_OK && received.tail == NULL);
    assert(received.payload_fd >= 0 && received.tail_length == sizeof(capture));
    uint8_t returned[sizeof(capture)];
    assert(pread(received.payload_fd, returned, sizeof(returned), 0)
           == (ssize_t)sizeof(returned));
    assert(memcmp(returned, capture, sizeof(capture)) == 0);
    assert(pread(sent.payload_fd, returned, sizeof(returned), 0)
           == (ssize_t)sizeof(returned));
    ksd_result_clear(&sent);
    ksd_result_clear(&received);

    /* An error still round-trips its diagnostic and carries no descriptor. */
    ksd_result_init(&sent);
    ksd_result_init(&received);
    ksd_result_error(&sent, KSD_STATUS_UNAVAILABLE, 0u, "no display");
    assert(ksd_capture_worker_test_round_trip(&sent, &received));
    assert(received.status == KSD_STATUS_UNAVAILABLE);
    assert(strcmp(received.diagnostic, "no display") == 0);
    assert(received.payload_fd < 0);
    ksd_result_clear(&sent);
    ksd_result_clear(&received);

    /* An answer with no payload must not carry a descriptor. */
    ksd_result_init(&received);
    assert(!ksd_capture_worker_test_scalar_ok_with_fd(&received));
    ksd_result_clear(&received);
}

int main(void)
{
    assert(!ksd_local_capture_kwin_process_trusted(getuid(), getpid()));

    uint8_t boundary[20] = { 0 };
    ksd_encode_u16(boundary, KSD_CAPTURE_FORMAT_PNG);
    ksd_encode_u32(boundary + 4u, 1u);
    ksd_encode_u32(boundary + 8u, 1u);
    ksd_encode_u32(boundary + 16u, KSD_MAX_CAPTURE_BYTES);
    assert(ksd_capture_tail_valid(boundary, KSD_MAX_CAPTURE_TAIL));
    assert(!ksd_capture_tail_valid(boundary, KSD_MAX_CAPTURE_TAIL - 1u));

    check_owner_is_latched();
    check_pipe_validators_agree();
    check_bootstrap_header_parse();
    check_scalar_ok_round_trip();

    if (geteuid() != 0u || !yama_scope_is_one())
        return 77;
    struct passwd *unprivileged = getpwnam("nobody");
    if (unprivileged == NULL || unprivileged->pw_uid == 0u)
        return 77;

    verify_large_pipe_drain();
    verify_call_child_layout(unprivileged->pw_uid, unprivileged->pw_gid);
    return 0;
}
