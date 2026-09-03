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
    /* Last, for now. A provider that is present still answers, so an existing
     * GNOME-on-X11 or Cinnamon-on-X11 user keeps exactly what they had; this
     * only replaces the generic backend on an X11 session with no provider. */
    if (ksd_session_is_x11_process(getpid()))
        return KSD_BACKEND_X11;
    return KSD_BACKEND_NONE;
}

typedef struct session_facts {
    ksd_backend compositor;
    bool session_type_x11;
    bool wayland_display;
} session_facts;

static bool entry_value(const char *entry, size_t entry_length,
                        const char *prefix, size_t prefix_length,
                        const char **value)
{
    if (entry_length < prefix_length
        || memcmp(entry, prefix, prefix_length) != 0)
        return false;
    *value = entry + prefix_length;
    return true;
}

/* One walk of the environment, two answers. The compositor comes from
 * XDG_CURRENT_DESKTOP as before; the session type is new and is read in the
 * same pass rather than by opening /proc twice. */
static bool read_session_facts(pid_t pid, session_facts *facts)
{
    char path[64];
    char environment[64u * 1024u + 1u];
    int length = snprintf(path, sizeof(path), "/proc/%ld/environ", (long)pid);

    facts->compositor = KSD_BACKEND_NONE;
    facts->session_type_x11 = false;
    facts->wayland_display = false;
    if (pid <= 0 || length <= 0 || (size_t)length >= sizeof(path))
        return false;
    int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return false;
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
        return false;
    environment[offset] = '\0';
    for (size_t index = 0u; index < offset;) {
        const char *entry = environment + index;
        size_t remaining = offset - index;
        size_t entry_length = strnlen(entry, remaining);
        const char *value;

        if (entry_length == remaining)
            return false;
        if (entry_value(entry, entry_length, "XDG_CURRENT_DESKTOP=", 20u,
                        &value)
            && facts->compositor == KSD_BACKEND_NONE) {
            if (strcasestr(value, "KDE") != NULL)
                facts->compositor = KSD_BACKEND_KWIN;
            else if (strcasestr(value, "Cinnamon") != NULL)
                facts->compositor = KSD_BACKEND_CINNAMON;
            else if (strcasestr(value, "GNOME") != NULL)
                facts->compositor = KSD_BACKEND_GNOME;
        }
        /* A whole-value match. XDG_SESSION_TYPE=x11-fallback is not an X11
         * session, and a substring test would call it one. */
        if (entry_value(entry, entry_length, "XDG_SESSION_TYPE=", 17u, &value))
            facts->session_type_x11 = strcasecmp(value, "x11") == 0;
        if (entry_value(entry, entry_length, "WAYLAND_DISPLAY=", 16u, &value))
            facts->wayland_display = value[0] != '\0';
        index += entry_length + 1u;
    }
    return true;
}

ksd_backend ksd_backend_resolve_process(pid_t pid)
{
    session_facts facts;
    if (!read_session_facts(pid, &facts))
        return KSD_BACKEND_NONE;
    return facts.compositor;
}

bool ksd_session_is_x11_process(pid_t pid)
{
    session_facts facts;
    /* DISPLAY is not consulted. XWayland sets it on nearly every Wayland
     * session, so consulting it would call those sessions X11. */
    return read_session_facts(pid, &facts)
        && facts.session_type_x11 && !facts.wayland_display;
}

bool ksd_backend_session_unsupported(void)
{
    return ksd_backend_resolve_process(getpid()) == KSD_BACKEND_NONE;
}

/* The coordinate group. These four move together or not at all: a caller that
 * can enumerate windows but cannot ask where the pointer is, or can ask for
 * the work area but not which window is active, falls back for the rest
 * anyway, so shipping a subset buys nothing. */
#define KSD_X11_COORDINATE_OPERATIONS \
    (KSD_OPERATION_WINDOW_LIST | KSD_OPERATION_WINDOW_ACTIVE \
     | KSD_OPERATION_CURSOR_POSITION | KSD_OPERATION_WORK_AREA)

uint64_t ksd_backend_x11_route(ksd_backend backend, bool x11_session)
{
    /* Off an X11 session there is no X server this worker may talk to, so no
     * operation routes there whatever the compositor is. */
    if (!x11_session)
        return 0u;
    if (backend != KSD_BACKEND_X11)
        return 0u;
    /* Never more than the backend advertises. Routing a bit outside the mask
     * would serve an operation the client was told does not exist. */
    return KSD_X11_COORDINATE_OPERATIONS & ksd_backend_operations(backend);
}

uint64_t ksd_backend_operations(ksd_backend backend)
{
    if (backend == KSD_BACKEND_GENERIC)
        return 0u;
    /* Zero until the authority dispatches to the X11 worker. Advertising the
     * coordinate group before anything serves it is the same defect this file
     * carried for Cinnamon window capture: announced, and refused every time.
     * The mask and the dispatch land in one commit. */
    if (backend == KSD_BACKEND_X11)
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
        | KSD_OPERATION_CLIPBOARD_SET_CONTENT
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
