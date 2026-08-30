#include "capture.h"

#include "common.h"
#include "keysharp_desktop/protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define KSD_CAPTURE_TIMEOUT_MS 30000

typedef struct extension_target {
    const char *name;
    const char *path;
    const char *interface;
} extension_target;

static const extension_target gnome_target = {
    "io.github.keysharp.GnomeShell",
    "/io/github/keysharp/GnomeShell",
    "io.github.keysharp.GnomeShell1",
};
static const extension_target cinnamon_target = {
    "io.github.keysharp.CinnamonShell",
    "/io/github/keysharp/CinnamonShell",
    "io.github.keysharp.CinnamonShell1",
};
static GDBusConnection *session_bus;

const char *ksd_backend_name(ksd_backend backend)
{
    switch (backend) {
        case KSD_BACKEND_KWIN: return "kwin";
        case KSD_BACKEND_GNOME: return "gnome";
        case KSD_BACKEND_CINNAMON: return "cinnamon";
        default: return "none";
    }
}

ksd_backend ksd_backend_parse(const char *name)
{
    if (name == NULL || strcmp(name, "auto") == 0 || strcmp(name, "none") == 0)
        return KSD_BACKEND_NONE;
    if (strcmp(name, "kwin") == 0) return KSD_BACKEND_KWIN;
    if (strcmp(name, "gnome") == 0) return KSD_BACKEND_GNOME;
    if (strcmp(name, "cinnamon") == 0) return KSD_BACKEND_CINNAMON;
    return (ksd_backend)-1;
}

static GDBusConnection *get_session_bus(void)
{
    GError *error = NULL;

    if (session_bus != NULL)
        return session_bus;
    session_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (session_bus == NULL && error != NULL) {
        fprintf(stderr, "keysharp-desktop serve: session bus: %s\n", error->message);
        g_error_free(error);
    }
    return session_bus;
}

static bool name_has_owner(const char *name)
{
    GDBusConnection *connection = get_session_bus();
    GVariant *reply;
    GError *error = NULL;
    gboolean owned = FALSE;

    if (connection == NULL)
        return false;
    reply = g_dbus_connection_call_sync(connection,
        "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        "NameHasOwner", g_variant_new("(s)", name), G_VARIANT_TYPE("(b)"),
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &error);
    if (reply != NULL) {
        g_variant_get(reply, "(b)", &owned);
        g_variant_unref(reply);
    }
    if (error != NULL)
        g_error_free(error);
    return owned == TRUE;
}

ksd_backend ksd_backend_resolve(ksd_backend requested)
{
    if (requested == KSD_BACKEND_KWIN)
        return name_has_owner("org.kde.KWin") ? requested : KSD_BACKEND_NONE;
    if (requested == KSD_BACKEND_GNOME)
        return name_has_owner(gnome_target.name) ? requested : KSD_BACKEND_NONE;
    if (requested == KSD_BACKEND_CINNAMON)
        return name_has_owner(cinnamon_target.name) ? requested : KSD_BACKEND_NONE;

    if (name_has_owner("org.kde.KWin")) return KSD_BACKEND_KWIN;
    if (name_has_owner(gnome_target.name)) return KSD_BACKEND_GNOME;
    if (name_has_owner(cinnamon_target.name)) return KSD_BACKEND_CINNAMON;
    return KSD_BACKEND_NONE;
}

bool ksd_write_error_response(int output_fd, const char *message)
{
    uint8_t status = KSD_RESPONSE_ERROR;
    uint32_t length = message == NULL ? 0u : (uint32_t)strlen(message);
    return ksd_write_all(output_fd, &status, sizeof(status))
        && ksd_write_all(output_fd, &length, sizeof(length))
        && (length == 0u || ksd_write_all(output_fd, message, length));
}

static bool write_png_response(int output_fd, const uint8_t *data, size_t length)
{
    static const char magic[8] = { 'K', 'S', 'S', 'G', '1', 0, 0, 0 };
    uint8_t status = KSD_RESPONSE_OK;
    uint64_t byte_count = (uint64_t)length;

    return length != 0u
        && ksd_write_all(output_fd, &status, sizeof(status))
        && ksd_write_all(output_fd, magic, sizeof(magic))
        && ksd_write_all(output_fd, &byte_count, sizeof(byte_count))
        && ksd_write_all(output_fd, data, length);
}

static bool extension_capture(ksd_backend backend, int output_fd, bool window,
                              int x, int y, int width, int height, uint64_t handle)
{
    const extension_target *target = backend == KSD_BACKEND_GNOME ? &gnome_target : &cinnamon_target;
    GDBusConnection *connection = get_session_bus();
    GVariant *reply = NULL;
    GVariant *bytes = NULL;
    GError *error = NULL;
    const uint8_t *data;
    gsize length = 0u;
    bool result = false;

    if (connection == NULL)
        return ksd_write_error_response(output_fd, "session bus unavailable");
    reply = g_dbus_connection_call_sync(connection, target->name, target->path, target->interface,
        window ? "CaptureWindow" : "CaptureArea",
        window ? g_variant_new("(t)", (guint64)handle) : g_variant_new("(iiii)", x, y, width, height),
        G_VARIANT_TYPE("(ay)"), G_DBUS_CALL_FLAGS_NONE, KSD_CAPTURE_TIMEOUT_MS, NULL, &error);
    if (reply == NULL) {
        const char *message = error != NULL ? error->message : "provider capture failed";
        result = ksd_write_error_response(output_fd, message);
        goto cleanup;
    }

    g_variant_get(reply, "(@ay)", &bytes);
    data = g_variant_get_fixed_array(bytes, &length, sizeof(uint8_t));
    if (data == NULL || length == 0u)
        result = ksd_write_error_response(output_fd, "provider returned an empty capture");
    else
        result = write_png_response(output_fd, data, length);

cleanup:
    if (bytes != NULL) g_variant_unref(bytes);
    if (reply != NULL) g_variant_unref(reply);
    if (error != NULL) g_error_free(error);
    return result;
}

static int capture_file(void)
{
    char path[] = "/tmp/keysharp-desktop-capture-XXXXXX";
    int descriptor = mkstemp(path);
    if (descriptor >= 0)
        (void)unlink(path);
    return descriptor;
}

static bool wait_for_bytes(int descriptor, uint64_t expected)
{
    for (int attempt = 0; attempt < 100; attempt++) {
        struct stat info;
        if (fstat(descriptor, &info) != 0)
            return false;
        if ((uint64_t)info.st_size >= expected)
            return true;
        struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000L };
        (void)nanosleep(&delay, NULL);
    }
    return false;
}

static bool copy_bytes(int input, int output, uint64_t length)
{
    uint8_t buffer[65536];

    while (length != 0u) {
        size_t requested = length < sizeof(buffer) ? (size_t)length : sizeof(buffer);
        ssize_t count = read(input, buffer, requested);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0 || !ksd_write_all(output, buffer, (size_t)count))
            return false;
        length -= (uint64_t)count;
    }
    return true;
}

static bool write_raw_response(int output_fd, int image_fd, guint32 width, guint32 height,
                               guint32 stride, guint32 format)
{
    static const char magic[8] = { 'K', 'S', 'S', 'C', '1', 0, 0, 0 };
    uint8_t status = KSD_RESPONSE_OK;
    uint64_t length = (uint64_t)stride * height;

    if (width == 0u || height == 0u || stride == 0u || length == 0u
        || !wait_for_bytes(image_fd, length) || lseek(image_fd, 0, SEEK_SET) < 0)
        return ksd_write_error_response(output_fd, "invalid KWin capture payload");
    return ksd_write_all(output_fd, &status, sizeof(status))
        && ksd_write_all(output_fd, magic, sizeof(magic))
        && ksd_write_all(output_fd, &width, sizeof(width))
        && ksd_write_all(output_fd, &height, sizeof(height))
        && ksd_write_all(output_fd, &stride, sizeof(stride))
        && ksd_write_all(output_fd, &format, sizeof(format))
        && ksd_write_all(output_fd, &length, sizeof(length))
        && copy_bytes(image_fd, output_fd, length);
}

static bool kwin_capture(int output_fd, bool window, const char *handle,
                         bool include_decoration, int x, int y, int width, int height)
{
    GDBusConnection *connection = get_session_bus();
    GUnixFDList *fd_list = NULL;
    GVariantBuilder options;
    GVariant *reply = NULL;
    GVariant *results = NULL;
    GError *error = NULL;
    gchar *type = NULL;
    guint32 result_width = 0u, result_height = 0u, stride = 0u, format = 0u;
    int image_fd = -1;
    int fd_handle;
    bool result = false;

    if (connection == NULL)
        return ksd_write_error_response(output_fd, "session bus unavailable");
    image_fd = capture_file();
    if (image_fd < 0)
        return ksd_write_error_response(output_fd, "capture file creation failed");
    fd_list = g_unix_fd_list_new();
    fd_handle = g_unix_fd_list_append(fd_list, image_fd, &error);
    if (fd_handle < 0)
        goto failed;

    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&options, "{sv}", "include-cursor", g_variant_new_boolean(FALSE));
    g_variant_builder_add(&options, "{sv}", "native-resolution", g_variant_new_boolean(FALSE));
    if (window)
        g_variant_builder_add(&options, "{sv}", "include-decoration",
                              g_variant_new_boolean(include_decoration));

    reply = g_dbus_connection_call_with_unix_fd_list_sync(connection,
        "org.kde.KWin", "/org/kde/KWin/ScreenShot2", "org.kde.KWin.ScreenShot2",
        window ? "CaptureWindow" : "CaptureArea",
        window ? g_variant_new("(sa{sv}h)", handle, &options, fd_handle)
               : g_variant_new("(iiuua{sv}h)", x, y, (guint32)width, (guint32)height, &options, fd_handle),
        G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, KSD_CAPTURE_TIMEOUT_MS,
        fd_list, NULL, NULL, &error);
    if (reply == NULL)
        goto failed;
    g_variant_get(reply, "(@a{sv})", &results);
    if (!g_variant_lookup(results, "type", "s", &type) || strcmp(type, "raw") != 0
        || !g_variant_lookup(results, "width", "u", &result_width)
        || !g_variant_lookup(results, "height", "u", &result_height)
        || !g_variant_lookup(results, "stride", "u", &stride)
        || !g_variant_lookup(results, "format", "u", &format))
        goto failed;
    result = write_raw_response(output_fd, image_fd, result_width, result_height, stride, format);
    goto cleanup;

failed:
    result = ksd_write_error_response(output_fd,
        error != NULL ? error->message : "KWin capture failed");
cleanup:
    g_free(type);
    if (results != NULL) g_variant_unref(results);
    if (reply != NULL) g_variant_unref(reply);
    if (fd_list != NULL) g_object_unref(fd_list);
    if (error != NULL) g_error_free(error);
    if (image_fd >= 0) close(image_fd);
    return result;
}

bool ksd_capture_area(ksd_backend backend, int output_fd,
                      int x, int y, int width, int height)
{
    if (width <= 0 || height <= 0)
        return ksd_write_error_response(output_fd, "invalid capture dimensions");
    if (backend == KSD_BACKEND_KWIN)
        return kwin_capture(output_fd, false, NULL, false, x, y, width, height);
    if (backend == KSD_BACKEND_GNOME || backend == KSD_BACKEND_CINNAMON)
        return extension_capture(backend, output_fd, false, x, y, width, height, 0u);
    return ksd_write_error_response(output_fd, "capture backend unavailable");
}

bool ksd_capture_window(ksd_backend backend, int output_fd,
                        const char *handle, bool include_decoration)
{
    if (handle == NULL || handle[0] == '\0')
        return ksd_write_error_response(output_fd, "invalid window handle");
    if (backend == KSD_BACKEND_KWIN)
        return kwin_capture(output_fd, true, handle, include_decoration, 0, 0, 0, 0);
    if (backend == KSD_BACKEND_GNOME || backend == KSD_BACKEND_CINNAMON) {
        char *end = NULL;
        errno = 0;
        uint64_t numeric = strtoull(handle, &end, 10);
        if (errno != 0 || end == handle || *end != '\0')
            return ksd_write_error_response(output_fd, "invalid provider window handle");
        return extension_capture(backend, output_fd, true, 0, 0, 0, 0, numeric);
    }
    return ksd_write_error_response(output_fd, "capture backend unavailable");
}
