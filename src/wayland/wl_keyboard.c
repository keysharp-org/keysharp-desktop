#include "wl_keyboard.h"

#include "protocol_io.h"
#include "wl_internal.h"

#include <errno.h>
#include <glib.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

#define KSD_WL_MAX_KEYMAP_BYTES (4u * 1024u * 1024u)

static void keymap(void *data, struct wl_keyboard *keyboard,
                    uint32_t format, int32_t fd, uint32_t size)
{
    ksd_wayland *connection = data;
    struct stat status;
    char *text = NULL;
    struct xkb_context *context = NULL;
    struct xkb_keymap *map = NULL;
    (void)keyboard;

    free(connection->keymap_text);
    connection->keymap_text = NULL;
    xkb_keymap_unref(connection->keymap);
    connection->keymap = NULL;
    g_clear_pointer(&connection->keymap_revision, g_free);

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size < 2u
        || size > KSD_WL_MAX_KEYMAP_BYTES || fstat(fd, &status) != 0
        || !S_ISREG(status.st_mode) || status.st_size < (off_t)size)
        goto done;
    text = malloc(size);
    if (text == NULL)
        goto done;
    size_t offset = 0u;
    while (offset < size) {
        ssize_t count = pread(fd, text + offset, size - offset, (off_t)offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            goto done;
        offset += (size_t)count;
    }
    if (text[size - 1u] != '\0'
        || !ksd_utf8_valid((const uint8_t *)text, size - 1u, false))
        goto done;
    context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (context == NULL)
        goto done;
    map = xkb_keymap_new_from_string(context, text, XKB_KEYMAP_FORMAT_TEXT_V1,
                                    XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (map == NULL)
        goto done;
    connection->keymap_text = text;
    connection->keymap = map;
    connection->keymap_revision = g_compute_checksum_for_string(
        G_CHECKSUM_SHA256, text, -1);
    text = NULL;
    map = NULL;

done:
    free(text);
    xkb_keymap_unref(map);
    xkb_context_unref(context);
    close(fd);
}

static void enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                  struct wl_surface *surface, struct wl_array *keys)
{
    (void)data; (void)keyboard; (void)serial; (void)surface; (void)keys;
}

static void leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                  struct wl_surface *surface)
{
    (void)data; (void)keyboard; (void)serial; (void)surface;
}

static void key(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                uint32_t time, uint32_t code, uint32_t state)
{
    (void)data; (void)keyboard; (void)serial;
    (void)time; (void)code; (void)state;
}

static void modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                      uint32_t depressed, uint32_t latched, uint32_t locked,
                      uint32_t group)
{
    /* This worker has no focused surface. Seat keymaps are global, whereas
     * these masks describe a client's keyboard focus and cannot establish
     * the globally active group or lock state. */
    (void)data; (void)keyboard; (void)serial;
    (void)depressed; (void)latched; (void)locked; (void)group;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keymap,
    .enter = enter,
    .leave = leave,
    .key = key,
    .modifiers = modifiers,
};

void ksd_wayland_keyboard_clear(ksd_wayland *connection)
{
    if (connection->keyboard != NULL)
        wl_keyboard_destroy(connection->keyboard);
    connection->keyboard = NULL;
    free(connection->keymap_text);
    connection->keymap_text = NULL;
    g_free(connection->keymap_revision);
    connection->keymap_revision = NULL;
    xkb_keymap_unref(connection->keymap);
    connection->keymap = NULL;
}

static void capabilities(void *data, struct wl_seat *seat, uint32_t mask)
{
    ksd_wayland *connection = data;
    if ((mask & WL_SEAT_CAPABILITY_KEYBOARD) == 0u) {
        ksd_wayland_keyboard_clear(connection);
    } else if (connection->keyboard == NULL) {
        connection->keyboard = wl_seat_get_keyboard(seat);
        if (connection->keyboard != NULL)
            wl_keyboard_add_listener(connection->keyboard, &keyboard_listener,
                                       connection);
    }
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = capabilities,
};

void ksd_wayland_keyboard_attach(ksd_wayland *connection)
{
    wl_seat_add_listener(connection->seat, &seat_listener, connection);
}

static void json_string(GString *out, const char *text)
{
    g_string_append_c(out, '"');
    for (const unsigned char *at = (const unsigned char *)text;
         *at != 0u; at++) {
        if (*at == '"' || *at == '\\')
            g_string_append_c(out, '\\');
        if (*at < 0x20u)
            g_string_append_printf(out, "\\u%04x", (unsigned)*at);
        else
            g_string_append_c(out, (char)*at);
    }
    g_string_append_c(out, '"');
}

void ksd_wayland_keyboard_state(ksd_wayland *connection,
                                ksd_operation_result *result)
{
    ksd_wayland_keyboard_state_since(connection, NULL, 0u, result);
}

void ksd_wayland_keyboard_state_since(ksd_wayland *connection,
    const uint8_t *revision, uint32_t length, ksd_operation_result *result)
{
    if (!ksd_wayland_roundtrip(connection, 2000)
        || connection->keymap == NULL) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                         "the compositor has not supplied a keyboard keymap");
        return;
    }
    bool unchanged = length == 64u && revision != NULL
        && memcmp(revision, connection->keymap_revision, 64u) == 0;
    GString *out = g_string_new("{\"ok\":true,\"validFields\":[");
    if (!unchanged)
        g_string_append(out, "\"keymap\",");
    g_string_append(out, "\"layouts\",\"mapRevision\"]");
    if (!unchanged) {
        g_string_append(out, ",\"keymap\":");
        json_string(out, connection->keymap_text);
    }
    g_string_append(out, ",\"mapRevision\":");
    json_string(out, connection->keymap_revision);
    g_string_append(out, ",\"layouts\":[");
    xkb_layout_index_t count = xkb_keymap_num_layouts(connection->keymap);
    for (xkb_layout_index_t index = 0u; index < count; index++) {
        if (index != 0u)
            g_string_append_c(out, ',');
        const char *name = xkb_keymap_layout_get_name(connection->keymap, index);
        json_string(out, name == NULL ? "" : name);
    }
    g_string_append(out, "]}");
    ksd_buffer framed;
    ksd_buffer_init(&framed, KSD_MAX_TEXT_BYTES);
    if (!ksd_buffer_bytes(&framed, out->str, out->len)
        || !ksd_buffer_frame_text(&framed, KSD_MAX_TEXT_BYTES)
        || !ksd_result_copy(result, framed.data, (uint32_t)framed.length))
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "the keyboard keymap exceeds the response limit");
    ksd_buffer_clear(&framed);
    g_string_free(out, TRUE);
}
