#include "x11_display.h"

#include <stdint.h>
#include <stdio.h>

#define KSD_X11_MAX_DISPLAY 255u
#define KSD_X11_MAX_SCREEN 255u

/* Reads one bounded run of digits. Rejects an empty run, a run longer than
 * the bound can justify, and any value over it, so a caller cannot spend the
 * parser on a long string of digits either. */
static bool read_number(const char **cursor, uint32_t limit, uint32_t *value)
{
    const char *scan = *cursor;
    uint32_t parsed = 0u;
    size_t digits = 0u;

    while (*scan >= 48 && *scan <= 57) {
        if (digits == 5u)
            return false;
        parsed = parsed * 10u + (uint32_t)(*scan - 48);
        digits++;
        scan++;
    }
    if (digits == 0u || parsed > limit)
        return false;
    *cursor = scan;
    *value = parsed;
    return true;
}

bool ksd_x11_display_parse(const char *value, char *canonical,
                           size_t capacity)
{
    uint32_t display = 0u;
    uint32_t screen = 0u;
    bool has_screen = false;
    int written;

    if (value == NULL || canonical == NULL
        || capacity < KSD_X11_DISPLAY_CAPACITY || value[0] != 58)
        return false;

    const char *cursor = value + 1;
    if (!read_number(&cursor, KSD_X11_MAX_DISPLAY, &display))
        return false;
    if (*cursor == 46) {
        cursor++;
        if (!read_number(&cursor, KSD_X11_MAX_SCREEN, &screen))
            return false;
        has_screen = true;
    }
    /* Nothing may follow. A trailing byte is how a shell metacharacter, a
     * second screen field or a stray space would arrive. */
    if (*cursor != 0)
        return false;

    written = has_screen
        ? snprintf(canonical, capacity, ":%u.%u", display, screen)
        : snprintf(canonical, capacity, ":%u", display);
    return written > 0 && (size_t)written < capacity;
}
