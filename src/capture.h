#ifndef KEYSHARP_DESKTOP_CAPTURE_H
#define KEYSHARP_DESKTOP_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum ksd_backend {
    KSD_BACKEND_NONE = 0,
    KSD_BACKEND_KWIN,
    KSD_BACKEND_GNOME,
    KSD_BACKEND_CINNAMON,
} ksd_backend;

const char *ksd_backend_name(ksd_backend backend);
ksd_backend ksd_backend_parse(const char *name);
ksd_backend ksd_backend_resolve(ksd_backend requested);
bool ksd_capture_area(ksd_backend backend, int output_fd,
                      int x, int y, int width, int height);
bool ksd_capture_window(ksd_backend backend, int output_fd,
                        const char *handle, bool include_decoration);
bool ksd_write_error_response(int output_fd, const char *message);

#endif
