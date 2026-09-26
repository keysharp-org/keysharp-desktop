#ifndef KEYSHARP_DESKTOP_PROVIDER_EXECUTABLE_H
#define KEYSHARP_DESKTOP_PROVIDER_EXECUTABLE_H

#include <stdbool.h>
#include <string.h>

static inline bool ksd_provider_executable_matches(const char *path,
                                                   const char *expected)
{
    const char *basename = strrchr(path, '/');
    basename = basename == NULL ? path : basename + 1u;
    if (strcmp(basename, expected) == 0)
        return true;

    /* Nix wrappers exec a hidden binary inside the immutable store. */
    size_t length = strlen(expected);
    return strncmp(path, "/nix/store/", 11u) == 0
        && basename[0] == '.'
        && strncmp(basename + 1u, expected, length) == 0
        && strcmp(basename + 1u + length, "-wrapped") == 0;
}

#endif
