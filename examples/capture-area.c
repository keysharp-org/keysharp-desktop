#include <keysharp_desktop/client.h>
#include <stdio.h>

int main(void)
{
    ksd_connect_options options;
    ksd_service_info info;
    ksd_error error;
    ksd_connection *connection = NULL;
    ksd_capture capture;
    uint32_t granted = 0u;
    FILE *file;

    ksd_connect_options_init(&options);
    ksd_service_info_init(&info);
    ksd_error_init(&error);
    ksd_capture_init(&capture);
    options.requested_scopes = KSD_SCOPE_SCREEN_CAPTURE;

    if (ksd_connect(&options, &connection, &info, &error) != KSD_STATUS_OK) {
        fprintf(stderr, "connect: %s\n", error.message);
        return 1;
    }
    if (ksd_authorize(connection, KSD_AUTH_REQUEST, KSD_SCOPE_SCREEN_CAPTURE,
                      &granted, &error) != KSD_STATUS_OK) {
        fprintf(stderr, "authorize: %s\n", error.message);
        ksd_disconnect(connection);
        return 1;
    }
    if (ksd_capture_area(connection, 0, 0, 400, 300, &capture, &error)
            != KSD_STATUS_OK) {
        fprintf(stderr, "capture: %s\n", error.message);
        ksd_disconnect(connection);
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
    return 0;
}
