#include <keysharp_desktop/client.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    ksd_connect_options options;
    ksd_service_info info;
    ksd_error error;
    ksd_connection *connection = NULL;
    ksd_connection *lease = NULL;
    ksd_string windows;
    uint64_t target = 0u;
    uint32_t scopes = KSD_SCOPE_WINDOW_MONITORING;

    if (argc != 1) {
        char *end = NULL;
        if (argc != 3 || strcmp(argv[1], "--unminimize") != 0
            || argv[2][0] == '-') {
            fprintf(stderr, "usage: %s [--unminimize HANDLE]\n", argv[0]);
            return 1;
        }
        target = strtoull(argv[2], &end, 0);
        if (target == 0u || *end != '\0') {
            fprintf(stderr, "invalid window handle\n");
            return 1;
        }
        scopes |= KSD_SCOPE_WINDOW_CONTROL;
    }

    ksd_connect_options_init(&options);
    ksd_service_info_init(&info);
    ksd_error_init(&error);
    ksd_string_init(&windows);
    options.role = KSD_ROLE_AUTHORIZATION_LEASE;
    options.authorization_mode = KSD_AUTH_REQUEST;
    options.requested_scopes = scopes;

    if (ksd_connect(&options, &lease, &info, &error) != KSD_STATUS_OK) {
        fprintf(stderr, "connect: %s\n", error.message);
        return 1;
    }
    options.role = KSD_ROLE_RPC;
    options.authorization_mode = KSD_AUTH_CHECK;
    options.requested_scopes = 0u;
    options.lease_id = info.lease_id;
    ksd_service_info_init(&info);
    if (ksd_connect(&options, &connection, &info, &error) != KSD_STATUS_OK) {
        fprintf(stderr, "connect: %s\n", error.message);
        ksd_disconnect(lease);
        return 1;
    }
    if (target != 0u && ksd_window_set_state(connection, target,
            KSD_WINDOW_STATE_UNMINIMIZED, &error) != KSD_STATUS_OK) {
        fprintf(stderr, "unminimize: %s\n", error.message);
        ksd_disconnect(connection);
        ksd_disconnect(lease);
        return 1;
    }
    if (ksd_window_list_json(connection, 0, &windows, &error) == KSD_STATUS_OK) {
        printf("%s\n", windows.data);
        ksd_string_clear(&windows);
    } else {
        fprintf(stderr, "window list: %s\n", error.message);
    }
    ksd_disconnect(connection);
    ksd_disconnect(lease);
    return 0;
}
