#include "backend.h"
#include "backend_protocol.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define TEST_BUS_ADDRESS \
    "DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent/keysharp-desktop-bus"

typedef struct session_environment {
    const char *desktop;
    const char *session_type;
    const char *wayland_display;
    const char *display;
} session_environment;

/* Runs the resolver in a child with exactly the environment given, because
 * the rule it implements is about what the environment says and nothing else. */
static ksd_backend child_resolve(const session_environment *session)
{
    char bus[] = TEST_BUS_ADDRESS;
    char program[] = "backend-session-tests";
    char mode[] = "resolve";
    char desktop[256];
    char session_type[256];
    char wayland_display[256];
    char display[256];
    char *arguments[] = { program, mode, NULL };
    char *environment[6] = { bus, NULL, NULL, NULL, NULL, NULL };
    size_t slot = 1u;
    int status = 0;

    struct { const char *format; const char *value; char *buffer; } vars[] = {
        { "XDG_CURRENT_DESKTOP=%s", session->desktop, desktop },
        { "XDG_SESSION_TYPE=%s", session->session_type, session_type },
        { "WAYLAND_DISPLAY=%s", session->wayland_display, wayland_display },
        { "DISPLAY=%s", session->display, display },
    };
    for (size_t index = 0u; index < 4u; index++) {
        if (vars[index].value == NULL)
            continue;
        int written = snprintf(vars[index].buffer, 256u, vars[index].format,
                               vars[index].value);
        assert(written > 0 && (size_t)written < 256u);
        assert(slot < 5u);
        environment[slot++] = vars[index].buffer;
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

static ksd_backend child_backend(const char *desktop)
{
    session_environment session = { desktop, NULL, NULL, NULL };
    return child_resolve(&session);
}

static void check_session_type_table(void)
{
    /* An X11 session with no provider on the bus resolves to X11. */
    session_environment x11 = { "GNOME", "x11", NULL, ":0" };
    assert(child_resolve(&x11) == KSD_BACKEND_X11);

    /* The session type is matched whole. A substring test would call this an
     * X11 session, and the tree already had that bug for the desktop name. */
    session_environment fallback = { "GNOME", "x11-fallback", NULL, ":0" };
    assert(child_resolve(&fallback) != KSD_BACKEND_X11);

    /* THE XWAYLAND TRAP. A Wayland session almost always has DISPLAY set,
     * because XWayland sets it. Consulting DISPLAY would call nearly every
     * Wayland session an X11 one. */
    session_environment xwayland = { "GNOME", "wayland", "wayland-0", ":0" };
    assert(child_resolve(&xwayland) != KSD_BACKEND_X11);

    /* Same trap with the session type absent entirely. */
    session_environment untyped = { "GNOME", NULL, "wayland-0", ":0" };
    assert(child_resolve(&untyped) != KSD_BACKEND_X11);

    /* A Wayland display present alongside an x11 session type is a session
     * that can reach Wayland, so it is not treated as X11 either. */
    session_environment stray = { "GNOME", "x11", "wayland-0", ":0" };
    assert(child_resolve(&stray) != KSD_BACKEND_X11);

    /* An empty WAYLAND_DISPLAY is not a Wayland display. */
    session_environment empty = { "GNOME", "x11", "", ":0" };
    assert(child_resolve(&empty) == KSD_BACKEND_X11);

    /* Case is not significant in the session type. */
    session_environment upper = { "GNOME", "X11", NULL, ":0" };
    assert(child_resolve(&upper) == KSD_BACKEND_X11);

    /* X11 with no DISPLAY at all still resolves: the rule never reads it. */
    session_environment nodisplay = { "GNOME", "x11", NULL, NULL };
    assert(child_resolve(&nodisplay) == KSD_BACKEND_X11);

    assert(ksd_backend_operations(KSD_BACKEND_X11) != 0u);
    assert(ksd_backend_x11_route(KSD_BACKEND_X11, false) == 0u);
}
/* The registration mask may only ever narrow. A daemon whose compositor lacks
 * a capability reports less and the authority believes it; a daemon that asks
 * for more than its backend supports gets the intersection, not the union,
 * because otherwise a compromised daemon could advertise capability the
 * service cannot deliver and every caller would be told a lie. */
static void check_registration_mask(void)
{
    uint64_t mask = 0u;
    const uint16_t version = KSD_BACKEND_REGISTRATION_VERSION;

    /* Asking for everything yields exactly what the backend supports. */
    assert(ksd_backend_registration_mask(KSD_BACKEND_GNOME, version, 0u,
                                         ~UINT64_C(0), &mask));
    assert(mask == ksd_backend_operations(KSD_BACKEND_GNOME));

    /* Asking for less yields less: this is the whole point of carrying it. */
    assert(ksd_backend_registration_mask(KSD_BACKEND_GNOME, version, 0u,
                                         KSD_OPERATION_WINDOW_LIST, &mask));
    assert(mask == (KSD_OPERATION_WINDOW_LIST
                    & ksd_backend_operations(KSD_BACKEND_GNOME)));

    /* Asking for something the backend does not have yields nothing extra. */
    assert(ksd_backend_registration_mask(KSD_BACKEND_X11, version, 0u,
                                         ~UINT64_C(0), &mask));
    assert(mask == ksd_backend_operations(KSD_BACKEND_X11));
    assert((mask & KSD_OPERATION_CAPTURE_AREA) == 0u);

    /* A backend that serves nothing cannot be talked into serving something. */
    assert(ksd_backend_registration_mask(KSD_BACKEND_GENERIC, version, 0u,
                                         ~UINT64_C(0), &mask));
    assert(mask == 0u);

    /* Only KWin hands over a callback socket, because only a KWin script
     * cannot be reached on the session bus. */
    assert(ksd_backend_registration_mask(KSD_BACKEND_KWIN, version,
                                         KSD_BACKEND_FLAG_PROVIDER_FD,
                                         ~UINT64_C(0), &mask));
    assert(!ksd_backend_registration_mask(KSD_BACKEND_GNOME, version,
                                          KSD_BACKEND_FLAG_PROVIDER_FD,
                                          ~UINT64_C(0), &mask));
    assert(!ksd_backend_registration_mask(KSD_BACKEND_X11, version,
                                          KSD_BACKEND_FLAG_PROVIDER_FD,
                                          ~UINT64_C(0), &mask));

    /* An unknown flag is a record this service does not understand, which is
     * a rejected registration rather than one with the flag ignored. */
    assert(!ksd_backend_registration_mask(KSD_BACKEND_KWIN, version, 0x8000u,
                                          0u, &mask));

    /* A registration narrows what is reported; its absence falls back to the
     * static table. Without a registration the answer must not be zero, or a
     * backend would appear to serve nothing before its daemon registers. */
    assert(ksd_backend_reported_operations(KSD_BACKEND_GNOME, false, 0u)
           == ksd_backend_operations(KSD_BACKEND_GNOME));
    assert(ksd_backend_reported_operations(KSD_BACKEND_GNOME, true,
                                           KSD_OPERATION_WINDOW_LIST)
           == KSD_OPERATION_WINDOW_LIST);
    assert(ksd_backend_reported_operations(KSD_BACKEND_GNOME, true, 0u) == 0u);
    /* A registration can never widen, even if a stored value somehow did. */
    assert(ksd_backend_reported_operations(KSD_BACKEND_X11, true,
                                           ~UINT64_C(0))
           == ksd_backend_operations(KSD_BACKEND_X11));

    /* And a version it does not speak. */
    assert(!ksd_backend_registration_mask(KSD_BACKEND_GNOME,
                                          (uint16_t)(version - 1u), 0u, 0u,
                                          &mask));
    assert(!ksd_backend_registration_mask(KSD_BACKEND_GNOME,
                                          (uint16_t)(version + 1u), 0u, 0u,
                                          &mask));
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "resolve") == 0) {
        ksd_backend resolved = ksd_backend_resolve();
        if (resolved != KSD_BACKEND_NONE)
            return (int)resolved;
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
    check_registration_mask();
    check_session_type_table();
    assert(ksd_backend_operations(KSD_BACKEND_GENERIC) == 0u);
    return 0;
}
