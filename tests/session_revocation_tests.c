#include "common.h"
#include "keysharp_desktop/protocol.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int listener(const char *path)
{
    struct sockaddr_un address;
    int descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(descriptor >= 0);
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    (void)snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    (void)unlink(path);
    assert(bind(descriptor, (const struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(descriptor, 4) == 0);
    return descriptor;
}

static int connect_with_retry(const char *path)
{
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    (void)snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    for (int attempt = 0; attempt < 100; attempt++) {
        int descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (descriptor >= 0
            && connect(descriptor, (const struct sockaddr *)&address, sizeof(address)) == 0)
            return descriptor;
        if (descriptor >= 0)
            close(descriptor);
        struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000L };
        (void)nanosleep(&delay, NULL);
    }
    return -1;
}

static void serve_authorization(int authority_listener)
{
    int connection = accept4(authority_listener, NULL, NULL, SOCK_CLOEXEC);
    ksd_authority_request request;
    int client_fd = -1;
    assert(connection >= 0);
    assert(ksd_receive_optional_fd(connection, &request, sizeof(request), &client_fd) == 0);
    assert(client_fd >= 0 && request.operation == KSD_AUTH_OP_CHECK);
    ksd_authority_response response = {
        .magic = KSD_AUTH_MAGIC,
        .major = KSD_PROTOCOL_MAJOR,
        .minor = KSD_PROTOCOL_MINOR,
        .status = KSD_AUTH_STATUS_GRANTED,
        .granted_capabilities = request.capabilities,
    };
    assert(ksd_write_all(connection, &response, sizeof(response)));
    close(client_fd);
    close(connection);
}

static void read_line(int descriptor, char *buffer, size_t capacity)
{
    size_t used = 0u;
    while (used + 1u < capacity) {
        assert(read(descriptor, &buffer[used], 1u) == 1);
        if (buffer[used++] == '\n') {
            buffer[used] = '\0';
            return;
        }
    }
    assert(false);
}

static void advance_generation(void)
{
    char path[KSD_PATH_MAX];
    uint64_t generation = 1u;
    assert(mkdir(KSD_RUNTIME_DIRECTORY, 0755) == 0 || errno == EEXIST);
    assert(ksd_revoke_generation_path(getuid(), path, sizeof(path)) == 0);
    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    assert(descriptor >= 0);
    assert(fchmod(descriptor, 0644) == 0);
    assert(ksd_write_all(descriptor, &generation, sizeof(generation)));
    close(descriptor);
}

int main(int argc, char **argv)
{
    static const char authority_path[] = "/tmp/keysharp-desktop-session-revoke-authority.sock";
    char directory[] = "/tmp/keysharp-desktop-session-revoke-XXXXXX";
    char broker_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    char generation_path[KSD_PATH_MAX];
    char response[1024];
    int authority_listener;
    int descriptor;
    int status;
    pid_t authority_child;
    pid_t broker_child;

    assert(argc == 2);
    assert(mkdtemp(directory) != NULL);
    (void)snprintf(broker_path, sizeof(broker_path), "%s/broker.sock", directory);
    assert(ksd_revoke_generation_path(getuid(), generation_path, sizeof(generation_path)) == 0);
    (void)unlink(generation_path);
    (void)rmdir(KSD_RUNTIME_DIRECTORY);
    authority_listener = listener(authority_path);

    authority_child = fork();
    assert(authority_child >= 0);
    if (authority_child == 0) {
        serve_authorization(authority_listener);
        _exit(0);
    }
    broker_child = fork();
    assert(broker_child >= 0);
    if (broker_child == 0) {
        setenv("KEYSHARP_DESKTOP_SOCKET", broker_path, 1);
        setenv("KEYSHARP_DESKTOP_IDLE_SECONDS", "0", 1);
        execl(argv[1], argv[1], NULL);
        _exit(127);
    }
    close(authority_listener);

    descriptor = connect_with_retry(broker_path);
    assert(descriptor >= 0);
    static const char hello[] = KSD_HANDSHAKE_PREFIX " HELLO auto 00000001 check\n";
    assert(ksd_write_all(descriptor, hello, sizeof(hello) - 1u));
    read_line(descriptor, response, sizeof(response));
    assert(strstr(response, " READY none 00000001 ") != NULL);

    struct timespec idle = { .tv_sec = 2, .tv_nsec = 0 };
    assert(nanosleep(&idle, NULL) == 0);
    assert(send(descriptor, "ping\n", 5u, MSG_NOSIGNAL) == 5);
    uint8_t ping;
    assert(read(descriptor, &ping, sizeof(ping)) == 1
           && ping == KSD_RESPONSE_OK);

    advance_generation();
    struct pollfd poll_fd = { .fd = descriptor, .events = POLLIN | POLLHUP };
    assert(poll(&poll_fd, 1u, 2000) > 0);
    char byte;
    assert(read(descriptor, &byte, 1u) == 0);
    close(descriptor);

    (void)kill(broker_child, SIGTERM);
    assert(waitpid(broker_child, &status, 0) == broker_child);
    assert(waitpid(authority_child, &status, 0) == authority_child && WIFEXITED(status));
    (void)unlink(authority_path);
    (void)unlink(generation_path);
    (void)rmdir(KSD_RUNTIME_DIRECTORY);
    (void)unlink(broker_path);
    (void)rmdir(directory);
    return 0;
}
