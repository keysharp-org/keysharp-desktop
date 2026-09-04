#include "install_mode.h"

#include "backend_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Long enough for the runtime directory plus the fixed tail below. A path that
 * does not fit is refused rather than truncated: a truncated socket path is a
 * different socket, and one nobody is listening on is the good case. */
#define KSD_INSTALL_PATH_CAPACITY 4096u

#define KSD_USER_SOCKET_TAIL "/keysharp-desktop/keysharp-desktop.sock"

uid_t ksd_install_owner(void)
{
    /* The effective uid, not a stored decision and not anything the caller can
     * pass in. This is the whole safety argument for the user installation:
     * while this process is root the answer is root, so every ownership test
     * built on it stays exactly as strict as it was. */
    return geteuid();
}

gid_t ksd_install_group(void)
{
    return getegid();
}

bool ksd_install_is_system(void)
{
    return ksd_install_owner() == 0u;
}

bool ksd_install_owner_trusted(uid_t owner)
{
    return owner == ksd_install_owner();
}

/* The per-user directory the socket and the store live under. Refused when it
 * is missing, not a directory, owned by anyone else, or reachable by anyone
 * else -- the same properties /run/keysharp-desktop gets from root. */
static bool runtime_directory(char *buffer, size_t capacity)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    struct stat status;
    size_t length;

    if (runtime == NULL || runtime[0] != '/')
        return false;
    length = strlen(runtime);
    /* A trailing slash would produce a doubled separator, which names the same
     * directory but not the same string, and the string is what the ownership
     * check and the client comparison are made of. */
    while (length > 1u && runtime[length - 1u] == '/')
        length--;
    if (length == 0u || length >= capacity)
        return false;
    memcpy(buffer, runtime, length);
    buffer[length] = 0;
    if (lstat(buffer, &status) != 0 || !S_ISDIR(status.st_mode)
        || status.st_uid != ksd_install_owner()
        || (status.st_mode & (S_IRWXG | S_IRWXO)) != 0u)
        return false;
    return true;
}

bool ksd_install_socket_path(char *buffer, size_t capacity)
{
    char runtime[KSD_INSTALL_PATH_CAPACITY];
    int written;

    if (buffer == NULL || capacity == 0u)
        return false;
    if (ksd_install_is_system()) {
        if (sizeof(KSD_SYSTEM_SOCKET) > capacity)
            return false;
        memcpy(buffer, KSD_SYSTEM_SOCKET, sizeof(KSD_SYSTEM_SOCKET));
        return true;
    }
    if (!runtime_directory(runtime, sizeof(runtime)))
        return false;
    written = snprintf(buffer, capacity, "%s%s", runtime,
                       KSD_USER_SOCKET_TAIL);
    return written > 0 && (size_t)written < capacity;
}

unsigned ksd_install_socket_mode(void)
{
    /* The system socket is the machine's front door and is filtered by peer
     * credentials once a connection arrives. The user socket sits inside a
     * directory only its owner can enter, and widening it past its owner would
     * claim a reach the directory does not grant. */
    return ksd_install_is_system() ? 0666u : 0600u;
}

/* A directory that already exists must be this user's and private. One that
 * does not is left to the store to create, so only its parent can be judged
 * here -- and a parent someone else owns is enough to reject, because whoever
 * owns it decides what appears inside. */
static bool acceptable_store_parent(const char *path)
{
    char parent[KSD_INSTALL_PATH_CAPACITY];
    struct stat status;
    const char *slash;
    size_t length;

    if (lstat(path, &status) == 0)
        return S_ISDIR(status.st_mode)
            && status.st_uid == ksd_install_owner()
            && (status.st_mode & (S_IWGRP | S_IWOTH)) == 0u;
    slash = strrchr(path, '/');
    if (slash == NULL || slash == path)
        return false;
    length = (size_t)(slash - path);
    if (length >= sizeof(parent))
        return false;
    memcpy(parent, path, length);
    parent[length] = 0;
    return acceptable_store_parent(parent);
}

/* The directories are static because the store keeps the pointers it is given
 * rather than copying them, and they are resolved once for the life of the
 * process -- the credentials they are derived from cannot change under a
 * daemon that never calls setuid. */
static const char *cached_directory(char *slot, const char *base,
                                    const char *tail)
{
    int written;

    if (slot[0] != 0)
        return slot;
    written = snprintf(slot, KSD_INSTALL_PATH_CAPACITY, "%s%s", base, tail);
    if (written <= 0 || (size_t)written >= KSD_INSTALL_PATH_CAPACITY
        || !acceptable_store_parent(slot)) {
        slot[0] = 0;
        return NULL;
    }
    return slot;
}

const char *ksd_install_persistent_directory(void)
{
    static char persistent[KSD_INSTALL_PATH_CAPACITY];
    const char *state = getenv("XDG_STATE_HOME");
    char fallback[KSD_INSTALL_PATH_CAPACITY];

    if (ksd_install_is_system())
        return NULL;
    /* The state directory, not the runtime one: a grant the user chose to keep
     * has to survive a logout, and XDG_RUNTIME_DIR is emptied at one. Not the
     * config directory either, which dotfile tooling copies between machines,
     * carrying a grant made on one machine onto another.
     *
     * Falling back the way the specification does when XDG_STATE_HOME is
     * unset, which is the usual case: most sessions set XDG_RUNTIME_DIR and
     * leave the rest to defaults. */
    if (state == NULL || state[0] != '/') {
        const char *home = getenv("HOME");
        int written;

        if (home == NULL || home[0] != '/')
            return NULL;
        written = snprintf(fallback, sizeof(fallback), "%s/.local/state", home);
        if (written <= 0 || (size_t)written >= sizeof(fallback))
            return NULL;
        state = fallback;
    }
    return cached_directory(persistent, state, "/keysharp-desktop");
}

const char *ksd_install_runtime_directory(void)
{
    static char runtime_store[KSD_INSTALL_PATH_CAPACITY];
    char runtime[KSD_INSTALL_PATH_CAPACITY];

    if (ksd_install_is_system())
        return NULL;
    /* Session-scoped state belongs beside the socket, and is meant to go when
     * the session does. */
    if (!runtime_directory(runtime, sizeof(runtime)))
        return NULL;
    return cached_directory(runtime_store, runtime, "/keysharp-desktop/run");
}
