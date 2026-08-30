#include "authority_client.h"
#include "common.h"
#include "keysharp_desktop/protocol.h"
#include "roles.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static void socket_path(char *path, size_t capacity)
{
    const char *configured = getenv("KEYSHARP_DESKTOP_SOCKET");
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    char fallback[64];

    if (configured != NULL && configured[0] != '\0') {
        (void)snprintf(path, capacity, "%s", configured);
        return;
    }
    if (runtime == NULL || runtime[0] == '\0') {
        (void)snprintf(fallback, sizeof(fallback), "/run/user/%lu", (unsigned long)getuid());
        runtime = fallback;
    }
    (void)snprintf(path, capacity, "%s/%s", runtime, KSD_DEFAULT_SOCKET_SUFFIX);
}

static int connect_broker(void)
{
    char path[4096];
    struct sockaddr_un address;
    int descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (descriptor < 0)
        return -1;
    socket_path(path, sizeof(path));
    if (strlen(path) >= sizeof(address.sun_path)) {
        close(descriptor);
        return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1u);
    if (connect(descriptor, (const struct sockaddr *)&address, sizeof(address)) != 0) {
        close(descriptor);
        return -1;
    }
    return descriptor;
}

static ssize_t read_line(int descriptor, char *buffer, size_t capacity)
{
    size_t used = 0u;
    while (used + 1u < capacity) {
        ssize_t count = read(descriptor, &buffer[used], 1u);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        if (buffer[used] == '\n') {
            buffer[used] = '\0';
            return (ssize_t)used;
        }
        used++;
    }
    return -1;
}

static uint32_t parse_capabilities(const char *name)
{
    if (name == NULL || strcmp(name, "all") == 0)
        return KSD_CAP_ALL;
    if (strcmp(name, "screen-capture") == 0)
        return KSD_CAP_SCREEN_CAPTURE;
    if (strcmp(name, "window-monitoring") == 0)
        return KSD_CAP_WINDOW_MONITORING;
    if (strcmp(name, "window-control") == 0)
        return KSD_CAP_WINDOW_CONTROL;
    if (strcmp(name, "audio-capture") == 0)
        return KSD_CAP_AUDIO_CAPTURE;
    if (strcmp(name, "camera-capture") == 0)
        return KSD_CAP_CAMERA_CAPTURE;
    if (strcmp(name, "clipboard-monitoring") == 0)
        return KSD_CAP_CLIPBOARD_MONITORING;
    return 0u;
}

static bool valid_hash(const char *hash)
{
    if (hash == NULL || strlen(hash) != KSD_HASH_HEX_LENGTH)
        return false;
    for (size_t index = 0u; index < KSD_HASH_HEX_LENGTH; index++)
        if (!((hash[index] >= '0' && hash[index] <= '9')
              || (hash[index] >= 'a' && hash[index] <= 'f')))
            return false;
    return true;
}

static void format_capabilities(uint32_t capabilities, char *buffer, size_t capacity)
{
    static const struct {
        uint32_t capability;
        const char *name;
    } names[] = {
        { KSD_CAP_SCREEN_CAPTURE, "screen-capture" },
        { KSD_CAP_WINDOW_MONITORING, "window-monitoring" },
        { KSD_CAP_WINDOW_CONTROL, "window-control" },
        { KSD_CAP_AUDIO_CAPTURE, "audio-capture" },
        { KSD_CAP_CAMERA_CAPTURE, "camera-capture" },
        { KSD_CAP_CLIPBOARD_MONITORING, "clipboard-monitoring" },
    };
    size_t used = 0u;

    if (capacity == 0u)
        return;
    buffer[0] = '\0';
    for (size_t index = 0u; index < sizeof(names) / sizeof(names[0]); index++) {
        if ((capabilities & names[index].capability) == 0u)
            continue;
        int written = snprintf(buffer + used, capacity - used, "%s%s",
                               used == 0u ? "" : ",", names[index].name);
        if (written < 0 || (size_t)written >= capacity - used)
            return;
        used += (size_t)written;
    }
}

static int list_grants(void)
{
    ksd_authority_grant_info *grants = NULL;
    size_t count = 0u;

    if (ksd_authority_list_current_uid(&grants, &count) != 0) {
        fputs("keysharp-desktop: list failed\n", stderr);
        return 2;
    }
    puts("HASH\tCAPABILITIES\tEXECUTABLE");
    for (size_t index = 0u; index < count; index++) {
        char capabilities[160];
        format_capabilities(grants[index].capabilities,
                            capabilities, sizeof(capabilities));
        printf("%s\t%s\t%s\n", grants[index].hash,
               capabilities, grants[index].executable);
    }
    free(grants);
    return 0;
}

static int handshake(uint32_t capabilities, bool interactive)
{
    char request[256];
    char response[1024];
    int descriptor = connect_broker();

    if (descriptor < 0) {
        perror("keysharp-desktop: connect");
        return 2;
    }
    int length = snprintf(request, sizeof(request), KSD_HANDSHAKE_PREFIX
        " HELLO auto %08x %s\n", capabilities, interactive ? "request" : "check");
    if (length <= 0 || (size_t)length >= sizeof(request)
        || !ksd_write_all(descriptor, request, (size_t)length)
        || read_line(descriptor, response, sizeof(response)) < 0) {
        fputs("keysharp-desktop: protocol exchange failed\n", stderr);
        close(descriptor);
        return 2;
    }
    close(descriptor);
    puts(response);
    return strstr(response, " READY ") != NULL ? 0
        : strstr(response, " ERROR denied ") != NULL ? 3 : 2;
}

static void print_version(void)
{
    puts("product=keysharp-desktop");
    puts("product_version=" KSD_PRODUCT_VERSION);
    printf("protocol_major=%u\nprotocol_minor=%u\n", KSD_PROTOCOL_MAJOR, KSD_PROTOCOL_MINOR);
    puts("protocol_name=keysharp-desktop/session-v1");
    puts("protocol=" KSD_PROTOCOL_LABEL);
    puts("modes=serve,authority,version,probe,status,grant,list,revoke");
    puts("capability_screen_capture=0x00000001");
    puts("capability_window_monitoring=0x00000002");
    puts("capability_window_control=0x00000004");
    puts("capability_audio_capture=0x00000008");
    puts("capability_camera_capture=0x00000010");
    puts("capability_clipboard_monitoring=0x00000020");
}

static void usage(const char *program)
{
    fprintf(stderr,
        "usage: %s version|probe|status [capability]|grant [capability]|list\n"
        "       %s revoke --all|<64-hex-app-hash> [capability]\n"
        "capability: all, screen-capture, window-monitoring, window-control,\n"
        "            audio-capture, camera-capture, clipboard-monitoring\n",
        program, program);
}

int ksd_cli_main(int argc, char **argv)
{
    uint32_t capabilities;

    if (argc == 2 && strcmp(argv[1], "version") == 0) {
        print_version();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "probe") == 0)
        return handshake(0u, false);
    if (argc == 2 && strcmp(argv[1], "list") == 0)
        return list_grants();
    if ((argc == 2 || argc == 3)
        && (strcmp(argv[1], "status") == 0 || strcmp(argv[1], "grant") == 0)) {
        capabilities = parse_capabilities(argc == 3 ? argv[2] : "all");
        if (capabilities == 0u) {
            usage(argv[0]);
            return 2;
        }
        return handshake(capabilities, strcmp(argv[1], "grant") == 0);
    }
    if (argc == 3 && strcmp(argv[1], "revoke") == 0
        && strcmp(argv[2], "--all") == 0) {
        if (ksd_authority_revoke_current_uid(NULL, 0u, true) != 0) {
            fputs("keysharp-desktop: revoke failed\n", stderr);
            return 2;
        }
        puts("Revoked all keysharp-desktop grants for the current user.");
        return 0;
    }
    if ((argc == 3 || argc == 4) && strcmp(argv[1], "revoke") == 0
        && valid_hash(argv[2])) {
        capabilities = parse_capabilities(argc == 4 ? argv[3] : "all");
        if (capabilities == 0u) {
            usage(argv[0]);
            return 2;
        }
        if (ksd_authority_revoke_current_uid(argv[2], capabilities, false) != 0) {
            fputs("keysharp-desktop: revoke failed\n", stderr);
            return 2;
        }
        printf("Revoked selected keysharp-desktop grants for %s.\n", argv[2]);
        return 0;
    }
    usage(argv[0]);
    return 2;
}
