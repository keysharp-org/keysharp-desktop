#include "session_environ.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define KSD_ENVIRON_LIMIT (64u * 1024u)

/* Reads one variable out of the environment of the registered session daemon.
 * The daemon is the party the authority authenticated and revalidates on every
 * operation, which is why its environment is the one entitled to name a
 * display; taking that from the calling client would let a client point the
 * broker at a server it started. Runs after privileges are dropped, so the
 * open is done as the user and root never touches a user-named path. */
bool ksd_session_environ_value(pid_t pid, const char *name,
                               char *destination, size_t capacity)
{
    char path[64];
    char environment[KSD_ENVIRON_LIMIT + 1u];
    size_t offset = 0u;
    size_t name_length = strlen(name);
    int length = snprintf(path, sizeof(path), "/proc/%ld/environ", (long)pid);

    if (pid <= 0 || length <= 0 || (size_t)length >= sizeof(path))
        return false;
    int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return false;
    while (offset < sizeof(environment) - 1u) {
        ssize_t count = read(descriptor, environment + offset,
                             sizeof(environment) - 1u - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        offset += (size_t)count;
    }
    close(descriptor);
    environment[offset] = 0;

    for (size_t index = 0u; index < offset;) {
        const char *entry = environment + index;
        size_t remaining = offset - index;
        size_t entry_length = strnlen(entry, remaining);

        if (entry_length == remaining)
            return false;
        if (entry_length > name_length && entry[name_length] == '='
            && memcmp(entry, name, name_length) == 0) {
            const char *value = entry + name_length + 1u;
            size_t value_length = entry_length - name_length - 1u;

            if (value_length == 0u || value_length >= capacity)
                return false;
            memcpy(destination, value, value_length);
            destination[value_length] = 0;
            return true;
        }
        index += entry_length + 1u;
    }
    return false;
}
