#ifndef KEYSHARP_DESKTOP_TRANSPORT_H
#define KEYSHARP_DESKTOP_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

uint64_t ksd_monotonic_milliseconds(void);
bool ksd_wait_until(int descriptor, short events, uint64_t deadline_ms);
ssize_t ksd_transfer_until(int descriptor, void *data, size_t length,
                           bool write_data, uint64_t deadline_ms);
bool ksd_write_all(int descriptor, const void *data, size_t length);
bool ksd_read_all(int descriptor, void *data, size_t length);
bool ksd_send_with_fd(int descriptor, const void *data, size_t length,
                      int passed_fd);
int ksd_receive_optional_fd(int descriptor, void *data, size_t length,
                            int *received_fd);
/* The same, but bounded. A registration arrives on a socket the peer controls,
 * and a peer that connects and then says nothing would otherwise hold the
 * accepting thread for ever -- and with it a slot in a pool that has one
 * thread per connection. deadline_ms is absolute, on the monotonic clock.
 *
 * Returns -1 with errno ETIMEDOUT when the deadline passes with nothing to
 * read, which the caller distinguishes from a malformed message: one is a slow
 * peer and the other is a peer sending something it should not. */
int ksd_receive_fd_until(int descriptor, void *data, size_t length,
                         uint64_t deadline_ms, int *received_fd);
int ksd_make_parent_directories(const char *path, mode_t mode);

#endif
