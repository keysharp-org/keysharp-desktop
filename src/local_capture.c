#include "local_capture.h"

#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define KSD_CAPTURE_TIMEOUT_MS 30000
#define KSD_MAX_WINDOW_HANDLE_BYTES 128u
#define KSD_CAPTURE_METADATA_SIZE 32u

static const uint8_t capture_metadata_magic[4] = { 'K', 'S', 'C', 'M' };

static GDBusConnection *session_bus;

static uint64_t monotonic_milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0u;
    return (uint64_t)now.tv_sec * 1000u
        + (uint64_t)now.tv_nsec / 1000000u;
}

static int remaining_timeout(uint64_t deadline)
{
    uint64_t now = monotonic_milliseconds();
    if (now == 0u || now >= deadline)
        return 0;
    uint64_t remaining = deadline - now;
    return remaining > (uint64_t)INT_MAX ? INT_MAX : (int)remaining;
}

static GDBusConnection *get_session_bus(GError **error)
{
    if (session_bus == NULL)
        session_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, error);
    return session_bus;
}

static bool trusted_kwin_owner(GDBusConnection *connection,
                               uint64_t deadline, char **destination,
                               pid_t *owner_pid)
{
    GError *error = NULL;
    GVariant *owner_reply = NULL;
    GVariant *pid_reply = NULL;
    GVariant *uid_reply = NULL;
    const char *owner = NULL;
    guint32 pid = 0u;
    guint32 uid = 0u;
    char proc_path[64];
    char executable[PATH_MAX + 1u];
    struct stat status;
    bool valid = false;

    owner_reply = g_dbus_connection_call_sync(connection,
        "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "GetNameOwner",
        g_variant_new("(s)", "org.kde.KWin"), G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE, remaining_timeout(deadline), NULL, &error);
    if (owner_reply == NULL)
        goto done;
    g_variant_get(owner_reply, "(&s)", &owner);
    pid_reply = g_dbus_connection_call_sync(connection,
        "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "GetConnectionUnixProcessID",
        g_variant_new("(s)", owner), G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE, remaining_timeout(deadline), NULL, &error);
    uid_reply = pid_reply == NULL ? NULL : g_dbus_connection_call_sync(
        connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "GetConnectionUnixUser",
        g_variant_new("(s)", owner), G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE, remaining_timeout(deadline), NULL, &error);
    if (pid_reply == NULL || uid_reply == NULL)
        goto done;
    g_variant_get(pid_reply, "(u)", &pid);
    g_variant_get(uid_reply, "(u)", &uid);
    int length = snprintf(proc_path, sizeof(proc_path), "/proc/%u/exe", pid);
    ssize_t executable_length = length > 0
        && (size_t)length < sizeof(proc_path)
        ? readlink(proc_path, executable, sizeof(executable) - 1u) : -1;
    if (uid != (guint32)getuid() || pid == 0u || executable_length <= 0
        || (size_t)executable_length >= sizeof(executable)
        || stat(proc_path, &status) != 0 || !S_ISREG(status.st_mode)
        || status.st_uid != 0u
        || (status.st_mode & (S_IWGRP | S_IWOTH)) != 0)
        goto done;
    executable[executable_length] = '\0';
    const char *basename = strrchr(executable, '/');
    basename = basename == NULL ? executable : basename + 1u;
    valid = strcmp(basename, "kwin_wayland") == 0;
    if (valid) {
        *destination = g_strdup(owner);
        if (*destination == NULL)
            valid = false;
        else
            *owner_pid = (pid_t)pid;
    }

done:
    if (uid_reply != NULL)
        g_variant_unref(uid_reply);
    if (pid_reply != NULL)
        g_variant_unref(pid_reply);
    if (owner_reply != NULL)
        g_variant_unref(owner_reply);
    if (error != NULL)
        g_error_free(error);
    return valid;
}

static bool yama_ptracer_exception_available(void)
{
    char value[4] = { 0 };
    struct stat status;
    int descriptor = open("/proc/sys/kernel/yama/ptrace_scope",
                          O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0 || fstat(descriptor, &status) != 0
        || !S_ISREG(status.st_mode) || status.st_uid != 0u) {
        if (descriptor >= 0)
            close(descriptor);
        return false;
    }
    ssize_t length;
    do {
        length = read(descriptor, value, sizeof(value) - 1u);
    } while (length < 0 && errno == EINTR);
    close(descriptor);
    return (length == 1 || (length == 2 && value[1] == '\n'))
        && value[0] == '1';
}

static bool expose_executable_to_kwin(pid_t owner_pid)
{
    if (owner_pid <= 0 || !yama_ptracer_exception_available())
        return false;
    if (prctl(PR_SET_PTRACER, (unsigned long)owner_pid, 0, 0, 0) != 0)
        return false;
    if (prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) == 0)
        return true;
    (void)prctl(PR_SET_PTRACER, 0, 0, 0, 0);
    return false;
}

static bool hide_executable_from_kwin(void)
{
    bool hidden = prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) == 0;
    bool cleared = prctl(PR_SET_PTRACER, 0, 0, 0, 0) == 0;
    return hidden && cleared;
}

static bool valid_rectangle(int32_t x, int32_t y,
                            uint32_t width, uint32_t height)
{
    return width != 0u && height != 0u
        && width <= KSD_MAX_CAPTURE_DIMENSION
        && height <= KSD_MAX_CAPTURE_DIMENSION
        && (uint64_t)width * height <= KSD_MAX_CAPTURE_PIXELS
        && (int64_t)x + width <= INT32_MAX
        && (int64_t)y + height <= INT32_MAX;
}

static bool parse_request(const ksd_frame *request, bool *window,
                          int32_t *x, int32_t *y,
                          uint32_t *width, uint32_t *height,
                          bool *include_decoration,
                          char handle[KSD_MAX_WINDOW_HANDLE_BYTES + 1u])
{
    ksd_cursor cursor;
    ksd_cursor_init(&cursor, request->payload, request->payload_length);
    if (request->opcode == KSD_OP_CAPTURE_AREA) {
        *window = false;
        return ksd_cursor_i32(&cursor, x) && ksd_cursor_i32(&cursor, y)
            && ksd_cursor_u32(&cursor, width)
            && ksd_cursor_u32(&cursor, height)
            && ksd_cursor_finished(&cursor)
            && valid_rectangle(*x, *y, *width, *height);
    }
    if (request->opcode != KSD_OP_CAPTURE_WINDOW)
        return false;
    uint32_t flags;
    uint32_t length;
    const uint8_t *bytes;
    if (!ksd_cursor_u32(&cursor, &flags)
        || !ksd_cursor_u32(&cursor, &length)
        || (flags & ~KSD_CAPTURE_WINDOW_INCLUDE_DECORATION) != 0u
        || length == 0u || length > KSD_MAX_WINDOW_HANDLE_BYTES
        || !ksd_cursor_bytes(&cursor, length, &bytes)
        || !ksd_cursor_finished(&cursor)
        || !ksd_utf8_valid(bytes, length, false))
        return false;
    memcpy(handle, bytes, length);
    handle[length] = '\0';
    *window = true;
    *include_decoration =
        (flags & KSD_CAPTURE_WINDOW_INCLUDE_DECORATION) != 0u;
    return true;
}

bool ksd_local_capture_request_valid(const ksd_frame *request)
{
    bool window;
    bool include_decoration;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    char handle[KSD_MAX_WINDOW_HANDLE_BYTES + 1u];
    return request != NULL
        && parse_request(request, &window, &x, &y, &width, &height,
                         &include_decoration, handle);
}

bool ksd_capture_pipe_valid(const int descriptors[2])
{
    struct stat read_status;
    struct stat write_status;
    int read_flags;
    int write_flags;
    return descriptors != NULL && descriptors[0] >= 0 && descriptors[1] >= 0
        && descriptors[0] != descriptors[1]
        && fstat(descriptors[0], &read_status) == 0
        && fstat(descriptors[1], &write_status) == 0
        && S_ISFIFO(read_status.st_mode) && S_ISFIFO(write_status.st_mode)
        && read_status.st_dev == write_status.st_dev
        && read_status.st_ino == write_status.st_ino
        && read_status.st_uid == 0u && write_status.st_uid == 0u
        && read_status.st_gid == 0u && write_status.st_gid == 0u
        && (read_status.st_mode & 0777u) == 0u
        && (write_status.st_mode & 0777u) == 0u
        && (read_flags = fcntl(descriptors[0], F_GETFL)) >= 0
        && (write_flags = fcntl(descriptors[1], F_GETFL)) >= 0
        && (read_flags & O_ACCMODE) == O_RDONLY
        && (write_flags & O_ACCMODE) == O_WRONLY
        && (read_flags & O_NONBLOCK) != 0;
}

bool ksd_capture_spool_valid(int descriptor)
{
    struct stat status;
    int flags;
    return descriptor >= 0 && fstat(descriptor, &status) == 0
        && S_ISREG(status.st_mode) && status.st_uid == 0u
        && status.st_gid == 0u && (status.st_mode & 0777u) == 0u
        && status.st_size >= 0
        && (uint64_t)status.st_size <= KSD_MAX_CAPTURE_BYTES
        && (flags = fcntl(descriptor, F_GETFL)) >= 0
        && (flags & O_ACCMODE) == O_RDWR
        && fcntl(descriptor, F_GET_SEALS) >= 0;
}

bool ksd_capture_child_endpoints_valid(int write_descriptor,
                                       int metadata_descriptor)
{
    struct stat status;
    int flags;
    int socket_type = 0;
    socklen_t socket_type_length = sizeof(socket_type);
    if (write_descriptor < 0 || metadata_descriptor < 0
        || write_descriptor == metadata_descriptor
        || fstat(write_descriptor, &status) != 0
        || !S_ISFIFO(status.st_mode) || status.st_uid != 0u
        || status.st_gid != 0u || (status.st_mode & 0777u) != 0u
        || (flags = fcntl(write_descriptor, F_GETFL)) < 0
        || (flags & O_ACCMODE) != O_WRONLY
        || getsockopt(metadata_descriptor, SOL_SOCKET, SO_TYPE,
                      &socket_type, &socket_type_length) != 0
        || socket_type_length != sizeof(socket_type)
        || socket_type != SOCK_SEQPACKET)
        return false;
    for (int descriptor = 0; descriptor < 64; descriptor++) {
        if (descriptor == write_descriptor
            || descriptor == metadata_descriptor)
            continue;
        errno = 0;
        if (fcntl(descriptor, F_GETFD) >= 0 || errno != EBADF)
            return false;
    }
    return true;
}

static bool wait_for_pipe(int descriptor, uint64_t deadline)
{
    for (;;) {
        int timeout = remaining_timeout(deadline);
        if (timeout <= 0)
            return false;
        struct pollfd item = {
            .fd = descriptor,
            .events = POLLIN | POLLHUP | POLLERR,
        };
        int ready = poll(&item, 1u, timeout);
        if (ready < 0 && errno == EINTR)
            continue;
        return ready > 0 && (item.revents & (POLLIN | POLLHUP)) != 0
            && (item.revents & (POLLERR | POLLNVAL)) == 0;
    }
}

bool ksd_capture_pipe_drain_until(int pipe_descriptor,
                                  int spool_descriptor,
                                  uint64_t deadline,
                                  uint32_t *length)
{
    uint64_t total = 0u;
    if (length == NULL || !ksd_capture_spool_valid(spool_descriptor)
        || ftruncate(spool_descriptor, 0) != 0
        || lseek(spool_descriptor, 0, SEEK_SET) != 0)
        return false;
    for (;;) {
        size_t capacity = (size_t)(KSD_MAX_CAPTURE_BYTES + 1u - total);
        size_t chunk = capacity > 1024u * 1024u
            ? 1024u * 1024u : capacity;
        ssize_t count = splice(pipe_descriptor, NULL, spool_descriptor, NULL,
                               chunk, SPLICE_F_MOVE | SPLICE_F_MORE
                                      | SPLICE_F_NONBLOCK);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!wait_for_pipe(pipe_descriptor, deadline))
                return false;
            continue;
        }
        if (count < 0)
            return false;
        if (count == 0) {
            if (total > KSD_MAX_CAPTURE_BYTES)
                return false;
            *length = (uint32_t)total;
            return true;
        }
        total += (uint64_t)count;
        if (total > KSD_MAX_CAPTURE_BYTES)
            return false;
    }
}

static bool read_exact(int descriptor, void *data, size_t length,
                       uint64_t deadline)
{
    uint8_t *cursor = data;
    while (length != 0u) {
        ssize_t count = read(descriptor, cursor, length);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!wait_for_pipe(descriptor, deadline))
                return false;
            continue;
        }
        if (count <= 0)
            return false;
        cursor += (size_t)count;
        length -= (size_t)count;
    }
    return true;
}

static bool require_pipe_eof(int descriptor, uint64_t deadline)
{
    uint8_t unexpected;
    for (;;) {
        ssize_t count = read(descriptor, &unexpected, sizeof(unexpected));
        if (count == 0)
            return true;
        if (count > 0)
            return false;
        if (errno == EINTR)
            continue;
        if ((errno == EAGAIN || errno == EWOULDBLOCK)
            && wait_for_pipe(descriptor, deadline))
            continue;
        return false;
    }
}

static unsigned bytes_per_pixel(uint32_t format)
{
    switch (format) {
        case 4u:
        case 5u:
        case 6u:
        case 16u:
        case 17u:
        case 18u:
            return 4u;
        case 13u:
        case 29u:
            return 3u;
        default:
            return 0u;
    }
}

static uint8_t premultiply(uint8_t value, uint8_t alpha)
{
    return (uint8_t)(((unsigned)value * alpha + 127u) / 255u);
}

static void convert_row(const uint8_t *source, uint8_t *destination,
                        uint32_t width, uint32_t format)
{
    for (uint32_t index = 0u; index < width; index++) {
        const uint8_t *input;
        uint8_t *output = destination + (size_t)index * 4u;
        if (format == 13u || format == 29u) {
            input = source + (size_t)index * 3u;
            if (format == 13u) {
                output[0] = input[2];
                output[1] = input[1];
                output[2] = input[0];
            } else {
                output[0] = input[0];
                output[1] = input[1];
                output[2] = input[2];
            }
            output[3] = 255u;
            continue;
        }
        input = source + (size_t)index * 4u;
        if (format == 4u || format == 5u || format == 6u) {
            output[0] = input[0];
            output[1] = input[1];
            output[2] = input[2];
            output[3] = format == 4u ? 255u : input[3];
        } else {
            output[0] = input[2];
            output[1] = input[1];
            output[2] = input[0];
            output[3] = format == 16u ? 255u : input[3];
        }
        if (format == 5u || format == 17u) {
            output[0] = premultiply(output[0], output[3]);
            output[1] = premultiply(output[1], output[3]);
            output[2] = premultiply(output[2], output[3]);
        }
    }
}

static bool encode_capture(int image_fd, uint32_t width, uint32_t height,
                           uint32_t source_stride, uint32_t source_format,
                           uint64_t deadline, ksd_operation_result *result)
{
    unsigned source_bpp = bytes_per_pixel(source_format);
    uint64_t source_length = (uint64_t)source_stride * height;
    uint64_t output_stride = (uint64_t)width * 4u;
    uint64_t output_length = output_stride * height;
    if (width == 0u || height == 0u
        || width > KSD_MAX_CAPTURE_DIMENSION
        || height > KSD_MAX_CAPTURE_DIMENSION
        || (uint64_t)width * height > KSD_MAX_CAPTURE_PIXELS
        || source_bpp == 0u
        || source_stride < (uint64_t)width * source_bpp
        || source_length == 0u || source_length > KSD_MAX_CAPTURE_BYTES
        || output_length == 0u || output_length > KSD_MAX_CAPTURE_BYTES
        || output_stride > UINT32_MAX)
        return false;

    uint8_t *tail = malloc(20u + (size_t)output_length);
    uint8_t *row = malloc(source_stride);
    if (tail == NULL || row == NULL) {
        free(tail);
        free(row);
        return false;
    }
    ksd_encode_u16(tail, KSD_CAPTURE_FORMAT_BGRA8_PREMULTIPLIED);
    ksd_encode_u16(tail + 2u, 0u);
    ksd_encode_u32(tail + 4u, width);
    ksd_encode_u32(tail + 8u, height);
    ksd_encode_u32(tail + 12u, (uint32_t)output_stride);
    ksd_encode_u32(tail + 16u, (uint32_t)output_length);
    for (uint32_t y = 0u; y < height; y++) {
        if (remaining_timeout(deadline) <= 0
            || !read_exact(image_fd, row, source_stride, deadline)) {
            free(tail);
            free(row);
            return false;
        }
        convert_row(row, tail + 20u + (size_t)y * (size_t)output_stride,
                    width, source_format);
    }
    free(row);
    if (!require_pipe_eof(image_fd, deadline)) {
        free(tail);
        return false;
    }
    return ksd_result_take(result, tail,
                           20u + (uint32_t)output_length);
}

static bool capture_metadata_valid(uint32_t width, uint32_t height,
                                   uint32_t stride, uint32_t format)
{
    unsigned source_bpp = bytes_per_pixel(format);
    return width != 0u && height != 0u
        && width <= KSD_MAX_CAPTURE_DIMENSION
        && height <= KSD_MAX_CAPTURE_DIMENSION
        && (uint64_t)width * height <= KSD_MAX_CAPTURE_PIXELS
        && source_bpp != 0u
        && stride >= (uint64_t)width * source_bpp
        && (uint64_t)stride * height <= KSD_MAX_CAPTURE_BYTES;
}

static bool send_capture_metadata(int descriptor, uint32_t status,
                                  uint32_t width, uint32_t height,
                                  uint32_t stride, uint32_t format)
{
    uint8_t message[KSD_CAPTURE_METADATA_SIZE] = { 0 };
    memcpy(message, capture_metadata_magic, sizeof(capture_metadata_magic));
    ksd_encode_u16(message + 4u, 1u);
    ksd_encode_u32(message + 8u, status);
    ksd_encode_u32(message + 12u, width);
    ksd_encode_u32(message + 16u, height);
    ksd_encode_u32(message + 20u, stride);
    ksd_encode_u32(message + 24u, format);
    ssize_t written;
    do {
        written = send(descriptor, message, sizeof(message), MSG_NOSIGNAL);
    } while (written < 0 && errno == EINTR);
    return written == (ssize_t)sizeof(message);
}

static int kwin_capture_child(bool window, bool include_decoration,
                              int32_t x, int32_t y,
                              uint32_t width, uint32_t height,
                              const char *handle, int image_write_fd,
                              int metadata_fd, uint64_t deadline)
{
    GError *error = NULL;
    GDBusConnection *connection = NULL;
    GUnixFDList *fd_list = NULL;
    GVariantBuilder options;
    GVariant *reply = NULL;
    GVariant *results = NULL;
    gchar *type = NULL;
    char *kwin_destination = NULL;
    pid_t kwin_pid = 0;
    guint32 result_width = 0u;
    guint32 result_height = 0u;
    guint32 stride = 0u;
    guint32 format = 0u;
    uint32_t status = KSD_STATUS_UNAVAILABLE;
    bool executable_exposed = false;

    connection = get_session_bus(&error);
    if (connection == NULL
        || !trusted_kwin_owner(connection, deadline, &kwin_destination,
                               &kwin_pid))
        goto done;
    fd_list = g_unix_fd_list_new();
    int fd_handle = fd_list == NULL ? -1
        : g_unix_fd_list_append(fd_list, image_write_fd, &error);
    close(image_write_fd);
    image_write_fd = -1;
    if (fd_handle < 0)
        goto done;
    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&options, "{sv}", "include-cursor",
                          g_variant_new_boolean(FALSE));
    g_variant_builder_add(&options, "{sv}", "native-resolution",
                          g_variant_new_boolean(FALSE));
    if (window)
        g_variant_builder_add(&options, "{sv}", "include-decoration",
                              g_variant_new_boolean(include_decoration));
    if (remaining_timeout(deadline) <= 0
        || !expose_executable_to_kwin(kwin_pid))
        goto done;
    executable_exposed = true;
    reply = g_dbus_connection_call_with_unix_fd_list_sync(connection,
        kwin_destination, "/org/kde/KWin/ScreenShot2",
        "org.kde.KWin.ScreenShot2", window ? "CaptureWindow" : "CaptureArea",
        window ? g_variant_new("(sa{sv}h)", handle, &options, fd_handle)
               : g_variant_new("(iiuua{sv}h)", x, y, width, height,
                               &options, fd_handle),
        G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE,
        remaining_timeout(deadline), fd_list, NULL, NULL, &error);
    if (!hide_executable_from_kwin())
        _exit(1);
    executable_exposed = false;
    g_object_unref(fd_list);
    fd_list = NULL;
    if (reply == NULL)
        goto done;
    g_variant_get(reply, "(@a{sv})", &results);
    if (!g_variant_lookup(results, "type", "s", &type)
        || strcmp(type, "raw") != 0
        || !g_variant_lookup(results, "width", "u", &result_width)
        || !g_variant_lookup(results, "height", "u", &result_height)
        || !g_variant_lookup(results, "stride", "u", &stride)
        || !g_variant_lookup(results, "format", "u", &format)
        || (!window && (result_width != width || result_height != height))
        || !capture_metadata_valid(result_width, result_height,
                                   stride, format))
        goto done;
    status = KSD_STATUS_OK;

done:
    if (executable_exposed && !hide_executable_from_kwin())
        _exit(1);
    if (fd_list != NULL)
        g_object_unref(fd_list);
    if (image_write_fd >= 0)
        close(image_write_fd);
    if (status != KSD_STATUS_OK
        && (remaining_timeout(deadline) <= 0
            || (error != NULL && g_error_matches(error, G_IO_ERROR,
                                                  G_IO_ERROR_TIMED_OUT))))
        status = KSD_STATUS_TIMEOUT;
    bool sent = send_capture_metadata(metadata_fd, status,
        status == KSD_STATUS_OK ? result_width : 0u,
        status == KSD_STATUS_OK ? result_height : 0u,
        status == KSD_STATUS_OK ? stride : 0u,
        status == KSD_STATUS_OK ? format : 0u);
    g_free(kwin_destination);
    g_free(type);
    if (results != NULL)
        g_variant_unref(results);
    if (reply != NULL)
        g_variant_unref(reply);
    if (error != NULL)
        g_error_free(error);
    close(metadata_fd);
    return sent ? 0 : 1;
}

static bool receive_capture_metadata(int descriptor, uint64_t deadline,
                                     uint32_t *status, uint32_t *width,
                                     uint32_t *height, uint32_t *stride,
                                     uint32_t *format)
{
    uint8_t message[KSD_CAPTURE_METADATA_SIZE];
    for (;;) {
        int timeout = remaining_timeout(deadline);
        if (timeout <= 0)
            return false;
        struct pollfd item = {
            .fd = descriptor,
            .events = POLLIN | POLLHUP | POLLERR,
        };
        int ready = poll(&item, 1u, timeout);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0 || (item.revents & (POLLERR | POLLNVAL)) != 0
            || (item.revents & POLLIN) == 0)
            return false;
        ssize_t count;
        do {
            count = recv(descriptor, message, sizeof(message), MSG_DONTWAIT);
        } while (count < 0 && errno == EINTR);
        if (count != (ssize_t)sizeof(message)
            || memcmp(message, capture_metadata_magic,
                      sizeof(capture_metadata_magic)) != 0
            || ksd_decode_u16(message + 4u) != 1u
            || ksd_decode_u16(message + 6u) != 0u
            || ksd_decode_u32(message + 28u) != 0u)
            return false;
        *status = ksd_decode_u32(message + 8u);
        *width = ksd_decode_u32(message + 12u);
        *height = ksd_decode_u32(message + 16u);
        *stride = ksd_decode_u32(message + 20u);
        *format = ksd_decode_u32(message + 24u);
        return *status == KSD_STATUS_OK || *status == KSD_STATUS_TIMEOUT
            || *status == KSD_STATUS_UNAVAILABLE;
    }
}

static bool wait_capture_child(pid_t child, uint64_t deadline, bool *reaped)
{
    *reaped = false;
    for (;;) {
        int status;
        pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child) {
            *reaped = true;
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        if (waited < 0 && errno != EINTR)
            return false;
        if (remaining_timeout(deadline) <= 0)
            return false;
        struct timespec pause = { .tv_nsec = 1000000L };
        (void)nanosleep(&pause, NULL);
    }
}

static void terminate_capture_child(pid_t child)
{
    if (child <= 0)
        return;
    (void)kill(child, SIGKILL);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
    }
}

void ksd_local_capture_execute(const ksd_frame *request,
                               int capture_read_fd, int capture_write_fd,
                               int capture_spool_fd,
                               ksd_operation_result *result)
{
    bool window;
    bool include_decoration = false;
    int32_t x = 0;
    int32_t y = 0;
    uint32_t width = 0u;
    uint32_t height = 0u;
    char handle[KSD_MAX_WINDOW_HANDLE_BYTES + 1u] = { 0 };
    uint32_t result_width = 0u;
    uint32_t result_height = 0u;
    uint32_t stride = 0u;
    uint32_t format = 0u;
    uint32_t child_status = KSD_STATUS_UNAVAILABLE;
    uint32_t drained_length = 0u;
    int image_fd = capture_read_fd;
    int image_write_fd = capture_write_fd;
    int spool_fd = capture_spool_fd;
    int metadata[2] = { -1, -1 };
    int child_write = -1;
    int child_metadata = -1;
    pid_t child = -1;
    uint64_t deadline = monotonic_milliseconds() + KSD_CAPTURE_TIMEOUT_MS;
    const int capture_pipe[2] = { image_fd, image_write_fd };
    bool drain_valid = false;
    bool metadata_valid = false;
    bool child_valid = false;

    ksd_result_init(result);
    if (!ksd_capture_pipe_valid(capture_pipe)
        || !ksd_capture_spool_valid(spool_fd)
        || !parse_request(request, &window, &x, &y, &width, &height,
                       &include_decoration, handle)) {
        ksd_result_error(result, KSD_STATUS_INVALID_REQUEST, 0u,
                         "invalid KWin capture payload");
        goto done;
    }
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC,
                   0, metadata) != 0)
        goto failed;
    child_write = fcntl(image_write_fd, F_DUPFD_CLOEXEC, 10);
    child_metadata = fcntl(metadata[1], F_DUPFD_CLOEXEC, 10);
    if (child_write < 0 || child_metadata < 0)
        goto failed;
    pid_t expected_parent = getpid();
    child = fork();
    if (child == 0) {
        (void)close(0);
        (void)close(1);
        (void)close(2);
        if (dup2(child_write, 3) < 0 || dup2(child_metadata, 4) < 0)
            _exit(1);
        if (close_range(5u, UINT_MAX, 0) != 0
            || prctl(PR_SET_PDEATHSIG, SIGKILL) != 0
            || getppid() != expected_parent
            || !ksd_capture_child_endpoints_valid(3, 4))
            _exit(1);
        _exit(kwin_capture_child(window, include_decoration,
            x, y, width, height, handle, 3, 4, deadline));
    }
    if (child < 0)
        goto failed;
    close(child_write);
    child_write = -1;
    close(child_metadata);
    child_metadata = -1;
    close(metadata[1]);
    metadata[1] = -1;
    close(image_write_fd);
    image_write_fd = -1;
    drain_valid = ksd_capture_pipe_drain_until(image_fd, spool_fd,
                                                deadline, &drained_length);
    close(image_fd);
    image_fd = -1;
    metadata_valid = receive_capture_metadata(metadata[0], deadline,
        &child_status, &result_width, &result_height, &stride, &format);
    bool child_reaped = false;
    child_valid = wait_capture_child(child, deadline, &child_reaped);
    if (child_reaped)
        child = -1;
    if (!drain_valid || !metadata_valid || !child_valid
        || child_status != KSD_STATUS_OK
        || !capture_metadata_valid(result_width, result_height,
                                   stride, format)
        || (uint64_t)stride * result_height != drained_length
        || fcntl(spool_fd, F_ADD_SEALS,
                 F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW
                     | F_SEAL_WRITE) != 0
        || lseek(spool_fd, 0, SEEK_SET) != 0
        || !encode_capture(spool_fd, result_width, result_height,
                           stride, format, deadline, result))
        goto failed;
    goto done;

failed:
    if (result->status == KSD_STATUS_OK) {
        struct stat spool_status;
        uint32_t failure_status = metadata_valid
            && child_status != KSD_STATUS_OK
            ? child_status : KSD_STATUS_UNAVAILABLE;
        if (remaining_timeout(deadline) <= 0)
            failure_status = KSD_STATUS_TIMEOUT;
        else if (!drain_valid && fstat(spool_fd, &spool_status) == 0
                 && (uint64_t)spool_status.st_size
                    > KSD_MAX_CAPTURE_BYTES)
            failure_status = KSD_STATUS_RESOURCE_EXHAUSTED;
        ksd_result_error(result, failure_status, 0u,
            failure_status == KSD_STATUS_TIMEOUT
                ? "KWin capture timed out" : "KWin capture failed");
    }
done:
    if (child > 0)
        terminate_capture_child(child);
    if (child_write >= 0)
        close(child_write);
    if (child_metadata >= 0)
        close(child_metadata);
    if (metadata[0] >= 0)
        close(metadata[0]);
    if (metadata[1] >= 0)
        close(metadata[1]);
    if (image_fd >= 0)
        close(image_fd);
    if (image_write_fd >= 0)
        close(image_write_fd);
    if (spool_fd >= 0)
        close(spool_fd);
}

bool ksd_capture_tail_valid(const void *tail, uint32_t tail_length)
{
    const uint8_t *bytes = tail;
    if (tail == NULL || tail_length < 20u)
        return false;
    uint16_t format = ksd_decode_u16(bytes);
    uint16_t reserved = ksd_decode_u16(bytes + 2u);
    uint32_t width = ksd_decode_u32(bytes + 4u);
    uint32_t height = ksd_decode_u32(bytes + 8u);
    uint32_t stride = ksd_decode_u32(bytes + 12u);
    uint32_t length = ksd_decode_u32(bytes + 16u);
    if (reserved != 0u || width == 0u || height == 0u
        || width > KSD_MAX_CAPTURE_DIMENSION
        || height > KSD_MAX_CAPTURE_DIMENSION
        || (uint64_t)width * height > KSD_MAX_CAPTURE_PIXELS
        || length == 0u || length > KSD_MAX_CAPTURE_BYTES
        || tail_length != 20u + length)
        return false;
    if (format == KSD_CAPTURE_FORMAT_PNG)
        return stride == 0u;
    return format == KSD_CAPTURE_FORMAT_BGRA8_PREMULTIPLIED
        && stride >= (uint64_t)width * 4u
        && (uint64_t)stride * height == length;
}
