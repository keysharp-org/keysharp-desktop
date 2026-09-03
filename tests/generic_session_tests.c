#include "permission_domain.h"
#include "protocol.h"
#include "protocol_io.h"

#include <assert.h>
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

    assert(close(sockets[0]) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(arguments.result == 0);
    assert(close(sockets[1]) == 0);
    (void)remove_tree(root);
    return 0;
}
