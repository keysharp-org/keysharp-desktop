#ifndef KEYSHARP_DESKTOP_PROVIDER_H
#define KEYSHARP_DESKTOP_PROVIDER_H

#include "capture.h"

#include <stdbool.h>
#include <stdint.h>

bool ksd_provider_window_list(ksd_backend backend, int output_fd,
                              bool include_hidden);
bool ksd_provider_active_window(ksd_backend backend, int output_fd);
bool ksd_provider_window_handle_command(ksd_backend backend, int output_fd,
                                        const char *method, uint64_t handle);
bool ksd_provider_window_move_resize(ksd_backend backend, int output_fd,
                                     const char *method, uint64_t handle,
                                     int x, int y, int width, int height);
bool ksd_provider_window_integer_command(ksd_backend backend, int output_fd,
                                         const char *method, uint64_t handle,
                                         int value);
bool ksd_provider_window_boolean_command(ksd_backend backend, int output_fd,
                                         const char *method, uint64_t handle,
                                         bool value);
bool ksd_provider_clipboard_mimetypes(ksd_backend backend, int output_fd);
bool ksd_provider_clipboard_content(ksd_backend backend, int output_fd,
                                    const char *mimetype);
bool ksd_provider_clipboard_text(ksd_backend backend, int output_fd);
bool ksd_provider_watch(ksd_backend backend, int output_fd, bool clipboard);

#endif
