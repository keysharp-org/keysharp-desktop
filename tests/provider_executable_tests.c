#include "provider_executable.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    const char *names[] = { "gnome-shell", "cinnamon", "kwin_wayland" };
    char path[256];
    for (size_t index = 0u; index < sizeof(names) / sizeof(names[0]); index++) {
        const char *name = names[index];
        snprintf(path, sizeof(path), "/usr/bin/%s", name);
        assert(ksd_provider_executable_matches(path, name));
        snprintf(path, sizeof(path), "/nix/store/example/bin/%s", name);
        assert(ksd_provider_executable_matches(path, name));
        snprintf(path, sizeof(path), "/nix/store/example/bin/.%s-wrapped", name);
        assert(ksd_provider_executable_matches(path, name));
        assert(!ksd_provider_executable_matches(path, "different-compositor"));
        snprintf(path, sizeof(path), "/home/user/.%s-wrapped", name);
        assert(!ksd_provider_executable_matches(path, name));
        snprintf(path, sizeof(path), "/nix/store-fake/example/.%s-wrapped", name);
        assert(!ksd_provider_executable_matches(path, name));
        snprintf(path, sizeof(path), "/nix/store/example/.%s-wrapped-extra", name);
        assert(!ksd_provider_executable_matches(path, name));
        snprintf(path, sizeof(path), "/nix/store/example/.%s-extra-wrapped", name);
        assert(!ksd_provider_executable_matches(path, name));
    }
    assert(!ksd_provider_executable_matches("", "gnome-shell"));
    assert(!ksd_provider_executable_matches("/nix/store/example/.", "gnome-shell"));
    assert(!ksd_provider_executable_matches("/nix/store/example/", "gnome-shell"));
    return 0;
}
