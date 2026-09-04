#include "wl_clipboard.h"

#include "protocol_io.h"
#include "wl_internal.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The same budget the compositor providers get for one clipboard call, so a
 * consumer sees one timeout whichever backend answered. */
#define KSD_WL_CLIPBOARD_TIMEOUT_MS 5000

#ifdef KSD_WAYLAND_TESTING
int ksd_wayland_clipboard_timeout_ms = KSD_WL_CLIPBOARD_TIMEOUT_MS;
#define KSD_WL_TIMEOUT ksd_wayland_clipboard_timeout_ms
#else
#define KSD_WL_TIMEOUT KSD_WL_CLIPBOARD_TIMEOUT_MS
#endif

/* One offer and the mime types it advertised. The compositor delivers the
 * types one event at a time before saying which offer is the selection, so
 * they are collected as they arrive and read once the selection names one. */
typedef struct offer_state {
    struct ext_data_control_offer_v1 *offer;
    char *types[KSD_MAX_MIMETYPES];
    size_t count;
} offer_state;

typedef struct clipboard_state {
    struct ext_data_control_device_v1 *device;
    /* The offer the compositor most recently named as the selection, and the
     * types it carries. NULL when the clipboard is empty, which is a state to
     * report rather than a failure. */
    offer_state *selection;
    offer_state *pending[KSD_MAX_MIMETYPES];
    size_t pending_count;
    bool finished;
} clipboard_state;

static void offer_free(offer_state *state)
{
    if (state == NULL)
        return;
    for (size_t index = 0u; index < state->count; index++)
        free(state->types[index]);
    if (state->offer != NULL)
        ext_data_control_offer_v1_destroy(state->offer);
    free(state);
}

static void offer_mime(void *data, struct ext_data_control_offer_v1 *offer,
                       const char *mime_type)
{
    offer_state *state = data;

    (void)offer;
    /* A compositor relays whatever the owning application offered, so the
     * count is not something this side chooses. Beyond the ceiling the extra
     * types are dropped rather than the whole offer refused: a clipboard with
     * too many formats still has the first ones, and they are the useful ones. */
    if (state == NULL || state->count >= KSD_MAX_MIMETYPES
        || mime_type == NULL)
        return;
    if (strlen(mime_type) == 0u || strlen(mime_type) > KSD_MAX_MIMETYPE_BYTES)
        return;
    if (!ksd_utf8_valid((const uint8_t *)mime_type, strlen(mime_type), false))
        return;
    state->types[state->count] = strdup(mime_type);
    if (state->types[state->count] != NULL)
        state->count++;
}

static const struct ext_data_control_offer_v1_listener offer_listener = {
    .offer = offer_mime,
};

static void device_data_offer(void *data,
                              struct ext_data_control_device_v1 *device,
                              struct ext_data_control_offer_v1 *offer)
{
    clipboard_state *state = data;
    offer_state *fresh;

    (void)device;
    if (state == NULL || offer == NULL)
        return;
    fresh = calloc(1u, sizeof(*fresh));
    if (fresh == NULL) {
        ext_data_control_offer_v1_destroy(offer);
        return;
    }
    fresh->offer = offer;
    ext_data_control_offer_v1_add_listener(offer, &offer_listener, fresh);
    /* Held until the selection event says which offer is current. An offer
     * that is never named is one the compositor superseded, and it is freed
     * with the rest. */
    if (state->pending_count < KSD_MAX_MIMETYPES)
        state->pending[state->pending_count++] = fresh;
    else
        offer_free(fresh);
}

static void device_selection(void *data,
                             struct ext_data_control_device_v1 *device,
                             struct ext_data_control_offer_v1 *offer)
{
    clipboard_state *state = data;

    (void)device;
    if (state == NULL)
        return;
    /* A null offer means the clipboard is empty. That is an answer, and the
     * providers give the same one. */
    state->selection = NULL;
    if (offer == NULL)
        return;
    for (size_t index = 0u; index < state->pending_count; index++) {
        if (state->pending[index] != NULL
            && state->pending[index]->offer == offer) {
            state->selection = state->pending[index];
            return;
        }
    }
}

static void device_finished(void *data,
                            struct ext_data_control_device_v1 *device)
{
    clipboard_state *state = data;

    (void)device;
    /* The compositor has taken the device away; nothing more will arrive on
     * it, and using it again is a protocol error rather than a slow answer. */
    if (state != NULL)
        state->finished = true;
}

static void device_primary(void *data,
                           struct ext_data_control_device_v1 *device,
                           struct ext_data_control_offer_v1 *offer)
{
    (void)data;
    (void)device;
    /* The primary selection is the middle-click buffer, which is a different
     * thing from the clipboard and is not what any of these verbs mean. The
     * offer still has to be destroyed or it leaks for the connection's life. */
    if (offer != NULL)
        ext_data_control_offer_v1_destroy(offer);
}

static const struct ext_data_control_device_v1_listener device_listener = {
    .data_offer = device_data_offer,
    .selection = device_selection,
    .finished = device_finished,
    .primary_selection = device_primary,
};

static void clipboard_clear(clipboard_state *state)
{
    for (size_t index = 0u; index < state->pending_count; index++)
        offer_free(state->pending[index]);
    state->pending_count = 0u;
    state->selection = NULL;
    if (state->device != NULL)
        ext_data_control_device_v1_destroy(state->device);
    state->device = NULL;
}

/* Opens the device and pumps events until the compositor has told us what the
 * current selection is. Every verb needs exactly this before it can act. */
static ksd_status clipboard_begin(ksd_wayland *connection,
                                  clipboard_state *state)
{
    memset(state, 0, sizeof(*state));
    if (!ksd_wayland_supported(connection).data_control)
        return KSD_STATUS_UNSUPPORTED;
    state->device = ext_data_control_manager_v1_get_data_device(
        connection->data_control, connection->seat);
    if (state->device == NULL)
        return KSD_STATUS_UNAVAILABLE;
    ext_data_control_device_v1_add_listener(state->device, &device_listener,
                                            state);
    /* Two round trips. The first brings the data_offer and its mime types, the
     * second the selection event that says which offer is current. A single
     * trip can deliver them in either order depending on the compositor. */
    if (!ksd_wayland_roundtrip(connection, KSD_WL_TIMEOUT)
        || !ksd_wayland_roundtrip(connection, KSD_WL_TIMEOUT)) {
        clipboard_clear(state);
        return KSD_STATUS_TIMEOUT;
    }
    if (state->finished) {
        clipboard_clear(state);
        return KSD_STATUS_UNAVAILABLE;
    }
    return KSD_STATUS_OK;
}

/* Asks the owner for one format and reads it out of a pipe. The write end is
 * closed here as soon as it is handed over, so the read sees end-of-file when
 * the owner is done rather than blocking on a descriptor this process still
 * holds open. */
static ksd_status receive_type(ksd_wayland *connection, offer_state *offer,
                               const char *mimetype, uint8_t **data,
                               size_t *length)
{
    int pair[2];

    if (pipe2(pair, O_CLOEXEC) != 0)
        return KSD_STATUS_INTERNAL;
    ext_data_control_offer_v1_receive(offer->offer, mimetype, pair[1]);
    /* Flushed before the write end is dropped, or the request naming that
     * descriptor might not have been sent yet. */
    if (!ksd_wayland_roundtrip(connection, KSD_WL_TIMEOUT)) {
        close(pair[0]);
        close(pair[1]);
        return KSD_STATUS_TIMEOUT;
    }
    close(pair[1]);
    if (!ksd_wayland_drain(pair[0], KSD_WL_TIMEOUT, data, length)) {
        close(pair[0]);
        return KSD_STATUS_TIMEOUT;
    }
    close(pair[0]);
    return KSD_STATUS_OK;
}

static bool offers_type(const offer_state *offer, const char *mimetype)
{
    for (size_t index = 0u; index < offer->count; index++) {
        if (strcmp(offer->types[index], mimetype) == 0)
            return true;
    }
    return false;
}

static void bytes_result(const uint8_t *value, size_t length,
                         ksd_operation_result *result)
{
    ksd_buffer framed;

    if (length > KSD_MAX_TEXT_BYTES) {
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "clipboard content is too large");
        return;
    }
    ksd_buffer_init(&framed, KSD_MAX_TEXT_BYTES + 4u);
    if (!ksd_buffer_u32(&framed, (uint32_t)length)
        || !ksd_buffer_bytes(&framed, value, length)
        || !ksd_result_copy(result, framed.data, (uint32_t)framed.length))
        ksd_result_error(result, KSD_STATUS_INTERNAL, 0u, "out of memory");
    ksd_buffer_clear(&framed);
}

void ksd_wayland_clipboard_mimetypes(ksd_wayland *connection,
                                     ksd_operation_result *result)
{
    clipboard_state state;
    ksd_buffer tail;
    ksd_status status = clipboard_begin(connection, &state);
    size_t count = 0u;
    bool ok;

    if (status != KSD_STATUS_OK) {
        ksd_result_error(result, status, 0u,
                         "could not read the clipboard formats");
        return;
    }
    count = state.selection == NULL ? 0u : state.selection->count;
    ksd_buffer_init(&tail, KSD_MAX_TEXT_BYTES);
    ok = ksd_buffer_u32(&tail, (uint32_t)count) && ksd_buffer_u32(&tail, 0u);
    for (size_t index = 0u; ok && index < count; index++) {
        const char *name = state.selection->types[index];
        size_t length = strlen(name);

        ok = ksd_buffer_u32(&tail, (uint32_t)length)
            && ksd_buffer_bytes(&tail, name, length);
    }
    if (!ok || !ksd_result_copy(result, tail.data, (uint32_t)tail.length))
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "clipboard format list is invalid or too large");
    ksd_buffer_clear(&tail);
    clipboard_clear(&state);
}

void ksd_wayland_clipboard_content(ksd_wayland *connection,
                                   const uint8_t *mimetype,
                                   uint32_t mimetype_length,
                                   ksd_operation_result *result)
{
    clipboard_state state;
    char name[KSD_MAX_MIMETYPE_BYTES + 1u];
    uint8_t *data = NULL;
    size_t length = 0u;
    ksd_status status;

    if (mimetype == NULL || mimetype_length == 0u
        || mimetype_length > KSD_MAX_MIMETYPE_BYTES) {
        ksd_result_error(result, KSD_STATUS_INVALID_REQUEST, 0u,
                         "that is not a mimetype this service will ask for");
        return;
    }
    memcpy(name, mimetype, mimetype_length);
    name[mimetype_length] = '\0';

    status = clipboard_begin(connection, &state);
    if (status != KSD_STATUS_OK) {
        ksd_result_error(result, status, 0u, "could not read the clipboard");
        return;
    }
    /* Asking for a format the owner did not offer is refused here rather than
     * sent. The protocol says an unoffered mime type gives an empty transfer,
     * which is indistinguishable from an empty clipboard entry; refusing says
     * which of the two it was. */
    if (state.selection == NULL || !offers_type(state.selection, name)) {
        ksd_result_error(result, KSD_STATUS_UNSUPPORTED, 0u,
                         "the clipboard does not offer that format");
        clipboard_clear(&state);
        return;
    }
    status = receive_type(connection, state.selection, name, &data, &length);
    if (status != KSD_STATUS_OK)
        ksd_result_error(result, status, 0u,
                         "the clipboard owner did not answer");
    else
        bytes_result(data, length, result);
    free(data);
    clipboard_clear(&state);
}

void ksd_wayland_clipboard_text(ksd_wayland *connection,
                                ksd_operation_result *result)
{
    static const char *const candidates[] = {
        KSD_CLIPBOARD_TEXT_MIMETYPE, "text/plain;charset=UTF-8",
        "text/plain", "UTF8_STRING", "STRING",
    };
    clipboard_state state;
    uint8_t *data = NULL;
    size_t length = 0u;
    ksd_status status = clipboard_begin(connection, &state);
    const char *chosen = NULL;

    if (status != KSD_STATUS_OK) {
        ksd_result_error(result, status, 0u, "could not read the clipboard");
        return;
    }
    /* In preference order. An application that publishes text usually offers
     * several spellings of it, and the canonical one is not always among them
     * -- an X11 client seen through Xwayland offers the X11 target names. */
    for (size_t index = 0u; chosen == NULL && index < 5u; index++) {
        if (state.selection != NULL
            && offers_type(state.selection, candidates[index]))
            chosen = candidates[index];
    }
    if (chosen == NULL) {
        /* Nothing on the clipboard is text. An empty string is the answer the
         * providers give, so a consumer sees one shape. */
        bytes_result((const uint8_t *)"", 0u, result);
        clipboard_clear(&state);
        return;
    }
    status = receive_type(connection, state.selection, chosen, &data, &length);
    if (status != KSD_STATUS_OK)
        ksd_result_error(result, status, 0u,
                         "the clipboard owner did not answer");
    else if (!ksd_utf8_valid(data, length, false))
        /* The bytes came from another application and the wire promises the
         * consumer valid UTF-8. Passing them through would put the consumer's
         * parser in a state this service said it would not. */
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "the clipboard holds text this service cannot"
                         " represent");
    else
        bytes_result(data, length, result);
    free(data);
    clipboard_clear(&state);
}
