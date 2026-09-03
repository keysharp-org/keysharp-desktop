#include "permission_domain.h"
#include "backend.h"
#include "protocol.h"
#include "provider.h"
#include "protocol_io.h"

#include <assert.h>
#include <fcntl.h>
#include <dirent.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

int ksd_authority_test_generic_session(int descriptor,
                                       const struct ucred *peer,
                                       const char *persistent_directory,
                                       const char *runtime_directory);
int ksd_authority_test_assembly_budget(unsigned int uid, int reserve);
int ksd_provider_test_capture_memfd(uint32_t width, uint32_t height,
                                    const uint8_t *data, size_t length);
bool ksd_capture_tail_valid(const void *tail, uint32_t tail_length);

/* The capture descriptor handed to a client must be sealed against every
 * kind of change. Unsealed, a peer could rewrite the pixels after their
 * length had been agreed, and the client maps it read-only expecting not to
 * have to re-check. */
/* The advertised mask and the provider dispatch drifted apart once, and
 * Cinnamon window capture was advertised, implemented and refused for as
 * long as they disagreed. Neither side may move without the other.
 * Exhaustive over every backend and both capture opcodes rather than a
 * literal check, so it survives edits to either side. */
static void check_capture_mask_matches_dispatch(void)
{
    static const uint16_t opcodes[] = {
        KSD_OP_CAPTURE_AREA, KSD_OP_CAPTURE_WINDOW,
    };
    static const uint64_t bits[] = {
        KSD_OPERATION_CAPTURE_AREA, KSD_OPERATION_CAPTURE_WINDOW,
    };

    for (uint32_t backend = 0u; backend <= KSD_BACKEND_GENERIC; backend++) {
        uint64_t mask = ksd_backend_operations((ksd_backend)backend);

        for (size_t index = 0u; index < 2u; index++) {
            bool advertised = (mask & bits[index]) != 0u;
            /* Two routes serve a capture. GNOME and Cinnamon go through the
             * provider; KWin never does, and is dispatched to the root capture
             * worker instead. Either route counts as served, but something
             * must. */
            bool served = backend == KSD_BACKEND_KWIN
                || ksd_provider_capture_supported((ksd_backend)backend,
                                                  opcodes[index]);

            if (advertised == served)
                continue;
            fprintf(stderr,
                    "backend %u opcode 0x%04x: advertised=%d served=%d. "
                    "Every advertised capture bit must have a route that "
                    "serves it, and no route may serve one that is not "
                    "advertised. Otherwise the operation is either advertised "
                    "and always refused, or reachable without being "
                    "announced.%s",
                    (unsigned)backend, (unsigned)opcodes[index],
                    (int)advertised, (int)served, "\n");
            abort();
        }
    }
}

static void check_capture_memfd_is_sealed(void)
{
    static const uint8_t png[] = { 0x89u, 0x50u, 0x4eu, 0x47u, 0u, 1u, 2u, 3u };
    int required = F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
    int descriptor = ksd_provider_test_capture_memfd(4u, 4u, png,
                                                     sizeof(png));
    assert(descriptor >= 0);

    int seals = fcntl(descriptor, F_GET_SEALS);
    assert(seals >= 0 && (seals & required) == required);

    uint8_t tail[20u + sizeof(png)];
    assert(pread(descriptor, tail, sizeof(tail), 0)
           == (ssize_t)sizeof(tail));
    assert(ksd_capture_tail_valid(tail, (uint32_t)sizeof(tail)));
    assert(close(descriptor) == 0);
}
int ksd_authority_test_capture_budget(unsigned int uid, int reserve);

static void check_capture_budget(void)
{
    assert(ksd_authority_test_capture_budget(1000u, 1) == 1);
    assert(ksd_authority_test_capture_budget(1000u, 1) == 1);
    assert(ksd_authority_test_capture_budget(1000u, 1) == 0);
    assert(ksd_authority_test_capture_budget(1001u, 1) == 1);
    assert(ksd_authority_test_capture_budget(1000u, 0) == 1);
    assert(ksd_authority_test_capture_budget(1000u, 1) == 1);
    assert(ksd_authority_test_capture_budget(1001u, 1) == 1);
    assert(ksd_authority_test_capture_budget(1002u, 1) == 0);
}

static void check_assembly_budget(void)
{
    for (unsigned int index = 0u; index < 4u; index++)
        assert(ksd_authority_test_assembly_budget(1000u, 1) == 1);
    assert(ksd_authority_test_assembly_budget(1000u, 1) == 0);
    assert(ksd_authority_test_assembly_budget(1001u, 1) == 1);
    assert(ksd_authority_test_assembly_budget(1000u, 0) == 1);
    assert(ksd_authority_test_assembly_budget(1000u, 1) == 1);
    for (unsigned int index = 0u; index < 3u; index++)
        assert(ksd_authority_test_assembly_budget(1001u, 1) == 1);
    for (unsigned int uid = 1002u; uid < 1004u; uid++)
        for (unsigned int index = 0u; index < 4u; index++)
            assert(ksd_authority_test_assembly_budget(uid, 1) == 1);
    assert(ksd_authority_test_assembly_budget(1005u, 1) == 0);
}

typedef struct authority_arguments {
    int descriptor;
    struct ucred peer;
    const char *persistent;
    const char *runtime;
    int result;
} authority_arguments;

static const uint8_t public_magic[4] = {
    KSD_FRAME_MAGIC_0, KSD_FRAME_MAGIC_1,
    KSD_FRAME_MAGIC_2, KSD_FRAME_MAGIC_3,
};

static void *authority_thread(void *argument)
{
    authority_arguments *arguments = argument;
    arguments->result = ksd_authority_test_generic_session(
        arguments->descriptor, &arguments->peer, arguments->persistent,
        arguments->runtime);
    return NULL;
}

static bool store_available(const char *persistent, const char *runtime)
{
    ksp_store_config store_config;
    ksp_store *store = NULL;
    ksp_store_config_init(&store_config, KSD_DESKTOP_MANAGED_SCOPES);
    store_config.persistent_directory = persistent;
    store_config.runtime_directory = runtime;
    store_config.owner_uid = getuid();
    bool available = ksp_store_create(&store, &store_config) == 0
        && ksp_store_prepare(store) == 0;
    ksp_store_destroy(store);
    return available;
}

static int remove_tree(const char *path)
{
    DIR *directory = opendir(path);
    if (directory == NULL)
        return 0;
    for (;;) {
        struct dirent *item = readdir(directory);
        char child[4096];
        struct stat info;
        if (item == NULL)
            break;
        if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0)
            continue;
        int length = snprintf(child, sizeof(child), "%s/%s", path,
                              item->d_name);
        if (length <= 0 || (size_t)length >= sizeof(child)
            || lstat(child, &info) != 0)
            continue;
        if (S_ISDIR(info.st_mode))
            (void)remove_tree(child);
        else
            (void)unlink(child);
    }
    (void)closedir(directory);
    return rmdir(path);
}

static void send_request(int descriptor, uint16_t opcode, uint64_t request_id,
                         uint8_t *payload, uint32_t payload_length)
{
    ksd_frame frame = {
        .magic = {
            KSD_FRAME_MAGIC_0, KSD_FRAME_MAGIC_1,
            KSD_FRAME_MAGIC_2, KSD_FRAME_MAGIC_3,
        },
        .major = KSD_PROTOCOL_MAJOR,
        .minor = KSD_PROTOCOL_MINOR,
        .opcode = opcode,
        .flags = 0u,
        .payload_length = payload_length,
        .request_id = request_id,
        .payload = payload,
    };
    assert(ksd_frame_write(descriptor, &frame));
}

static void send_chunk(int descriptor, uint16_t opcode, uint64_t request_id,
                       uint8_t *payload, uint32_t payload_length, bool more)
{
    ksd_frame frame = {
        .magic = {
            KSD_FRAME_MAGIC_0, KSD_FRAME_MAGIC_1,
            KSD_FRAME_MAGIC_2, KSD_FRAME_MAGIC_3,
        },
        .major = KSD_PROTOCOL_MAJOR,
        .minor = KSD_PROTOCOL_MINOR,
        .opcode = opcode,
        .flags = (uint16_t)(more ? KSD_FLAG_MORE : 0u),
        .payload_length = payload_length,
        .request_id = request_id,
        .payload = payload,
    };
    assert(ksd_frame_write(descriptor, &frame));
}

static uint32_t read_status(int descriptor, uint16_t opcode,
                            uint64_t request_id, uint8_t *tail,
                            uint32_t tail_capacity, uint32_t *tail_length)
{
    ksd_frame frame;
    assert(ksd_frame_read(descriptor, public_magic, KSD_PROTOCOL_MAJOR,
                          KSD_PROTOCOL_MINOR, KSD_MAX_TEXT_BYTES, true,
                          &frame) == 1);
    assert(frame.opcode == opcode && frame.request_id == request_id);
    assert((frame.flags & KSD_FLAG_RESPONSE) != 0u);
    assert((frame.flags & KSD_FLAG_MORE) == 0u);
    assert(frame.payload_length >= 8u);
    uint32_t status = ksd_decode_u32(frame.payload);
    if (tail != NULL && tail_length != NULL) {
        uint32_t length = frame.payload_length - 8u;
        assert(length <= tail_capacity);
        memcpy(tail, frame.payload + 8u, length);
        *tail_length = length;
    }
    ksd_frame_clear(&frame);
    return status;
}

int main(void)
{
    char root[1024];
    char persistent[1024];
    char runtime[1024];
    const char *home = getenv("HOME");
    char *resolved = home == NULL ? NULL : realpath(home, NULL);
    int sockets[2];
    pthread_t thread;
    uint8_t hello[16] = { 0 };
    uint8_t authorize[16] = { 0 };
    uint8_t revoke[48] = { 0 };
    uint8_t tail[64] = { 0 };
    uint32_t tail_length = 0u;

    if (geteuid() == 0u || resolved == NULL)
        return 77;
    int length = snprintf(root, sizeof(root), "%s/.ksd-generic-XXXXXX",
                          resolved);
    free(resolved);
    if (length <= 0 || (size_t)length >= sizeof(root)
        || mkdtemp(root) == NULL)
        return 77;
    length = snprintf(persistent, sizeof(persistent), "%s/persistent", root);
    assert(length > 0 && (size_t)length < sizeof(persistent));
    length = snprintf(runtime, sizeof(runtime), "%s/runtime", root);
    assert(length > 0 && (size_t)length < sizeof(runtime));
    if (!store_available(persistent, runtime)) {
        (void)remove_tree(root);
        return 77;
    }

    struct timeval receive_timeout = { .tv_sec = 30, .tv_usec = 0 };
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    assert(setsockopt(sockets[0], SOL_SOCKET, SO_RCVTIMEO, &receive_timeout,
                      sizeof(receive_timeout)) == 0);
    authority_arguments arguments = {
        .descriptor = sockets[1],
        .peer = {
            .pid = getpid(),
            .uid = getuid(),
            .gid = getgid(),
        },
        .persistent = persistent,
        .runtime = runtime,
        .result = -1,
    };
    pthread_attr_t attributes;
    assert(pthread_attr_init(&attributes) == 0);
    assert(pthread_attr_setstacksize(&attributes, 8u * 1024u * 1024u)
           == 0);
    assert(pthread_create(&thread, &attributes, authority_thread,
                          &arguments) == 0);
    assert(pthread_attr_destroy(&attributes) == 0);

    ksd_encode_u16(hello, (uint16_t)KSD_ROLE_RPC);
    ksd_encode_u16(hello + 2u, (uint16_t)KSD_AUTH_CHECK);
    send_request(sockets[0], KSD_OP_HELLO, 1u, hello, sizeof(hello));
    assert(read_status(sockets[0], KSD_OP_HELLO, 1u, tail, sizeof(tail),
                       &tail_length) == KSD_STATUS_OK);
    assert(tail_length == 24u);
    assert(ksd_decode_u32(tail) == 0u);
    assert(ksd_decode_u64(tail + 8u) == 0u);
    assert(ksd_decode_u32(tail + 16u) == KSD_BACKEND_GENERIC);

    send_request(sockets[0], KSD_OP_WORK_AREA, 2u, NULL, 0u);
    assert(read_status(sockets[0], KSD_OP_WORK_AREA, 2u, NULL, 0u, NULL)
           == KSD_STATUS_UNAVAILABLE);

    send_request(sockets[0], KSD_OP_WINDOW_LIST, 3u, NULL, 0u);
    assert(read_status(sockets[0], KSD_OP_WINDOW_LIST, 3u, NULL, 0u, NULL)
           == KSD_STATUS_DENIED);

    ksd_encode_u16(authorize, (uint16_t)KSD_AUTH_CHECK);
    ksd_encode_u32(authorize + 4u, KSD_SCOPE_SCREEN_CAPTURE);
    send_request(sockets[0], KSD_OP_AUTHORIZE, 4u, authorize,
                 sizeof(authorize));
    assert(read_status(sockets[0], KSD_OP_AUTHORIZE, 4u, NULL, 0u, NULL)
           == KSD_STATUS_DENIED);

    send_request(sockets[0], KSD_OP_PERMISSIONS_LIST, 5u, NULL, 0u);
    assert(read_status(sockets[0], KSD_OP_PERMISSIONS_LIST, 5u, NULL, 0u,
                       NULL) == KSD_STATUS_OK);

    ksd_encode_u32(revoke, KSD_PERMISSION_TARGET_ALL);
    ksd_encode_u32(revoke + 4u, (uint32_t)KSD_DESKTOP_MANAGED_SCOPES);
    send_request(sockets[0], KSD_OP_PERMISSIONS_REVOKE, 6u, revoke,
                 sizeof(revoke));
    assert(read_status(sockets[0], KSD_OP_PERMISSIONS_REVOKE, 6u, NULL, 0u,
                       NULL) == KSD_STATUS_OK);

    static uint8_t chunk[KSD_MAX_REQUEST_PAYLOAD];
    const uint32_t mimetype_length =
        (uint32_t)sizeof(KSD_CLIPBOARD_TEXT_MIMETYPE) - 1u;
    ksd_encode_u32(chunk, mimetype_length);
    memcpy(chunk + 4u, KSD_CLIPBOARD_TEXT_MIMETYPE, mimetype_length);
    ksd_encode_u32(chunk + 4u + mimetype_length, KSD_MAX_REQUEST_PAYLOAD);
    send_chunk(sockets[0], KSD_OP_CLIPBOARD_SET_CONTENT, 7u, chunk,
               KSD_MAX_REQUEST_PAYLOAD, true);
    send_chunk(sockets[0], KSD_OP_CLIPBOARD_SET_CONTENT, 7u, chunk,
               8u + mimetype_length, false);
    assert(read_status(sockets[0], KSD_OP_CLIPBOARD_SET_CONTENT, 7u, NULL,
                       0u, NULL) == KSD_STATUS_UNAVAILABLE);

    check_assembly_budget();
    check_capture_mask_matches_dispatch();
    check_capture_memfd_is_sealed();
    check_capture_budget();

    assert(close(sockets[0]) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(arguments.result == 0);
    assert(close(sockets[1]) == 0);
    (void)remove_tree(root);
    return 0;
}
