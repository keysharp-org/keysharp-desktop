#include "common.h"
#include "keysharp_desktop/protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/if_alg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct ksd_sha256 {
    int algorithm_fd;
    int operation_fd;
} ksd_sha256;

int ksd_format_capability_names(uint32_t capabilities, char *destination,
                                size_t capacity)
{
    static const struct {
        uint32_t capability;
        const char *name;
    } names[] = {
        { KSD_CAP_SCREEN_CAPTURE, "Screen Capture" },
        { KSD_CAP_WINDOW_MONITORING, "Window Monitoring" },
        { KSD_CAP_WINDOW_CONTROL, "Window Control" },
        { KSD_CAP_AUDIO_CAPTURE, "Audio Capture" },
        { KSD_CAP_CAMERA_CAPTURE, "Camera Capture" },
        { KSD_CAP_CLIPBOARD_MONITORING, "Clipboard Monitoring" },
    };
    size_t used = 0u;

    if (destination == NULL || capacity == 0u || capabilities == 0u
        || (capabilities & ~KSD_CAP_ALL) != 0u) {
        errno = EINVAL;
        return -1;
    }
    destination[0] = '\0';
    for (size_t index = 0u; index < sizeof(names) / sizeof(names[0]); index++) {
        if ((capabilities & names[index].capability) == 0u)
            continue;
        int written = snprintf(destination + used, capacity - used, "%s%s",
                               used == 0u ? "" : ", ", names[index].name);
        if (written < 0 || (size_t)written >= capacity - used) {
            destination[0] = '\0';
            errno = ENOSPC;
            return -1;
        }
        used += (size_t)written;
    }
    return 0;
}

void ksd_sanitize_display_text(const char *source, char *destination,
                               size_t capacity)
{
    size_t index = 0u;

    if (destination == NULL || capacity == 0u)
        return;
    if (source != NULL) {
        while (source[index] != '\0' && index + 1u < capacity) {
            unsigned char value = (unsigned char)source[index];
            destination[index] = value < 0x20u || value == 0x7fu
                ? '?'
                : (char)value;
            index++;
        }
    }
    destination[index] = '\0';
}

bool ksd_write_all(int fd, const void *data, size_t length)
{
    const unsigned char *cursor = data;

    while (length != 0u) {
        ssize_t written = write(fd, cursor, length);

        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return false;

        cursor += (size_t)written;
        length -= (size_t)written;
    }

    return true;
}

bool ksd_read_all(int fd, void *data, size_t length)
{
    unsigned char *cursor = data;

    while (length != 0u) {
        ssize_t count = read(fd, cursor, length);

        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;

        cursor += (size_t)count;
        length -= (size_t)count;
    }

    return true;
}

int ksd_receive_optional_fd(int fd, void *data, size_t length, int *received_fd)
{
    unsigned char control[CMSG_SPACE(sizeof(int) * 16u)];
    struct iovec iov = { .iov_base = data, .iov_len = length };
    struct msghdr message;
    ssize_t count;
    int first_fd = -1;
    size_t fd_count = 0u;
    bool malformed = false;

    if (fd < 0 || data == NULL || length == 0u || received_fd == NULL) {
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
        count = recvmsg(fd, &message, MSG_CMSG_CLOEXEC | MSG_WAITALL);
    } while (count < 0 && errno == EINTR);

    unsigned char *cursor = control;
    size_t remaining = message.msg_controllen;
    while (remaining >= sizeof(struct cmsghdr)) {
        struct cmsghdr *header = (struct cmsghdr *)cursor;
        size_t header_length = CMSG_LEN(0);
        if (header->cmsg_len < header_length) {
            malformed = true;
            break;
        }

        size_t available = header->cmsg_len < remaining ? header->cmsg_len : remaining;
        size_t payload_length = available > header_length ? available - header_length : 0u;
        if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS) {
            if (payload_length % sizeof(int) != 0u)
                malformed = true;
            for (size_t offset = 0u; offset + sizeof(int) <= payload_length;
                 offset += sizeof(int)) {
                int received;
                memcpy(&received, CMSG_DATA(header) + offset, sizeof(received));
                if (received < 0) {
                    malformed = true;
                    continue;
                }
                if (fd_count++ == 0u)
                    first_fd = received;
                else
                    close(received);
            }
        } else {
            malformed = true;
        }

        if (header->cmsg_len > remaining) {
            malformed = true;
            break;
        }
        size_t advance = CMSG_ALIGN(header->cmsg_len);
        if (advance > remaining)
            break;
        cursor += advance;
        remaining -= advance;
    }

    if (count != (ssize_t)length || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0
        || malformed || fd_count > 1u) {
        if (first_fd >= 0)
            close(first_fd);
        errno = EPROTO;
        return -1;
    }
    *received_fd = fd_count == 1u ? first_fd : -1;
    return 0;
}

int ksd_revoke_generation_path(uid_t uid, char *path, size_t capacity)
{
    int length;
    if (path == NULL || capacity == 0u) {
        errno = EINVAL;
        return -1;
    }
    length = snprintf(path, capacity, KSD_RUNTIME_DIRECTORY "/revoke-%lu.generation",
                      (unsigned long)uid);
    return length > 0 && (size_t)length < capacity ? 0 : -1;
}

int ksd_revoke_generation_read(uid_t uid, uint64_t *generation)
{
    char path[KSD_PATH_MAX];
    struct stat info;
    uint64_t value;
    unsigned char trailing;
    int descriptor;
    ssize_t count;

    if (generation == NULL) {
        errno = EINVAL;
        return -1;
    }
    *generation = 0u;
    if (ksd_revoke_generation_path(uid, path, sizeof(path)) != 0)
        return -1;
    descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return errno == ENOENT ? 0 : -1;
    if (fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode)
        || info.st_uid != KSD_REVOKE_GENERATION_OWNER_UID
        || (info.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        close(descriptor);
        errno = EACCES;
        return -1;
    }
    count = read(descriptor, &value, sizeof(value));
    if (count != (ssize_t)sizeof(value) || read(descriptor, &trailing, 1u) != 0) {
        close(descriptor);
        errno = EPROTO;
        return -1;
    }
    close(descriptor);
    *generation = value;
    return 0;
}

uint64_t ksd_process_start_time(pid_t pid)
{
    char path[64];
    char buffer[1024];
    char *cursor;
    int fd;
    ssize_t length;

    (void)snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0u;

    length = read(fd, buffer, sizeof(buffer) - 1u);
    close(fd);
    if (length <= 0)
        return 0u;

    buffer[length] = '\0';
    cursor = strrchr(buffer, ')');
    if (cursor == NULL || cursor[1] != ' ')
        return 0u;
    cursor += 2;

    for (int field = 3; field < 22; field++) {
        cursor = strchr(cursor, ' ');
        if (cursor == NULL)
            return 0u;
        cursor++;
    }

    errno = 0;
    unsigned long long value = strtoull(cursor, NULL, 10);
    return errno == 0 ? (uint64_t)value : 0u;
}

static int sha256_begin(ksd_sha256 *context)
{
    static const struct sockaddr_alg address = {
        .salg_family = AF_ALG,
        .salg_type = "hash",
        .salg_name = "sha256",
    };

    context->algorithm_fd = socket(AF_ALG, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    context->operation_fd = -1;
    if (context->algorithm_fd < 0)
        return -1;
    if (bind(context->algorithm_fd, (const struct sockaddr *)&address, sizeof(address)) != 0)
        return -1;

    context->operation_fd = accept4(context->algorithm_fd, NULL, NULL, SOCK_CLOEXEC);
    return context->operation_fd < 0 ? -1 : 0;
}

static void sha256_end(ksd_sha256 *context)
{
    if (context->operation_fd >= 0)
        close(context->operation_fd);
    if (context->algorithm_fd >= 0)
        close(context->algorithm_fd);
    context->operation_fd = -1;
    context->algorithm_fd = -1;
}

static int sha256_update(ksd_sha256 *context, const void *data, size_t length)
{
    return send(context->operation_fd, data, length, MSG_MORE) == (ssize_t)length ? 0 : -1;
}

static int sha256_finish(ksd_sha256 *context, char output[KSD_HASH_HEX_LENGTH + 1u])
{
    static const char digits[] = "0123456789abcdef";
    unsigned char digest[32];

    if (read(context->operation_fd, digest, sizeof(digest)) != (ssize_t)sizeof(digest))
        return -1;

    for (size_t index = 0; index < sizeof(digest); index++) {
        output[index * 2u] = digits[digest[index] >> 4u];
        output[(index * 2u) + 1u] = digits[digest[index] & 0x0fu];
    }
    output[KSD_HASH_HEX_LENGTH] = '\0';
    return 0;
}

static bool protected_metadata(const struct stat *info)
{
    return info->st_uid == 0 && (info->st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

bool ksd_executable_path_is_protected(int executable_fd, const char *absolute_path)
{
    char path[KSD_PATH_MAX];
    struct stat executable;
    struct stat current;
    size_t length;

    if (executable_fd < 0 || absolute_path == NULL || absolute_path[0] != '/'
        || (length = strlen(absolute_path)) == 0u || length >= sizeof(path)
        || fstat(executable_fd, &executable) != 0 || !S_ISREG(executable.st_mode)
        || !protected_metadata(&executable))
        return false;
    (void)memcpy(path, absolute_path, length + 1u);

    if (lstat("/", &current) != 0 || !S_ISDIR(current.st_mode)
        || !protected_metadata(&current))
        return false;

    for (char *cursor = path + 1; ; cursor++) {
        if (*cursor != '/' && *cursor != '\0')
            continue;
        char saved = *cursor;
        *cursor = '\0';
        bool final = saved == '\0';
        int status = final ? stat(path, &current) : lstat(path, &current);
        bool valid = status == 0 && protected_metadata(&current)
            && (final
                ? S_ISREG(current.st_mode) && current.st_dev == executable.st_dev
                    && current.st_ino == executable.st_ino
                : S_ISDIR(current.st_mode));
        *cursor = saved;
        if (!valid)
            return false;
        if (final)
            return true;
    }
}

int ksd_hash_app_identity(const char *kind, const void *identity_bytes,
                          size_t identity_length,
                          char output[KSD_HASH_HEX_LENGTH + 1u])
{
    static const unsigned char separator = 0u;
    ksd_sha256 hash = { .algorithm_fd = -1, .operation_fd = -1 };
    int result = -1;

    if (kind == NULL || kind[0] == '\0' || identity_bytes == NULL
        || identity_length == 0u || output == NULL || sha256_begin(&hash) != 0)
        goto done;
    if (sha256_update(&hash, KSD_APP_IDENTITY_DOMAIN,
                      sizeof(KSD_APP_IDENTITY_DOMAIN) - 1u) != 0
        || sha256_update(&hash, &separator, sizeof(separator)) != 0
        || sha256_update(&hash, kind, strlen(kind)) != 0
        || sha256_update(&hash, &separator, sizeof(separator)) != 0
        || sha256_update(&hash, identity_bytes, identity_length) != 0)
        goto done;
    result = sha256_finish(&hash, output);

done:
    sha256_end(&hash);
    return result;
}

static int executable_content_hash(int descriptor,
                                   char output[KSD_HASH_HEX_LENGTH + 1u])
{
    unsigned char buffer[8192];
    ksd_sha256 hash = { .algorithm_fd = -1, .operation_fd = -1 };
    ssize_t count;
    int result = -1;

    if (lseek(descriptor, 0, SEEK_SET) < 0 || sha256_begin(&hash) != 0)
        goto done;
    while ((count = read(descriptor, buffer, sizeof(buffer))) > 0)
        if (sha256_update(&hash, buffer, (size_t)count) != 0)
            goto done;
    if (count == 0)
        result = sha256_finish(&hash, output);

done:
    sha256_end(&hash);
    return result;
}

int ksd_identify_process(pid_t pid, uid_t expected_uid, uint64_t expected_start_time,
                         ksd_process_identity *identity)
{
    char proc_path[64];
    char content_hash[KSD_HASH_HEX_LENGTH + 1u];
    struct stat process_info;
    ssize_t count;
    int executable_fd = -1;
    int result = -1;

    if (identity == NULL || pid <= 0 || expected_start_time == 0u)
        return -1;

    memset(identity, 0, sizeof(*identity));
    (void)snprintf(proc_path, sizeof(proc_path), "/proc/%ld", (long)pid);
    if (stat(proc_path, &process_info) != 0 || process_info.st_uid != expected_uid)
        return -1;
    if (ksd_process_start_time(pid) != expected_start_time)
        return -1;

    (void)snprintf(proc_path, sizeof(proc_path), "/proc/%ld/exe", (long)pid);
    executable_fd = open(proc_path, O_RDONLY | O_CLOEXEC);
    if (executable_fd < 0)
        return -1;

    count = readlink(proc_path, identity->executable, sizeof(identity->executable) - 1u);
    if (count <= 0 || (size_t)count >= sizeof(identity->executable))
        goto cleanup;
    identity->executable[count] = '\0';

    if (ksd_executable_path_is_protected(executable_fd, identity->executable)) {
        if (ksd_hash_app_identity(KSD_APP_IDENTITY_PATH_KIND,
                                  identity->executable, strlen(identity->executable),
                                  identity->hash) != 0)
            goto cleanup;
    } else {
        if (executable_content_hash(executable_fd, content_hash) != 0
            || ksd_hash_app_identity(KSD_APP_IDENTITY_SHA256_KIND,
                                     content_hash, KSD_HASH_HEX_LENGTH,
                                     identity->hash) != 0)
            goto cleanup;
    }

    if (ksd_process_start_time(pid) != expected_start_time)
        goto cleanup;

    identity->uid = expected_uid;
    identity->pid = pid;
    identity->start_time = expected_start_time;
    result = 0;

cleanup:
    if (executable_fd >= 0)
        close(executable_fd);
    return result;
}

int ksd_make_parent_directories(const char *path, mode_t mode)
{
    char copy[KSD_PATH_MAX];

    if (path == NULL || strlen(path) >= sizeof(copy))
        return -1;
    (void)snprintf(copy, sizeof(copy), "%s", path);

    for (char *cursor = copy + 1; *cursor != '\0'; cursor++) {
        if (*cursor != '/')
            continue;
        *cursor = '\0';
        if (mkdir(copy, mode) != 0 && errno != EEXIST)
            return -1;
        *cursor = '/';
    }
    return 0;
}
