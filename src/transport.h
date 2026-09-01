#ifndef KEYSHARP_DESKTOP_TRANSPORT_H
#define KEYSHARP_DESKTOP_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

bool ksd_write_all(int descriptor, const void *data, size_t length);
bool ksd_read_all(int descriptor, void *data, size_t length);
bool ksd_send_with_fd(int descriptor, const void *data, size_t length,
                      int passed_fd);
int ksd_receive_optional_fd(int descriptor, void *data, size_t length,
                            int *received_fd);
int ksd_make_parent_directories(const char *path, mode_t mode);

#endif
