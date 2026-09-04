/* The rule that decides who this installation trusts.
 *
 * A user installation exists so that keysharp-desktop can be installed without
 * administrator rights. It is allowed to trust artefacts owned by the user who
 * installed it -- and that is a relaxation, so the property worth pinning is
 * not that the relaxation works but that it cannot reach a root daemon. The
 * answer is derived from the process credentials alone, so there is no
 * argument, variable or file that moves it.
 *
 * Running as root is not available here, so the cases below establish the
 * other half: nothing outside the process's own credentials changes the
 * answer, and the directory a user socket may live in is held to the same
 * standard the system directory gets from being root-owned.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "install_mode.h"

/* Whatever the environment says, the owner is this process. Every ownership
 * test in the service is built on this one call, so if it could be moved from
 * outside, all of them could. */
static void check_owner_follows_credentials_only(void)
{
    static const char *const attempts[] = {
        "0", "", "root", "65534", "-1",
    };

    assert(ksd_install_owner() == geteuid());
    for (size_t index = 0u;
         index < sizeof(attempts) / sizeof(attempts[0]); index++) {
        assert(setenv("KSD_INSTALL_OWNER", attempts[index], 1) == 0);
        assert(setenv("KSD_INSTALL_MODE", attempts[index], 1) == 0);
        assert(setenv("KSD_INSTALL_SYSTEM", attempts[index], 1) == 0);
        assert(ksd_install_owner() == geteuid());
        assert(ksd_install_is_system() == (geteuid() == 0u));
    }
    assert(unsetenv("KSD_INSTALL_OWNER") == 0);
    assert(unsetenv("KSD_INSTALL_MODE") == 0);
    assert(unsetenv("KSD_INSTALL_SYSTEM") == 0);
}

/* Only this uid is trusted, and in particular not root -- an unprivileged
 * daemon has no reason to accept a root-owned artefact it did not create, and
 * treating uid zero as universally acceptable is how a fallback becomes a
 * privilege boundary nobody checked. */
static void check_only_the_owner_is_trusted(void)
{
    uid_t self = geteuid();

    assert(ksd_install_owner_trusted(self));
    assert(!ksd_install_owner_trusted(self + 1u));
    if (self != 0u)
        assert(!ksd_install_owner_trusted(0u));
}

static void set_runtime(const char *path)
{
    if (path == NULL)
        assert(unsetenv("XDG_RUNTIME_DIR") == 0);
    else
        assert(setenv("XDG_RUNTIME_DIR", path, 1) == 0);
}

/* A directory that is private but belongs to another user. It cannot be made
 * here -- creating one requires being that user -- so an existing one is
 * borrowed. Every candidate is checked rather than assumed, so a machine where
 * one of them is world-readable does not turn this into a test of the
 * permission rule wearing the ownership rule's name. */
static bool foreign_private_directory(char *buffer, size_t capacity)
{
    static const char *const candidates[] = {
        "/root", "/etc/ssl/private", "/var/lib/private", "/etc/credstore",
    };
    uid_t self = geteuid();

    for (size_t index = 0u;
         index < sizeof(candidates) / sizeof(candidates[0]); index++) {
        struct stat status;

        if (lstat(candidates[index], &status) != 0
            || !S_ISDIR(status.st_mode) || status.st_uid == self
            || (status.st_mode & (S_IRWXG | S_IRWXO)) != 0u
            || strlen(candidates[index]) >= capacity)
            continue;
        strcpy(buffer, candidates[index]);
        return true;
    }
    return false;
}

/* A user socket may only live in a directory with the properties the system
 * directory gets from root: this user's own, and reachable by nobody else. */
static void check_user_socket_directory_is_held_to_the_system_standard(void)
{
    char path[4096];
    char private_dir[4096];
    char open_dir[4096];
    char foreign[4096];
    const char *base = getenv("TMPDIR");
    int written;

    if (geteuid() == 0u)
        return;
    if (base == NULL)
        base = "/tmp";
    written = snprintf(private_dir, sizeof(private_dir),
                       "%s/ksd-install-mode-%ld-private", base,
                       (long)getpid());
    assert(written > 0 && (size_t)written < sizeof(private_dir));
    written = snprintf(open_dir, sizeof(open_dir), "%s/ksd-install-mode-%ld-open",
                       base, (long)getpid());
    assert(written > 0 && (size_t)written < sizeof(open_dir));
    (void)rmdir(private_dir);
    (void)rmdir(open_dir);
    assert(mkdir(private_dir, 0700) == 0);
    assert(mkdir(open_dir, 0755) == 0);
    /* mkdir is filtered by the umask, so the permissive case has to be set
     * explicitly or the test would silently be running two identical ones. */
    assert(chmod(open_dir, 0755) == 0);

    /* No runtime directory at all: there is nowhere safe to put it, and the
     * answer is no rather than somewhere unsafe. */
    set_runtime(NULL);
    assert(!ksd_install_socket_path(path, sizeof(path)));

    /* A relative path names whatever directory the daemon happens to be in.
     * The one used here resolves, and to a directory that passes every other
     * check: a name that simply did not exist would be turned away by the
     * lookup and the rule under test would never be reached. */
    assert(chdir(base) == 0);
    set_runtime(strrchr(private_dir, '/') + 1);
    assert(!ksd_install_socket_path(path, sizeof(path)));

    /* Reachable by group and other, so anyone could replace the socket in it
     * and be connected to in its place. */
    set_runtime(open_dir);
    assert(!ksd_install_socket_path(path, sizeof(path)));

    /* Private, but somebody else's -- so its owner decides what appears in it,
     * including what the client would find at our socket's name. Refusing this
     * is not the same rule as refusing a readable directory, and a directory
     * that fails both would not tell the two apart. */
    if (foreign_private_directory(foreign, sizeof(foreign))) {
        set_runtime(foreign);
        assert(!ksd_install_socket_path(path, sizeof(path)));
    } else {
        fputs("no private directory owned by another user was found: the "
              "ownership rule went unexercised\n", stderr);
    }

    /* Somewhere that is not a directory at all. */
    set_runtime("/etc/hostname");
    assert(!ksd_install_socket_path(path, sizeof(path)));

    set_runtime(private_dir);
    assert(ksd_install_socket_path(path, sizeof(path)));
    assert(strncmp(path, private_dir, strlen(private_dir)) == 0);
    assert(path[strlen(private_dir)] == '/');
    /* Not the machine-wide path: a user installation that bound that one would
     * be claiming to serve everybody. */
    assert(strcmp(path, "/run/keysharp-desktop/keysharp-desktop.sock") != 0);

    /* A buffer too small is refused rather than filled with a prefix, which
     * would name a different socket -- and one nobody is listening on is the
     * good case. */
    assert(!ksd_install_socket_path(path, strlen(private_dir) + 2u));

    /* Session-scoped store state follows the socket into the same private
     * directory, and goes when the session does. */
    assert(ksd_install_runtime_directory() != NULL);
    assert(strncmp(ksd_install_runtime_directory(), private_dir,
                   strlen(private_dir)) == 0);

    set_runtime(NULL);
    assert(rmdir(private_dir) == 0);
    assert(rmdir(open_dir) == 0);
}

/* A grant the user chose to keep has to survive a logout, so the persistent
 * store cannot live under XDG_RUNTIME_DIR -- that directory is emptied at one,
 * and every grant would be asked for again at the next login. */
static void check_persistent_store_outlives_the_session(void)
{
    char state_dir[4096];
    char runtime_dir[4096];
    const char *base = getenv("TMPDIR");
    const char *persistent;
    int written;

    if (geteuid() == 0u)
        return;
    if (base == NULL)
        base = "/tmp";
    written = snprintf(state_dir, sizeof(state_dir), "%s/ksd-install-mode-%ld-state",
                       base, (long)getpid());
    assert(written > 0 && (size_t)written < sizeof(state_dir));
    written = snprintf(runtime_dir, sizeof(runtime_dir),
                       "%s/ksd-install-mode-%ld-rt", base, (long)getpid());
    assert(written > 0 && (size_t)written < sizeof(runtime_dir));
    (void)rmdir(state_dir);
    (void)rmdir(runtime_dir);
    assert(mkdir(state_dir, 0700) == 0);
    assert(mkdir(runtime_dir, 0700) == 0);

    set_runtime(runtime_dir);
    assert(setenv("XDG_STATE_HOME", state_dir, 1) == 0);
    persistent = ksd_install_persistent_directory();
    assert(persistent != NULL);
    assert(strncmp(persistent, state_dir, strlen(state_dir)) == 0);
    assert(strncmp(persistent, runtime_dir, strlen(runtime_dir)) != 0);

    assert(unsetenv("XDG_STATE_HOME") == 0);
    set_runtime(NULL);
    assert(rmdir(state_dir) == 0);
    assert(rmdir(runtime_dir) == 0);
}

/* A user socket is reachable by its owner and nobody else. The system socket
 * is deliberately open, because it is the machine's front door and is filtered
 * by peer credentials after the connection arrives. */
static void check_socket_mode_matches_the_installation(void)
{
    if (geteuid() == 0u)
        assert(ksd_install_socket_mode() == 0666u);
    else
        assert(ksd_install_socket_mode() == 0600u);
}

/* A system installation takes the library's own defaults. Returning a
 * user-shaped directory there would move a root daemon's grant store into a
 * path derived from an environment variable. */
static void check_system_installation_keeps_the_default_store(void)
{
    if (geteuid() != 0u)
        return;
    assert(ksd_install_persistent_directory() == NULL);
    assert(ksd_install_runtime_directory() == NULL);
}

int main(void)
{
    check_owner_follows_credentials_only();
    check_only_the_owner_is_trusted();
    check_user_socket_directory_is_held_to_the_system_standard();
    check_persistent_store_outlives_the_session();
    check_socket_mode_matches_the_installation();
    check_system_installation_keeps_the_default_store();
    return 0;
}
