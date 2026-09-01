#include "transport.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define KSD_PATH_CAPACITY 4096u

bool ksd_write_all(int descriptor, const void *data, size_t length)
{
    const uint8_t *cursor = data;
    while (length != 0u) {
        ssize_t count = write(descriptor, cursor, length);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        cursor += (size_t)count;
        length -= (size_t)count;
    }
    return true;
}

bool ksd_read_all(int descriptor, void *data, size_t length)
{
    uint8_t *cursor = data;
    while (length != 0u) {
        ssize_t count = read(descriptor, cursor, length);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        cursor += (size_t)count;
        length -= (size_t)count;
    }
    return true;
}

bool ksd_send_with_fd(int descriptor, const void *data, size_t length,
                      int passed_fd)
{
    uint8_t control[CMSG_SPACE(sizeof(int))];
    struct iovec iov = {
        .iov_base = (void *)data,
        .iov_len = length,
    };
    struct msghdr message;
    ssize_t count;

    if (descriptor < 0 || data == NULL || length == 0u || passed_fd < 0)
        return false;
    memset(&message, 0, sizeof(message));
    memset(control, 0, sizeof(control));
    message.msg_iov = &iov;
    message.msg_iovlen = 1u;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(header), &passed_fd, sizeof(passed_fd));
    do {
        count = sendmsg(descriptor, &message, MSG_NOSIGNAL);
    } while (count < 0 && errno == EINTR);
    if (count <= 0 || (size_t)count > length)
        return false;
    return (size_t)count == length
        || ksd_write_all(descriptor, (const uint8_t *)data + (size_t)count,
                         length - (size_t)count);
}

int ksd_receive_optional_fd(int descriptor, void *data, size_t length,
                            int *received_fd)
{
    uint8_t control[CMSG_SPACE(sizeof(int) * 16u)];
    struct iovec iov = { .iov_base = data, .iov_len = length };
    struct msghdr message;
    ssize_t count;
    int first_fd = -1;
    size_t fd_count = 0u;
    bool malformed = false;

    if (descriptor < 0 || data == NULL || length == 0u
        || received_fd == NULL) {
        errno = EINVAL;
        return -1;
    }
    *received_fd = -1;
    memset(&message, 0, sizeof(message));
    memset(control, 0, sizeof(control));
    message.msg_iov = &iov;
    message.msg_iovlen = 1u;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    do {
        count = recvmsg(descriptor, &message, MSG_CMSG_CLOEXEC | MSG_WAITALL);
    } while (count < 0 && errno == EINTR);
    for (struct cmsghdr *header = CMSG_FIRSTHDR(&message);
         header != NULL; header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level != SOL_SOCKET
            || header->cmsg_type != SCM_RIGHTS
            || header->cmsg_len < CMSG_LEN(0u)) {
            malformed = true;
            continue;
        }
        size_t payload = header->cmsg_len - CMSG_LEN(0u);
        if (payload % sizeof(int) != 0u) {
            malformed = true;
            continue;
        }
        for (size_t offset = 0u; offset < payload; offset += sizeof(int)) {
            int value;
            memcpy(&value, CMSG_DATA(header) + offset, sizeof(value));
            if (fd_count++ == 0u)
                first_fd = value;
            else
                close(value);
        }
    }
    if (count != (ssize_t)length
        || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0
        || malformed || fd_count > 1u) {
        if (first_fd >= 0)
            close(first_fd);
        errno = EPROTO;
        return -1;
    }
    *received_fd = fd_count == 1u ? first_fd : -1;
    return 0;
}

int ksd_make_parent_directories(const char *path, mode_t mode)
{
    char copy[KSD_PATH_CAPACITY];
    if (path == NULL || strlen(path) >= sizeof(copy)) {
        errno = EINVAL;
        return -1;
    }
    memcpy(copy, path, strlen(path) + 1u);
    for (char *cursor = copy + 1u; *cursor != '\0'; cursor++) {
        if (*cursor != '/')
            continue;
        *cursor = '\0';
        if (mkdir(copy, mode) != 0 && errno != EEXIST)
            return -1;
        *cursor = '/';
    }
    return 0;
}
