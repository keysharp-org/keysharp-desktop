#include "common.h"
#include "keysharp_desktop/protocol.h"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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

static ssize_t line(int descriptor, char *buffer, size_t capacity)
{
    size_t used = 0u;
    while (used + 1u < capacity) {
        if (read(descriptor, &buffer[used], 1u) != 1)
            return -1;
        if (buffer[used] == '\n') {
            buffer[used] = '\0';
            return (ssize_t)used;
        }
        used++;
    }
    return -1;
}

static void expect_error(int descriptor, const char *command,
                         const char *expected)
{
    uint8_t response_status;
    uint32_t length;
    char message[256];

    assert(ksd_write_all(descriptor, command, strlen(command)));
    assert(ksd_read_all(descriptor, &response_status, sizeof(response_status)));
    assert(response_status == KSD_RESPONSE_ERROR);
    assert(ksd_read_all(descriptor, &length, sizeof(length)));
    assert(length < sizeof(message));
    assert(ksd_read_all(descriptor, message, length));
    message[length] = '\0';
    assert(strstr(message, expected) != NULL);
}

int main(int argc, char **argv)
{
    char directory[] = "/tmp/keysharp-desktop-test-XXXXXX";
    char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    char response[1024];
    pid_t child;
    int descriptor;
    int second_descriptor;
    int outdated_descriptor;
    int reserved_descriptor;
    int status;
    uint8_t ping;

    assert(argc == 2);
    assert(mkdtemp(directory) != NULL);
    (void)snprintf(socket_path, sizeof(socket_path), "%s/broker.sock", directory);
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        setenv("KEYSHARP_DESKTOP_SOCKET", socket_path, 1);
        setenv("KEYSHARP_DESKTOP_IDLE_SECONDS", "5", 1);
        execl(argv[1], argv[1], "serve", NULL);
        _exit(127);
    }

    descriptor = connect_with_retry(socket_path);
    assert(descriptor >= 0);
    static const char hello[] = KSD_HANDSHAKE_PREFIX " HELLO auto 00000000 check\n";
    assert(ksd_write_all(descriptor, hello, sizeof(hello) - 1u));
    assert(line(descriptor, response, sizeof(response)) > 0);
    assert(strstr(response, " READY none 00000000 ") != NULL);

    second_descriptor = connect_with_retry(socket_path);
    assert(second_descriptor >= 0);
    assert(ksd_write_all(second_descriptor, hello, sizeof(hello) - 1u));
    assert(line(second_descriptor, response, sizeof(response)) > 0);
    assert(strstr(response, " READY none 00000000 ") != NULL);

    expect_error(descriptor, "window-list 0\n",
                 "window-monitoring capability is not granted");
    expect_error(descriptor, "window-focus 1\n",
                 "window-control capability is not granted");
    expect_error(descriptor, "clipboard-text\n",
                 "clipboard-monitoring capability is not granted");

    outdated_descriptor = connect_with_retry(socket_path);
    assert(outdated_descriptor >= 0);
    static const char outdated_hello[] =
        "KSDP/1.1 HELLO auto 00000000 check\n";
    assert(ksd_write_all(outdated_descriptor, outdated_hello,
                         sizeof(outdated_hello) - 1u));
    assert(line(outdated_descriptor, response, sizeof(response)) > 0);
    assert(strstr(response, " ERROR protocol incompatible protocol version") != NULL);
    close(outdated_descriptor);

    reserved_descriptor = connect_with_retry(socket_path);
    assert(reserved_descriptor >= 0);
    static const char reserved_hello[] =
        KSD_HANDSHAKE_PREFIX " HELLO auto 00000040 check\n";
    assert(ksd_write_all(reserved_descriptor, reserved_hello,
                         sizeof(reserved_hello) - 1u));
    assert(line(reserved_descriptor, response, sizeof(response)) > 0);
    assert(strstr(response, " ERROR protocol unknown capability bits") != NULL);
    close(reserved_descriptor);

    assert(ksd_write_all(descriptor, "ping\n", 5u));
    assert(read(descriptor, &ping, 1u) == 1 && ping == KSD_RESPONSE_OK);
    assert(ksd_write_all(second_descriptor, "ping\n", 5u));
    assert(read(second_descriptor, &ping, 1u) == 1 && ping == KSD_RESPONSE_OK);
    assert(ksd_write_all(descriptor, "quit\n", 5u));
    assert(ksd_write_all(second_descriptor, "quit\n", 5u));
    close(descriptor);
    close(second_descriptor);

    (void)kill(child, SIGTERM);
    assert(waitpid(child, &status, 0) == child);
    (void)unlink(socket_path);
    (void)rmdir(directory);
    return 0;
}
