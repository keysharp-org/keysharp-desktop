#include "backend.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define TEST_BUS_ADDRESS \
    "DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent/keysharp-desktop-bus"

static ksd_backend child_backend(const char *desktop)
{
    char bus[] = TEST_BUS_ADDRESS;
    char program[] = "backend-session-tests";
    char mode[] = "resolve";
    char entry[256];
    char *arguments[] = { program, mode, NULL };
    char *environment[] = { bus, NULL, NULL };
    int status = 0;

    if (desktop != NULL) {
        int written = snprintf(entry, sizeof(entry),
                               "XDG_CURRENT_DESKTOP=%s", desktop);
        assert(written > 0 && (size_t)written < sizeof(entry));
        environment[1] = entry;
    }
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        execve("/proc/self/exe", arguments, environment);
        _exit(127);
    }
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) != 127);
    return (ksd_backend)WEXITSTATUS(status);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "resolve") == 0) {
        assert(ksd_backend_resolve() == KSD_BACKEND_NONE);
        return ksd_backend_session_unsupported()
            ? (int)KSD_BACKEND_GENERIC : (int)KSD_BACKEND_NONE;
    }
    assert(argc == 1);
    assert(child_backend(NULL) == KSD_BACKEND_GENERIC);
    assert(child_backend("") == KSD_BACKEND_GENERIC);
    assert(child_backend("sway") == KSD_BACKEND_GENERIC);
    assert(child_backend("Hyprland") == KSD_BACKEND_GENERIC);
    assert(child_backend("COSMIC") == KSD_BACKEND_GENERIC);
    assert(child_backend("wlroots:river") == KSD_BACKEND_GENERIC);
    assert(child_backend("GNOME") == KSD_BACKEND_NONE);
    assert(child_backend("ubuntu:GNOME") == KSD_BACKEND_NONE);
    assert(child_backend("X-Cinnamon") == KSD_BACKEND_NONE);
    assert(child_backend("KDE") == KSD_BACKEND_NONE);
    assert(ksd_backend_operations(KSD_BACKEND_GENERIC) == 0u);
    return 0;
}
