#include "enable_extension.h"

#include "backend.h"
#include "session_environ.h"

#include <gio/gio.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Both providers ship under one uuid, so there is nothing to parse out of a
 * metadata.json to learn it. */
#define KSD_EXTENSION_UUID "keysharp@keysharp.io"

#define KSD_GNOME_PROVIDER_NAME "io.github.keysharp.GnomeShell"
#define KSD_CINNAMON_PROVIDER_NAME "io.github.keysharp.CinnamonShell"

/* Long enough for XDG_DATA_DIRS, which is a path list and can be long. A value
 * that does not fit reads as unknown rather than as absent. */
#define KSD_DATA_DIRS_CAPACITY 4096u

/* How long to wait for the shell to notice the key change before concluding it
 * needs a re-login. Bounded as elapsed time rather than as a count of sleeps,
 * because each poll makes a synchronous bus call that can itself take a
 * second, and two root installers wait on this. */
#define KSD_ENABLE_VERIFY_MS 3000u
#define KSD_ENABLE_POLL_MS 250u

const char *ksd_enable_status_name(ksd_enable_status status)
{
    switch (status) {
        case KSD_ENABLE_ALREADY_LIVE: return "already_live";
        case KSD_ENABLE_ENABLED: return "enabled";
        case KSD_ENABLE_NEEDS_RELOGIN: return "needs_relogin";
        case KSD_ENABLE_ALREADY_LISTED: return "already_listed";
        case KSD_ENABLE_NOT_APPLICABLE: return "not_applicable";
        case KSD_ENABLE_NO_SHELL: return "no_shell";
        case KSD_ENABLE_NO_BUS: return "no_bus";
        case KSD_ENABLE_KILL_SWITCH: return "kill_switch";
        case KSD_ENABLE_LOCKED: return "locked";
        case KSD_ENABLE_NO_SCHEMA: return "no_schema";
        case KSD_ENABLE_SHELL_REJECTED: return "shell_rejected";
        case KSD_ENABLE_REFUSED: return "refused";
    }
    return "unknown";
}

int ksd_enable_exit_code(ksd_enable_status status)
{
    switch (status) {
        /* Done, or there was never anything to do. An installer must not treat
         * a KWin or X11 machine as a failure. */
        case KSD_ENABLE_ALREADY_LIVE:
        case KSD_ENABLE_ENABLED:
        case KSD_ENABLE_NOT_APPLICABLE:
            return 0;
        /* Written, but the user has to act: log out, or clear the thing that
         * is holding it back. */
        case KSD_ENABLE_NEEDS_RELOGIN:
        case KSD_ENABLE_ALREADY_LISTED:
        case KSD_ENABLE_KILL_SWITCH:
        case KSD_ENABLE_SHELL_REJECTED:
            return 3;
        /* Nothing was written and something is wrong. */
        default:
            return 1;
    }
}

bool ksd_enable_credentials_refused(uid_t uid, uid_t euid, gid_t gid,
                                    gid_t egid)
{
    return uid == 0u || euid == 0u || uid != euid || gid != egid;
}

bool ksd_strv_contains(const char *const *values, const char *needle)
{
    if (values == NULL || needle == NULL)
        return false;
    for (size_t index = 0u; values[index] != NULL; index++)
        if (strcmp(values[index], needle) == 0)
            return true;
    return false;
}

static size_t strv_length(const char *const *values)
{
    size_t length = 0u;
    if (values != NULL)
        while (values[length] != NULL)
            length++;
    return length;
}

char **ksd_strv_with(const char *const *values, const char *addition)
{
    size_t length;
    char **result;
    size_t index;

    if (addition == NULL)
        return NULL;
    if (ksd_strv_contains(values, addition))
        return g_strdupv((char **)(void *)(const char **)values);
    length = strv_length(values);
    result = g_new0(char *, length + 2u);
    for (index = 0u; index < length; index++)
        result[index] = g_strdup(values[index]);
    result[length] = g_strdup(addition);
    return result;
}

char **ksd_strv_without(const char *const *values, const char *removal)
{
    size_t length = strv_length(values);
    char **result = g_new0(char *, length + 1u);
    size_t kept = 0u;

    for (size_t index = 0u; index < length; index++) {
        if (removal != NULL && strcmp(values[index], removal) == 0)
            continue;
        result[kept++] = g_strdup(values[index]);
    }
    return result;
}

bool ksd_data_dir_listed(const char *search_path, const char *directory)
{
    const char *cursor = search_path;
    size_t wanted;

    if (search_path == NULL || directory == NULL)
        return false;
    wanted = strlen(directory);
    /* Compared with trailing slashes ignored on both sides, so that
     * /usr/local/share and /usr/local/share/ are one answer. */
    while (wanted > 1u && directory[wanted - 1u] == '/')
        wanted--;
    if (wanted == 0u)
        return false;
    while (*cursor != '\0') {
        const char *end = strchr(cursor, ':');
        size_t length = end == NULL ? strlen(cursor) : (size_t)(end - cursor);

        while (length > 1u && cursor[length - 1u] == '/')
            length--;
        if (length == wanted && memcmp(cursor, directory, wanted) == 0)
            return true;
        if (end == NULL)
            break;
        cursor = end + 1u;
    }
    return false;
}

/* A schema looked up rather than constructed.
 *
 * g_settings_new aborts the process when the schema is not installed -- it
 * calls g_error, so there is no return value to test and no GError to read. On
 * a Cinnamon machine org.gnome.shell is usually absent, and the other way
 * round, so the obvious spelling would turn "this shell is not installed" into
 * a crash. */
static GSettings *open_schema(const char *schema_id)
{
    GSettingsSchemaSource *source = g_settings_schema_source_get_default();
    GSettingsSchema *schema;
    GSettings *settings;

    if (source == NULL)
        return NULL;
    schema = g_settings_schema_source_lookup(source, schema_id, TRUE);
    if (schema == NULL)
        return NULL;
    settings = g_settings_new_full(schema, NULL, NULL);
    g_settings_schema_unref(schema);
    return settings;
}

/* g_settings_get_strv and friends abort on a key the schema does not have, so
 * every access is gated on this. A renamed key becomes a diagnostic instead of
 * a crash. */
static bool schema_has_key(GSettings *settings, const char *key)
{
    GSettingsSchema *schema = NULL;
    bool present;

    if (settings == NULL)
        return false;
    g_object_get(settings, "settings-schema", &schema, NULL);
    if (schema == NULL)
        return false;
    present = g_settings_schema_has_key(schema, key) == TRUE;
    g_settings_schema_unref(schema);
    return present;
}

static bool session_bus_reachable(void)
{
    GError *error = NULL;
    GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL,
                                                 &error);
    if (connection == NULL) {
        if (error != NULL)
            g_error_free(error);
        return false;
    }
    g_object_unref(connection);
    return true;
}

static bool provider_is_live(const char *name)
{
    GError *error = NULL;
    GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL,
                                                 &error);
    GVariant *reply;
    gboolean owned = FALSE;

    if (connection == NULL) {
        if (error != NULL)
            g_error_free(error);
        return false;
    }
    reply = g_dbus_connection_call_sync(connection, "org.freedesktop.DBus",
        "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
        g_variant_new("(s)", name), G_VARIANT_TYPE("(b)"),
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &error);
    if (reply != NULL) {
        g_variant_get(reply, "(b)", &owned);
        g_variant_unref(reply);
    }
    if (error != NULL)
        g_error_free(error);
    g_object_unref(connection);
    return owned == TRUE;
}

/* Whether the directory this build installs its extension into is one the
 * running shell will actually search.
 *
 * Read from the SHELL's environment, not ours: the shell resolved its search
 * path when it started, and the installers run this under env -i, whose
 * XDG_DATA_DIRS is not the session's.
 *
 * A false return from the environ read means UNKNOWN, not absent. It is false
 * for an empty value, for one too large for the buffer, and for an unreadable
 * /proc entry alike -- and an absent XDG_DATA_DIRS means GLib applies its
 * /usr/local/share:/usr/share default, under which /usr/local IS searched. So
 * this only ever downgrades to a warning; it never refuses to write. */
static bool shell_searches(pid_t shell_pid, const char *directory,
                           bool *known)
{
    char dirs[KSD_DATA_DIRS_CAPACITY];

    *known = false;
    if (shell_pid <= 0 || directory == NULL)
        return false;
    if (!ksd_session_environ_value(shell_pid, "XDG_DATA_DIRS", dirs,
                                   sizeof(dirs)))
        return false;
    *known = true;
    return ksd_data_dir_listed(dirs, directory);
}

typedef struct {
    const char *schema_id;
    const char *provider_name;
    const char *datadir_leaf;
    const char *shell_label;
} shell_profile;

static const shell_profile gnome_profile = {
    "org.gnome.shell", KSD_GNOME_PROVIDER_NAME, "gnome-shell", "GNOME Shell",
};

static const shell_profile cinnamon_profile = {
    "org.cinnamon", KSD_CINNAMON_PROVIDER_NAME, "cinnamon", "Cinnamon",
};

/* Waits for the shell to pick the change up, bounded by elapsed time. */
static bool wait_for_provider(const char *name)
{
    gint64 deadline = g_get_monotonic_time()
        + (gint64)KSD_ENABLE_VERIFY_MS * 1000;

    for (;;) {
        if (provider_is_live(name))
            return true;
        if (g_get_monotonic_time() >= deadline)
            return false;
        g_usleep(KSD_ENABLE_POLL_MS * 1000);
    }
}

/* Adds the uuid to enabled-extensions, and on GNOME takes it back out of
 * disabled-extensions, which otherwise wins. Reports whether anything changed
 * so the caller can tell "already listed" from "we just added it". */
static ksd_enable_status apply_lists(GSettings *settings,
                                     const shell_profile *profile,
                                     bool *changed)
{
    char **enabled;
    char **updated;

    *changed = false;
    if (!schema_has_key(settings, "enabled-extensions"))
        return KSD_ENABLE_NO_SCHEMA;
    if (!g_settings_is_writable(settings, "enabled-extensions"))
        return KSD_ENABLE_LOCKED;

    /* The kill switch makes every extension inert whatever the list says, so
     * it is read before the write and reported rather than overridden -- the
     * user may have turned it on deliberately. */
    if (schema_has_key(settings, "disable-user-extensions")
        && g_settings_get_boolean(settings, "disable-user-extensions"))
        return KSD_ENABLE_KILL_SWITCH;

    enabled = g_settings_get_strv(settings, "enabled-extensions");
    if (!ksd_strv_contains((const char *const *)enabled,
                           KSD_EXTENSION_UUID)) {
        updated = ksd_strv_with((const char *const *)enabled,
                                KSD_EXTENSION_UUID);
        g_settings_set_strv(settings, "enabled-extensions",
                            (const gchar *const *)updated);
        g_strfreev(updated);
        *changed = true;
    }
    g_strfreev(enabled);

    /* GNOME only. An entry here overrides the enabled list entirely, so an
     * enable that ignored it would report success and do nothing. */
    if (schema_has_key(settings, "disabled-extensions")
        && g_settings_is_writable(settings, "disabled-extensions")) {
        char **disabled = g_settings_get_strv(settings,
                                              "disabled-extensions");
        if (ksd_strv_contains((const char *const *)disabled,
                              KSD_EXTENSION_UUID)) {
            updated = ksd_strv_without((const char *const *)disabled,
                                       KSD_EXTENSION_UUID);
            g_settings_set_strv(settings, "disabled-extensions",
                                (const gchar *const *)updated);
            g_strfreev(updated);
            *changed = true;
        }
        g_strfreev(disabled);
    }

    /* dconf batches writes and flushes on idle, and this process is about to
     * exit. Without this the caller sees success with nothing written. */
    g_settings_sync();
    (void)profile;
    return KSD_ENABLE_ENABLED;
}

/* Did the shell take our entry back out? Cinnamon drops an extension that
 * throws while loading, and re-running the command then re-adds it -- an
 * enable/still-None loop that reads as a broken command rather than a broken
 * extension. */
static bool shell_removed_entry(GSettings *settings)
{
    char **enabled;
    bool present;

    if (!schema_has_key(settings, "enabled-extensions"))
        return false;
    enabled = g_settings_get_strv(settings, "enabled-extensions");
    present = ksd_strv_contains((const char *const *)enabled,
                                KSD_EXTENSION_UUID);
    g_strfreev(enabled);
    return !present;
}

static void report_search_path(const shell_profile *profile)
{
    char installed[1024];
    pid_t shell_pid = ksd_backend_provider_pid(
        profile == &gnome_profile ? KSD_BACKEND_GNOME : KSD_BACKEND_CINNAMON);
    bool known = false;
    int written = snprintf(installed, sizeof(installed), "%s",
                           KSD_EXTENSION_DATADIR);

    if (written <= 0 || (size_t)written >= sizeof(installed))
        return;
    printf("installed_path=%s/%s/extensions/%s\n", installed,
           profile->datadir_leaf, KSD_EXTENSION_UUID);
    if (shell_searches(shell_pid, installed, &known) || !known)
        return;
    fprintf(stderr,
        "keysharp-desktop: %s does not search %s, so it will not find the"
        " extension there. Link it into your own data directory:\n"
        "  mkdir -p ~/.local/share/%s/extensions\n"
        "  ln -sfn %s/%s/extensions/%s"
        " ~/.local/share/%s/extensions/%s\n",
        profile->shell_label, installed, profile->datadir_leaf,
        installed, profile->datadir_leaf, KSD_EXTENSION_UUID,
        profile->datadir_leaf, KSD_EXTENSION_UUID);
}

static ksd_enable_status enable_for(const shell_profile *profile)
{
    GSettings *settings;
    ksd_enable_status status;
    bool changed = false;

    if (provider_is_live(profile->provider_name))
        return KSD_ENABLE_ALREADY_LIVE;
    settings = open_schema(profile->schema_id);
    if (settings == NULL)
        return KSD_ENABLE_NO_SCHEMA;
    status = apply_lists(settings, profile, &changed);
    if (status != KSD_ENABLE_ENABLED) {
        g_object_unref(settings);
        return status;
    }
    report_search_path(profile);
    if (wait_for_provider(profile->provider_name)) {
        g_object_unref(settings);
        return KSD_ENABLE_ENABLED;
    }
    if (shell_removed_entry(settings)) {
        g_object_unref(settings);
        return KSD_ENABLE_SHELL_REJECTED;
    }
    g_object_unref(settings);
    /* The list is right and the provider is not up. Either the shell has not
     * scanned this uuid yet, or it parked it as out of date. Both need the
     * user, and both are fixed by logging back in after checking the version
     * the extension declares. */
    return changed ? KSD_ENABLE_NEEDS_RELOGIN : KSD_ENABLE_ALREADY_LISTED;
}

static void explain(ksd_enable_status status)
{
    switch (status) {
        case KSD_ENABLE_ALREADY_LIVE:
            puts("The extension is already enabled and answering.");
            break;
        case KSD_ENABLE_ENABLED:
            puts("Enabled. The provider is up; no further action needed.");
            break;
        case KSD_ENABLE_NEEDS_RELOGIN:
            fputs("Enabled, but the shell has not loaded it yet. Log out and"
                  " back in, then check with: keysharp-desktop probe\n",
                  stderr);
            break;
        case KSD_ENABLE_ALREADY_LISTED:
            fputs("The extension was already listed but is not running. Log"
                  " out and back in; if it still does not appear, the shell"
                  " may consider it out of date for this version.\n", stderr);
            break;
        case KSD_ENABLE_NOT_APPLICABLE:
            puts("This session needs no extension.");
            break;
        case KSD_ENABLE_NO_SHELL:
            fputs("No GNOME or Cinnamon shell is running on this session"
                  " bus, so there is no extension to enable.\n", stderr);
            break;
        case KSD_ENABLE_NO_BUS:
            fputs("No session bus. Run this inside your desktop session.\n",
                  stderr);
            break;
        case KSD_ENABLE_KILL_SWITCH:
            fputs("GNOME has all user extensions turned off"
                  " (org.gnome.shell disable-user-extensions). Nothing was"
                  " changed. Turn it back on with:\n"
                  "  gsettings set org.gnome.shell disable-user-extensions"
                  " false\n", stderr);
            break;
        case KSD_ENABLE_LOCKED:
            fputs("The extension list is locked by a system dconf profile, so"
                  " it cannot be changed here.\n", stderr);
            break;
        case KSD_ENABLE_NO_SCHEMA:
            fputs("The shell's settings schema is not installed, so there is"
                  " nothing to write.\n", stderr);
            break;
        case KSD_ENABLE_SHELL_REJECTED:
            fputs("The shell removed the extension again, which means it"
                  " failed to load. Check: journalctl --user -b\n", stderr);
            break;
        case KSD_ENABLE_REFUSED:
            fputs("keysharp-desktop enable-extension: refusing elevated"
                  " credentials. Extensions are enabled per user; run this as"
                  " yourself, without sudo.\n", stderr);
            break;
    }
}

int ksd_enable_extension_main(int argc, char **argv)
{
    ksd_enable_status status;

    (void)argv;
    /* Before anything else, so the command is usable in a container without a
     * bus or a shell to answer the arity question. */
    if (argc != 1)
        return 2;
    if (ksd_enable_credentials_refused(getuid(), geteuid(), getgid(),
                                       getegid())) {
        status = KSD_ENABLE_REFUSED;
    } else if (!session_bus_reachable()) {
        status = KSD_ENABLE_NO_BUS;
    } else if (ksd_backend_provider_pid(KSD_BACKEND_CINNAMON) > 0) {
        /* Cinnamon first: org.Cinnamon is unambiguous, where a session can
         * carry GNOME names for other reasons. */
        status = enable_for(&cinnamon_profile);
    } else if (ksd_backend_provider_pid(KSD_BACKEND_GNOME) > 0) {
        status = enable_for(&gnome_profile);
    } else if (ksd_backend_resolve() != KSD_BACKEND_NONE) {
        /* KWin, X11 or generic Wayland: served without an extension. */
        status = KSD_ENABLE_NOT_APPLICABLE;
    } else {
        status = KSD_ENABLE_NO_SHELL;
    }
    printf("status=%s\n", ksd_enable_status_name(status));
    explain(status);
    return ksd_enable_exit_code(status);
}
