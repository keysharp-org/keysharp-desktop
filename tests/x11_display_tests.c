#include "x11_display.h"

#include <assert.h>
#include <string.h>

static bool accepts(const char *value, const char *expected)
{
    char canonical[KSD_X11_DISPLAY_CAPACITY];
    if (!ksd_x11_display_parse(value, canonical, sizeof(canonical)))
        return false;
    return strcmp(canonical, expected) == 0;
}

static bool rejects(const char *value)
{
    char canonical[KSD_X11_DISPLAY_CAPACITY];
    return !ksd_x11_display_parse(value, canonical, sizeof(canonical));
}

int main(void)
{
    char long_colons[4096];

    assert(accepts(":0", ":0"));
    assert(accepts(":0.0", ":0.0"));
    assert(accepts(":10", ":10"));
    assert(accepts(":255.255", ":255.255"));

    /* A host part names a remote server and a transport prefix chooses a
     * transport. The broker will do neither on a caller's behalf, so both are
     * refused rather than passed through to the X library to interpret. */
    assert(rejects("host:0"));
    assert(rejects("tcp/host:0"));
    assert(rejects("unix/:0"));
    assert(rejects("localhost:0"));

    /* Shapes with nothing where a number must be. */
    assert(rejects(""));
    assert(rejects(":"));
    assert(rejects(":0."));
    assert(rejects("0"));
    assert(rejects(".0"));

    /* Anything trailing. This is how a shell metacharacter or a second screen
     * field would arrive. */
    assert(rejects(":0 ;rm"));
    assert(rejects(":0.0.0"));
    assert(rejects(":0x"));
    assert(rejects(":0 "));

    /* Out of range, and too many digits to be worth reading. */
    assert(rejects(":99999"));
    assert(rejects(":256"));
    assert(rejects(":0.256"));
    assert(rejects(":000000"));

    memset(long_colons, 58, sizeof(long_colons) - 1u);
    long_colons[sizeof(long_colons) - 1u] = 0;
    assert(rejects(long_colons));

    /* A buffer that cannot hold the longest accepted value is refused rather
     * than filled with a truncated one. */
    char narrow[4];
    assert(!ksd_x11_display_parse(":0", narrow, sizeof(narrow)));
    return 0;
}
