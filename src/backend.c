#include "backend.h"

#include "protocol.h"

#include <gio/gio.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static GDBusConnection *get_session_bus(void)
{
    GError *error = NULL;
    GDBusConnection *session_bus =
        g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (error != NULL)
        g_error_free(error);
    return session_bus;
}

static bool name_has_owner(const char *name)
{
    GDBusConnection *connection = get_session_bus();
    GError *error = NULL;
    GVariant *reply;
    gboolean owned = FALSE;

    if (connection == NULL)
        return false;
    reply = g_dbus_connection_call_sync(connection,
        "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "NameHasOwner", g_variant_new("(s)", name),
        G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &error);
    if (reply != NULL) {
        g_variant_get(reply, "(b)", &owned);
        g_variant_unref(reply);
    }
    if (error != NULL)
        g_error_free(error);
    g_object_unref(connection);
    return owned == TRUE;
}

static bool kwin_wayland_owner(void)
{
    GDBusConnection *connection = get_session_bus();
    GError *error = NULL;
    GVariant *owner_reply = NULL;
    GVariant *pid_reply = NULL;
    const char *owner = NULL;
    guint32 pid = 0u;
    char proc_path[64];
    char executable[PATH_MAX + 1u];
    struct stat status;
    bool wayland = false;

    if (connection == NULL)
        return false;
    owner_reply = g_dbus_connection_call_sync(connection,
        "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "GetNameOwner",
        g_variant_new("(s)", "org.kde.KWin"), G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &error);
    if (owner_reply == NULL)
        goto done;
    g_variant_get(owner_reply, "(&s)", &owner);
    pid_reply = g_dbus_connection_call_sync(connection,
        "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "GetConnectionUnixProcessID",
        g_variant_new("(s)", owner), G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &error);
    if (pid_reply == NULL)
        goto done;
    g_variant_get(pid_reply, "(u)", &pid);
    int length = snprintf(proc_path, sizeof(proc_path), "/proc/%u/exe", pid);
    ssize_t executable_length = pid != 0u && length > 0
        && (size_t)length < sizeof(proc_path)
        ? readlink(proc_path, executable, sizeof(executable) - 1u) : -1;
    if (executable_length <= 0
        || (size_t)executable_length >= sizeof(executable)
        || stat(proc_path, &status) != 0 || !S_ISREG(status.st_mode)
        || status.st_uid != 0u
        || (status.st_mode & (S_IWGRP | S_IWOTH)) != 0)
        goto done;
    executable[executable_length] = '\0';
    const char *basename = strrchr(executable, '/');
    basename = basename == NULL ? executable : basename + 1u;
    wayland = strcmp(basename, "kwin_wayland") == 0;

done:
    if (pid_reply != NULL)
        g_variant_unref(pid_reply);
    if (owner_reply != NULL)
        g_variant_unref(owner_reply);
    if (error != NULL)
        g_error_free(error);
    g_object_unref(connection);
    return wayland;
}

ksd_backend ksd_backend_resolve(void)
{
    if (kwin_wayland_owner())
        return KSD_BACKEND_KWIN;
    if (name_has_owner("io.github.keysharp.GnomeShell"))
        return KSD_BACKEND_GNOME;
    if (name_has_owner("io.github.keysharp.CinnamonShell"))
        return KSD_BACKEND_CINNAMON;
    return KSD_BACKEND_NONE;
}

ksd_backend ksd_backend_resolve_process(pid_t pid)
{
    char path[64];
    char environment[64u * 1024u + 1u];
    int length = snprintf(path, sizeof(path), "/proc/%ld/environ", (long)pid);
    if (pid <= 0 || length <= 0 || (size_t)length >= sizeof(path))
        return KSD_BACKEND_NONE;
    int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return KSD_BACKEND_NONE;
    size_t offset = 0u;
    while (offset < sizeof(environment) - 1u) {
        ssize_t count = read(descriptor, environment + offset,
                             sizeof(environment) - 1u - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        offset += (size_t)count;
    }
    char extra;
    ssize_t overflow = read(descriptor, &extra, sizeof(extra));
    close(descriptor);
    if (overflow != 0)
        return KSD_BACKEND_NONE;
    environment[offset] = '\0';
    for (size_t index = 0u; index < offset;) {
        const char *entry = environment + index;
        size_t remaining = offset - index;
        size_t entry_length = strnlen(entry, remaining);
        if (entry_length == remaining)
            return KSD_BACKEND_NONE;
        static const char prefix[] = "XDG_CURRENT_DESKTOP=";
        if (entry_length >= sizeof(prefix) - 1u
            && memcmp(entry, prefix, sizeof(prefix) - 1u) == 0) {
            const char *desktop = entry + sizeof(prefix) - 1u;
            if (strcasestr(desktop, "KDE") != NULL)
                return KSD_BACKEND_KWIN;
            if (strcasestr(desktop, "Cinnamon") != NULL)
                return KSD_BACKEND_CINNAMON;
            if (strcasestr(desktop, "GNOME") != NULL)
                return KSD_BACKEND_GNOME;
        }
        index += entry_length + 1u;
    }
    return KSD_BACKEND_NONE;
}

bool ksd_backend_session_unsupported(void)
{
    return ksd_backend_resolve_process(getpid()) == KSD_BACKEND_NONE;
}

uint64_t ksd_backend_operations(ksd_backend backend)
{
    if (backend == KSD_BACKEND_GENERIC)
        return 0u;
    if (backend == KSD_BACKEND_KWIN)
        return KSD_OPERATION_CAPTURE_AREA | KSD_OPERATION_CAPTURE_WINDOW;
    if (backend != KSD_BACKEND_GNOME && backend != KSD_BACKEND_CINNAMON)
        return 0u;
    uint64_t operations = KSD_OPERATION_WINDOW_LIST
        | KSD_OPERATION_WINDOW_ACTIVE
        | KSD_OPERATION_WINDOW_WATCH | KSD_OPERATION_WINDOW_FOCUS
        | KSD_OPERATION_WINDOW_RAISE | KSD_OPERATION_WINDOW_LOWER
        | KSD_OPERATION_WINDOW_CLOSE | KSD_OPERATION_WINDOW_KILL
        | KSD_OPERATION_WINDOW_MOVE_RESIZE
        | KSD_OPERATION_WINDOW_MOVE_RESIZE_XID
        | KSD_OPERATION_WINDOW_SET_STATE
        | KSD_OPERATION_WINDOW_SET_OPACITY
        | KSD_OPERATION_WINDOW_SET_ABOVE
        | KSD_OPERATION_WINDOW_SET_DECORATED
        | KSD_OPERATION_WINDOW_RESERVE
        | KSD_OPERATION_WINDOW_GET_RESERVED
        | KSD_OPERATION_CLIPBOARD_MIMETYPES
        | KSD_OPERATION_CLIPBOARD_CONTENT
        | KSD_OPERATION_CLIPBOARD_TEXT
        | KSD_OPERATION_CLIPBOARD_WATCH
        | KSD_OPERATION_MOUSE_MOVE_ABSOLUTE
        | KSD_OPERATION_MOUSE_MOVE_RELATIVE
        | KSD_OPERATION_MOUSE_BUTTON
        | KSD_OPERATION_MOUSE_SCROLL
        | KSD_OPERATION_CURSOR_POSITION
        | KSD_OPERATION_WORK_AREA;
    operations |= KSD_OPERATION_CAPTURE_WINDOW;
    if (backend == KSD_BACKEND_GNOME)
        operations |= KSD_OPERATION_CAPTURE_AREA;
    return operations;
}
