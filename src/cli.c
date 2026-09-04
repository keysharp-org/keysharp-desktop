#include "enable_extension.h"
#include "keysharp_desktop/client.h"
#include "protocol.h"
#include "permission_domain.h"
#include "roles.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct list_context {
    bool first;
} list_context;

static void usage(FILE *stream, const char *program)
{
    fprintf(stream,
        "usage: %s version\n"
        "       %s info\n"
        "       %s probe [--socket PATH]\n"
        "       %s daemon\n"
        "       %s permissions list [--socket PATH]\n"
        "       %s permissions revoke (--hash HASH|--pid PID|--all) [SCOPE ...] [--socket PATH]\n"
        "       %s enable-extension\n",
        program, program, program, program, program, program, program);
}

static int report_error(const char *operation, ksd_status status,
                        const ksd_error *error)
{
    fprintf(stderr, "keysharp-desktop: %s: %s", operation,
            ksd_status_name(status));
    if (error != NULL && error->message[0] != '\0')
        fprintf(stderr, ": %s", error->message);
    fputc('\n', stderr);
    return 1;
}

static bool socket_option(int argc, char **argv, int first,
                          const char **socket_path)
{
    *socket_path = NULL;
    if (first == argc)
        return true;
    if (first + 2 != argc || strcmp(argv[first], "--socket") != 0
        || argv[first + 1][0] != '/')
        return false;
    *socket_path = argv[first + 1];
    return true;
}

static ksd_status open_rpc(const char *socket_path,
                           ksd_connection **connection,
                           ksd_service_info *info, ksd_error *error)
{
    ksd_connect_options options;
    ksd_connect_options_init(&options);
    options.socket_path = socket_path;
    options.role = KSD_ROLE_RPC;
    options.requested_scopes = 0u;
    ksd_service_info_init(info);
    return ksd_connect(&options, connection, info, error);
}

static void print_version(void)
{
    printf("%s %s\n", ksd_client_product_name(),
           ksd_client_product_version());
}

static void print_info(void)
{
    printf("product_name=%s\n", ksd_client_product_name());
    printf("product_version=%s\n", ksd_client_product_version());
    printf("client_abi_major=%u\n", ksd_client_abi_major());
    printf("client_abi_minor=%u\n", ksd_client_abi_minor());
    puts("permission_store_version=" KSD_PERMISSION_STORE_VERSION);
    puts("socket=" KSD_DEFAULT_SOCKET_PATH);
    puts("service_scope=system");
    puts("activation=systemd-socket");
}

static int probe_command(int argc, char **argv)
{
    const char *socket_path;
    if (!socket_option(argc, argv, 2, &socket_path))
        return 2;
    ksd_connection *connection = NULL;
    ksd_service_info info;
    ksd_error error;
    ksd_error_init(&error);
    ksd_status status = open_rpc(socket_path, &connection, &info, &error);
    if (status != KSD_STATUS_OK)
        return report_error("probe", status, &error);
    puts("service_status=ready");
    printf("granted_scopes=0x%08x\n", info.granted_scopes);
    printf("available_operations=0x%016" PRIx64 "\n",
           info.available_operations);
    printf("backend=%s\n", ksd_backend_name(info.backend));
    ksd_disconnect(connection);
    return 0;
}

static bool format_scopes(uint32_t scopes, char *text, size_t capacity)
{
    static const uint32_t ordered[] = {
        KSD_SCOPE_INPUT_MONITORING,
        KSD_SCOPE_INPUT_CONTROL,
        KSD_SCOPE_WINDOW_MONITORING,
        KSD_SCOPE_WINDOW_CONTROL,
        KSD_SCOPE_SCREEN_CAPTURE,
        KSD_SCOPE_AUDIO_CAPTURE,
        KSD_SCOPE_CAMERA_CAPTURE,
        KSD_SCOPE_CLIPBOARD_MONITORING,
    };
    size_t used = 0u;
    uint32_t named = 0u;
    for (size_t index = 0u; index < sizeof(ordered) / sizeof(ordered[0]);
         index++) {
        if ((scopes & ordered[index]) == 0u)
            continue;
        named |= ordered[index];
        const char *name = ksd_scope_name(ordered[index]);
        size_t length = strlen(name);
        if (used + (used == 0u ? 0u : 1u) + length >= capacity)
            return false;
        if (used != 0u)
            text[used++] = ',';
        memcpy(text + used, name, length);
        used += length;
    }
    uint32_t unnamed = scopes & ~named;
    if (unnamed != 0u) {
        int written = snprintf(text + used, capacity - used, "%s0x%08x",
                               used == 0u ? "" : ",", unnamed);
        if (written < 0 || (size_t)written >= capacity - used)
            return false;
        used += (size_t)written;
    }
    if (used == 0u || used >= capacity)
        return false;
    text[used] = '\0';
    return true;
}

static bool print_permission(const ksd_permission_entry *entry,
                             void *user_data)
{
    list_context *context = user_data;
    char scopes[256];
    if (!format_scopes(entry->scopes, scopes, sizeof(scopes)))
        return false;
    if (!context->first)
        putchar('\n');
    context->first = false;
    printf("uid=%lu\n", (unsigned long)getuid());
    printf("hash=%s\n", entry->hash);
    printf("executable=%s\n", entry->executable);
    printf("scopes=%s\n", scopes);
    printf("granted_at_utc=%" PRIu64 "\n", entry->granted_at_utc);
    return !ferror(stdout);
}

static int permissions_list_command(int argc, char **argv)
{
    const char *socket_path;
    if (!socket_option(argc, argv, 3, &socket_path))
        return 2;
    ksd_connection *connection = NULL;
    ksd_service_info info;
    ksd_error error;
    ksd_error_init(&error);
    ksd_status status = open_rpc(socket_path, &connection, &info, &error);
    if (status != KSD_STATUS_OK)
        return report_error("permissions list", status, &error);
    list_context context = { .first = true };
    status = ksd_permissions_list(connection, print_permission,
                                  &context, &error);
    ksd_disconnect(connection);
    return status == KSD_STATUS_OK
        ? 0 : report_error("permissions list", status, &error);
}

static uint32_t scope_from_name(const char *name)
{
    static const uint32_t owned[] = {
        KSD_SCOPE_INPUT_CONTROL,
        KSD_SCOPE_WINDOW_MONITORING,
        KSD_SCOPE_WINDOW_CONTROL,
        KSD_SCOPE_SCREEN_CAPTURE,
        KSD_SCOPE_AUDIO_CAPTURE,
        KSD_SCOPE_CAMERA_CAPTURE,
        KSD_SCOPE_CLIPBOARD_MONITORING,
    };
    for (size_t index = 0u; index < sizeof(owned) / sizeof(owned[0]); index++)
        if (strcmp(name, ksd_scope_name(owned[index])) == 0)
            return owned[index];
    return 0u;
}

static bool parse_pid(const char *text, uint64_t *pid)
{
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0u
        || value > INT32_MAX)
        return false;
    *pid = (uint64_t)value;
    return true;
}

static bool parse_hash(const char *text)
{
    if (text == NULL || strlen(text) != KSD_PERMISSION_HASH_HEX_SIZE - 1u)
        return false;
    for (size_t index = 0u; index < KSD_PERMISSION_HASH_HEX_SIZE - 1u;
         index++)
        if (!((text[index] >= '0' && text[index] <= '9')
              || (text[index] >= 'a' && text[index] <= 'f')))
            return false;
    return true;
}

static int permissions_revoke_command(int argc, char **argv)
{
    if (argc < 4)
        return 2;
    ksd_permission_revoke revoke;
    ksd_permission_revoke_init(&revoke);
    int index;
    if (strcmp(argv[3], "--all") == 0) {
        revoke.target_kind = KSD_PERMISSION_TARGET_ALL;
        index = 4;
    } else if (strcmp(argv[3], "--hash") == 0 && argc >= 5) {
        if (!parse_hash(argv[4]))
            return 2;
        revoke.target_kind = KSD_PERMISSION_TARGET_HASH;
        memcpy(revoke.hash, argv[4], KSD_PERMISSION_HASH_HEX_SIZE);
        index = 5;
    } else if (strcmp(argv[3], "--pid") == 0 && argc >= 5
               && parse_pid(argv[4], &revoke.pid)) {
        revoke.target_kind = KSD_PERMISSION_TARGET_PID;
        index = 5;
    } else {
        return 2;
    }

    const char *socket_path = NULL;
    revoke.scopes = 0u;
    while (index < argc) {
        if (strcmp(argv[index], "--socket") == 0) {
            if (socket_path != NULL || index + 2 != argc
                || argv[index + 1][0] != '/')
                return 2;
            socket_path = argv[index + 1];
            index += 2;
            break;
        }
        uint32_t scope = scope_from_name(argv[index]);
        if (scope == 0u || (revoke.scopes & scope) != 0u)
            return 2;
        revoke.scopes |= scope;
        index++;
    }
    if (index != argc)
        return 2;
    if (revoke.scopes == 0u)
        revoke.scopes = KSD_DESKTOP_MANAGED_SCOPES;

    ksd_connection *connection = NULL;
    ksd_service_info info;
    ksd_error error;
    ksd_error_init(&error);
    ksd_status status = open_rpc(socket_path, &connection, &info, &error);
    if (status != KSD_STATUS_OK)
        return report_error("permissions revoke", status, &error);
    status = ksd_permissions_revoke(connection, &revoke, &error);
    ksd_disconnect(connection);
    if (status != KSD_STATUS_OK)
        return report_error("permissions revoke", status, &error);
    char scopes[256];
    if (!format_scopes(revoke.scopes, scopes, sizeof(scopes)))
        return 1;
    printf("revoked_scopes=%s\n", scopes);
    return 0;
}

int ksd_cli_main(int argc, char **argv)
{
    if (argc == 1 || (argc == 2
        && (strcmp(argv[1], "help") == 0
            || strcmp(argv[1], "--help") == 0))) {
        usage(stdout, argv[0]);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "version") == 0) {
        print_version();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "info") == 0) {
        print_info();
        return 0;
    }
    if (strcmp(argv[1], "probe") == 0)
        return probe_command(argc, argv);
    if (strcmp(argv[1], "enable-extension") == 0)
        return ksd_enable_extension_main(argc - 1, argv + 1);
    if (argc >= 3 && strcmp(argv[1], "permissions") == 0) {
        if (strcmp(argv[2], "list") == 0)
            return permissions_list_command(argc, argv);
        if (strcmp(argv[2], "revoke") == 0)
            return permissions_revoke_command(argc, argv);
    }
    usage(stderr, argv[0]);
    return 2;
}
