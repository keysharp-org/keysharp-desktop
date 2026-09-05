#include <keysharp_desktop/client.h>
#include <stdio.h>

int main(void)
{
    ksd_connect_options options;
    ksd_service_info info;
    ksd_error error;
    ksd_connection *connection = NULL;
    ksd_string windows;
    uint32_t granted = 0u;

    ksd_connect_options_init(&options);
    ksd_service_info_init(&info);
    ksd_error_init(&error);
    ksd_string_init(&windows);
    options.requested_scopes = KSD_SCOPE_WINDOW_MONITORING;

    if (ksd_connect(&options, &connection, &info, &error) != KSD_STATUS_OK) {
        fprintf(stderr, "connect: %s\n", error.message);
        return 1;
    }
    if (ksd_authorize(connection, KSD_AUTH_REQUEST, KSD_SCOPE_WINDOW_MONITORING,
                      &granted, &error) != KSD_STATUS_OK) {
        fprintf(stderr, "authorize: %s\n", error.message);
        ksd_disconnect(connection);
        return 1;
    }
    if (ksd_window_list_json(connection, 0, &windows, &error) == KSD_STATUS_OK) {
        printf("%s\n", windows.data);
        ksd_string_clear(&windows);
    } else {
        fprintf(stderr, "window list: %s\n", error.message);
    }
    ksd_disconnect(connection);
    return 0;
}
