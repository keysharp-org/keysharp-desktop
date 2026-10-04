#include <keysharp_desktop/client.h>
#include <stdio.h>

int main(void)
{
    ksd_connect_options options;
    ksd_service_info info;
    ksd_error error;
    ksd_connection *connection = NULL;
    ksd_connection *lease = NULL;
    ksd_capture capture;
    FILE *file;

    ksd_connect_options_init(&options);
    ksd_service_info_init(&info);
    ksd_error_init(&error);
    ksd_capture_init(&capture);
    options.role = KSD_ROLE_AUTHORIZATION_LEASE;
    options.authorization_mode = KSD_AUTH_REQUEST;
    options.requested_scopes = KSD_SCOPE_SCREEN_CAPTURE;

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
    if (ksd_capture_area(connection, 0, 0, 400, 300, &capture, &error)
            != KSD_STATUS_OK) {
        fprintf(stderr, "capture: %s\n", error.message);
        ksd_disconnect(connection);
        ksd_disconnect(lease);
        return 1;
    }
    if (capture.format == KSD_CAPTURE_FORMAT_PNG
        && (file = fopen("shot.png", "wb")) != NULL) {
        fwrite(capture.data.data, 1, capture.data.length, file);
        fclose(file);
        printf("wrote shot.png (%ux%u)\n", capture.width, capture.height);
    }
    ksd_capture_clear(&capture);
    ksd_disconnect(connection);
    ksd_disconnect(lease);
    return 0;
}
