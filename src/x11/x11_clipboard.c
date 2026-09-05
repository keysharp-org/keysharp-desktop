#include "x11_clipboard.h"

#include "protocol.h"
#include "x11_internal.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The same budget the compositor providers are given for one clipboard call,
 * so a consumer sees one timeout whichever backend answered. The whole read
 * shares it: an owner that answers each INCR chunk just inside a per-chunk
 * limit could otherwise hold the worker indefinitely. */
#define KSD_X11_CLIPBOARD_TIMEOUT_MS 5000

#ifdef KSD_X11_TESTING
/* Lowered by the tests so the hostile-owner case -- a client that takes the
 * selection and then never answers -- can be gated without spending the whole
 * production budget on it. */
int ksd_x11_clipboard_timeout_ms = KSD_X11_CLIPBOARD_TIMEOUT_MS;
#define KSD_X11_TIMEOUT ksd_x11_clipboard_timeout_ms
#else
#define KSD_X11_TIMEOUT KSD_X11_CLIPBOARD_TIMEOUT_MS
#endif

/* One GetProperty asks for this many 32-bit words at a time. */
#define KSD_X11_PROPERTY_WORDS (256u * 1024u)

typedef struct clipboard_atoms {
    xcb_atom_t clipboard;
    xcb_atom_t targets;
    xcb_atom_t incr;
    xcb_atom_t utf8_string;
    xcb_atom_t property;
} clipboard_atoms;

static xcb_atom_t intern(xcb_connection_t *connection, const char *name)
{
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(connection,
        xcb_intern_atom(connection, 0, (uint16_t)strlen(name), name), NULL);
    xcb_atom_t atom = XCB_ATOM_NONE;

    if (reply != NULL) {
        atom = reply->atom;
        free(reply);
    }
    return atom;
}

static bool load_atoms(xcb_connection_t *connection, clipboard_atoms *atoms)
{
    atoms->clipboard = intern(connection, "CLIPBOARD");
    atoms->targets = intern(connection, "TARGETS");
    atoms->incr = intern(connection, "INCR");
    atoms->utf8_string = intern(connection, "UTF8_STRING");
    /* The selection is delivered into a property on our own window, named so
     * it cannot collide with one another client put there. */
    atoms->property = intern(connection, "KEYSHARP_DESKTOP_SELECTION");
    return atoms->clipboard != XCB_ATOM_NONE
        && atoms->targets != XCB_ATOM_NONE && atoms->incr != XCB_ATOM_NONE
        && atoms->utf8_string != XCB_ATOM_NONE
        && atoms->property != XCB_ATOM_NONE;
}

static void deadline_from_now(struct timespec *deadline, int milliseconds)
{
    clock_gettime(CLOCK_MONOTONIC, deadline);
    deadline->tv_sec += milliseconds / 1000;
    deadline->tv_nsec += (long)(milliseconds % 1000) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec += 1;
        deadline->tv_nsec -= 1000000000L;
    }
}

static int remaining_ms(const struct timespec *deadline)
{
    struct timespec now;
    long long milliseconds;

    clock_gettime(CLOCK_MONOTONIC, &now);
    milliseconds = (long long)(deadline->tv_sec - now.tv_sec) * 1000
        + (deadline->tv_nsec - now.tv_nsec) / 1000000L;
    if (milliseconds < 0)
        return 0;
    if (milliseconds > KSD_X11_TIMEOUT)
        return KSD_X11_TIMEOUT;
    return (int)milliseconds;
}

/* Waits for one event, or gives up at the deadline. The clipboard owner is
 * another application, and nothing obliges it to answer at all: without a
 * deadline any client could park this worker for as long as it liked by
 * claiming the selection and then going quiet. */
static xcb_generic_event_t *wait_event(xcb_connection_t *connection,
                                       const struct timespec *deadline)
{
    for (;;) {
        xcb_generic_event_t *event = xcb_poll_for_event(connection);
        struct pollfd descriptor;
        int ready;
        int timeout;

        if (event != NULL)
            return event;
        if (xcb_connection_has_error(connection) != 0)
            return NULL;
        timeout = remaining_ms(deadline);
        if (timeout <= 0)
            return NULL;
        /* Flushed before waiting, or the request this is waiting on might
         * still be sitting in the output buffer. */
        xcb_flush(connection);
        descriptor.fd = xcb_get_file_descriptor(connection);
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        ready = poll(&descriptor, 1u, timeout);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0)
            return NULL;
    }
}

/* Collects one property, appending to out. Returns false on any failure,
 * including a property larger than the ceiling. */
static bool append_property(xcb_connection_t *connection, xcb_window_t window,
                            xcb_atom_t property, bool delete,
                            xcb_atom_t *type, ksd_buffer *out)
{
    xcb_get_property_reply_t *reply = xcb_get_property_reply(connection,
        xcb_get_property(connection, delete ? 1u : 0u, window, property,
                         XCB_GET_PROPERTY_TYPE_ANY, 0u,
                         KSD_X11_PROPERTY_WORDS), NULL);
    int length;
    bool ok;

    if (reply == NULL)
        return false;
    if (type != NULL)
        *type = reply->type;
    length = xcb_get_property_value_length(reply);
    ok = length >= 0
        && ksd_buffer_bytes(out, xcb_get_property_value(reply),
                            (size_t)length);
    free(reply);
    return ok;
}

/* Asks the current owner to convert the selection to one target and collects
 * the answer, following an INCR transfer if the owner starts one. */
static ksd_status selection_read(xcb_connection_t *connection,
                                 xcb_window_t window,
                                 const clipboard_atoms *atoms,
                                 xcb_atom_t target, ksd_buffer *out,
                                 xcb_atom_t *type)
{
    struct timespec deadline;
    xcb_generic_event_t *event;
    xcb_selection_notify_event_t *notify;
    xcb_atom_t delivered = XCB_ATOM_NONE;
    bool incremental = false;

    deadline_from_now(&deadline, KSD_X11_TIMEOUT);
    xcb_delete_property(connection, window, atoms->property);
    xcb_convert_selection(connection, window, atoms->clipboard, target,
                          atoms->property, XCB_CURRENT_TIME);
    xcb_flush(connection);

    for (;;) {
        event = wait_event(connection, &deadline);
        if (event == NULL)
            return KSD_STATUS_TIMEOUT;
        if ((event->response_type & 0x7fu) == XCB_SELECTION_NOTIFY) {
            notify = (xcb_selection_notify_event_t *)event;
            if (notify->requestor == window
                && notify->selection == atoms->clipboard) {
                delivered = notify->property;
                free(event);
                break;
            }
        }
        free(event);
    }
    /* A property of None is the owner saying it will not convert to this
     * target. That is an answer, not a failure of this service. */
    if (delivered == XCB_ATOM_NONE)
        return KSD_STATUS_UNSUPPORTED;

    if (!append_property(connection, window, delivered, true, type, out))
        return KSD_STATUS_INTERNAL;
    incremental = type != NULL && *type == atoms->incr;
    if (!incremental)
        return KSD_STATUS_OK;

    /* An INCR transfer: the first property held only a size estimate, and the
     * owner now writes the value in pieces, each announced by a PropertyNotify
     * and consumed by deleting the property. A zero-length piece ends it. */
    out->length = 0u;
    for (;;) {
        xcb_property_notify_event_t *changed;
        size_t before = out->length;

        event = wait_event(connection, &deadline);
        if (event == NULL)
            return KSD_STATUS_TIMEOUT;
        if ((event->response_type & 0x7fu) != XCB_PROPERTY_NOTIFY) {
            free(event);
            continue;
        }
        changed = (xcb_property_notify_event_t *)event;
        if (changed->window != window || changed->atom != delivered
            || changed->state != XCB_PROPERTY_NEW_VALUE) {
            free(event);
            continue;
        }
        free(event);
        if (!append_property(connection, window, delivered, true, NULL, out))
            return KSD_STATUS_INTERNAL;
        if (out->length == before)
            return KSD_STATUS_OK;
        if (out->length > KSD_MAX_TEXT_BYTES)
            return KSD_STATUS_RESOURCE_EXHAUSTED;
    }
}

/* The window the selection is delivered to. InputOnly because it is never
 * drawn, and never mapped, so it cannot appear on screen or in a window list.
 * PropertyChangeMask has to be selected before the request, or the first INCR
 * piece would arrive with nothing listening for it. */
static xcb_window_t make_requestor(xcb_connection_t *connection,
                                   const xcb_screen_t *screen)
{
    uint32_t values[] = { XCB_EVENT_MASK_PROPERTY_CHANGE };
    xcb_window_t window = xcb_generate_id(connection);

    xcb_create_window(connection, XCB_COPY_FROM_PARENT, window, screen->root,
                      0, 0, 1u, 1u, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
                      XCB_COPY_FROM_PARENT, XCB_CW_EVENT_MASK, values);
    return window;
}

static void bytes_result(ksd_buffer *value, ksd_operation_result *result)
{
    if (!ksd_buffer_frame_text(value, KSD_MAX_TEXT_BYTES)
        || !ksd_result_copy(result, value->data, (uint32_t)value->length))
        ksd_result_error(result, KSD_STATUS_INTERNAL, 0u, "out of memory");
}

/* Latin-1 is what a STRING selection holds by definition, and every Latin-1
 * byte is a code point, so the conversion cannot fail. Done rather than
 * refused because plenty of older applications offer STRING and nothing
 * else, and refusing them would report an empty clipboard that is not empty. */
static bool latin1_to_utf8(const ksd_buffer *in, ksd_buffer *out)
{
    for (size_t index = 0u; index < in->length; index++) {
        uint8_t byte = ((const uint8_t *)in->data)[index];
        uint8_t encoded[2];

        if (byte < 0x80u) {
            if (!ksd_buffer_bytes(out, &byte, 1u))
                return false;
            continue;
        }
        encoded[0] = (uint8_t)(0xc0u | (byte >> 6));
        encoded[1] = (uint8_t)(0x80u | (byte & 0x3fu));
        if (!ksd_buffer_bytes(out, encoded, 2u))
            return false;
    }
    return true;
}

void ksd_x11_clipboard_text(ksd_x11 *connection, ksd_operation_result *result)
{
    xcb_connection_t *c = connection->connection;
    clipboard_atoms atoms;
    xcb_window_t window;
    ksd_buffer value;
    xcb_atom_t type = XCB_ATOM_NONE;
    ksd_status status;

    if (!load_atoms(c, &atoms)) {
        ksd_result_error(result, KSD_STATUS_INTERNAL, 0u,
                         "could not name the clipboard on this display");
        return;
    }
    window = make_requestor(c, connection->screen);
    ksd_buffer_init(&value, KSD_MAX_TEXT_BYTES);
    status = selection_read(c, window, &atoms, atoms.utf8_string, &value,
                            &type);
    if (status == KSD_STATUS_UNSUPPORTED) {
        /* No UTF8_STRING on offer. STRING is the older spelling and is still
         * all some applications publish. */
        ksd_buffer latin1;
        ksd_buffer_init(&latin1, KSD_MAX_TEXT_BYTES);
        status = selection_read(c, window, &atoms, XCB_ATOM_STRING, &latin1,
                                &type);
        if (status == KSD_STATUS_OK && !latin1_to_utf8(&latin1, &value))
            status = KSD_STATUS_RESOURCE_EXHAUSTED;
        ksd_buffer_clear(&latin1);
    }
    if (status == KSD_STATUS_UNSUPPORTED) {
        /* Nothing on the clipboard offers text. An empty string is the same
         * answer the providers give, so a consumer sees one shape. */
        value.length = 0u;
        status = KSD_STATUS_OK;
    }
    if (status != KSD_STATUS_OK)
        ksd_result_error(result, status, 0u,
                         status == KSD_STATUS_TIMEOUT
                             ? "the clipboard owner did not answer"
                             : "could not read the clipboard");
    else if (!ksd_utf8_valid(value.data, value.length, false))
        /* The bytes came from another application, and the wire promises the
         * consumer valid UTF-8. Passing invalid bytes through would put the
         * consumer's parser in a state this service promised it would not. */
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "the clipboard holds text this service cannot"
                         " represent");
    else
        bytes_result(&value, result);
    ksd_buffer_clear(&value);
    xcb_destroy_window(c, window);
    xcb_flush(c);
}

void ksd_x11_clipboard_content(ksd_x11 *connection, const uint8_t *mimetype,
                               uint32_t mimetype_length,
                               ksd_operation_result *result)
{
    xcb_connection_t *c = connection->connection;
    clipboard_atoms atoms;
    xcb_window_t window;
    ksd_buffer value;
    xcb_atom_t target;
    xcb_atom_t type = XCB_ATOM_NONE;
    ksd_status status;
    char name[KSD_MAX_MIMETYPE_BYTES + 1u];

    if (mimetype_length == 0u || mimetype_length > KSD_MAX_MIMETYPE_BYTES) {
        ksd_result_error(result, KSD_STATUS_INVALID_REQUEST, 0u,
                         "that is not a mimetype this service will ask for");
        return;
    }
    memcpy(name, mimetype, mimetype_length);
    name[mimetype_length] = '\0';
    if (!load_atoms(c, &atoms)) {
        ksd_result_error(result, KSD_STATUS_INTERNAL, 0u,
                         "could not name the clipboard on this display");
        return;
    }
    /* The canonical text mimetype is spelled UTF8_STRING on X11, and an owner
     * that offers text will answer to that name rather than to the mimetype. */
    target = strcmp(name, KSD_CLIPBOARD_TEXT_MIMETYPE) == 0
        ? atoms.utf8_string : intern(c, name);
    if (target == XCB_ATOM_NONE) {
        ksd_result_error(result, KSD_STATUS_UNSUPPORTED, 0u,
                         "the clipboard does not offer that format");
        return;
    }
    window = make_requestor(c, connection->screen);
    ksd_buffer_init(&value, KSD_MAX_TEXT_BYTES);
    status = selection_read(c, window, &atoms, target, &value, &type);
    if (status != KSD_STATUS_OK)
        ksd_result_error(result, status, 0u,
                         status == KSD_STATUS_TIMEOUT
                             ? "the clipboard owner did not answer"
                             : "the clipboard does not offer that format");
    else
        bytes_result(&value, result);
    ksd_buffer_clear(&value);
    xcb_destroy_window(c, window);
    xcb_flush(c);
}

void ksd_x11_clipboard_mimetypes(ksd_x11 *connection,
                                 ksd_operation_result *result)
{
    xcb_connection_t *c = connection->connection;
    clipboard_atoms atoms;
    xcb_window_t window;
    ksd_buffer value;
    ksd_buffer tail;
    xcb_atom_t type = XCB_ATOM_NONE;
    ksd_status status;
    size_t count = 0u;
    bool offers_text = false;
    bool ok = true;
    const xcb_atom_t *offered;
    size_t offered_count;

    if (!load_atoms(c, &atoms)) {
        ksd_result_error(result, KSD_STATUS_INTERNAL, 0u,
                         "could not name the clipboard on this display");
        return;
    }
    window = make_requestor(c, connection->screen);
    ksd_buffer_init(&value, KSD_MAX_TEXT_BYTES);
    status = selection_read(c, window, &atoms, atoms.targets, &value, &type);
    if (status == KSD_STATUS_UNSUPPORTED) {
        /* No owner, or one that will not list its targets. An empty list is
         * the honest answer and the one the providers give. */
        value.length = 0u;
        status = KSD_STATUS_OK;
    }
    if (status != KSD_STATUS_OK) {
        ksd_result_error(result, status, 0u,
                         status == KSD_STATUS_TIMEOUT
                             ? "the clipboard owner did not answer"
                             : "could not read the clipboard formats");
        ksd_buffer_clear(&value);
        xcb_destroy_window(c, window);
        xcb_flush(c);
        return;
    }
    /* The buffer came from malloc, so it is aligned for any type, and TARGETS
     * is a list of 32-bit atoms. A trailing partial atom is dropped rather
     * than read: the division is what makes that so. */
    offered = (const xcb_atom_t *)(const void *)value.data;
    offered_count = value.length / sizeof(xcb_atom_t);
    if (offered_count > KSD_MAX_MIMETYPES)
        offered_count = KSD_MAX_MIMETYPES;

    /* Two passes, because the count is framed before the names. Every atom
     * name is fetched once and reused. */
    char **names = offered_count == 0u
        ? NULL : calloc(offered_count, sizeof(char *));

    for (size_t index = 0u; index < offered_count && names != NULL; index++) {
        xcb_get_atom_name_reply_t *reply = xcb_get_atom_name_reply(c,
            xcb_get_atom_name(c, offered[index]), NULL);
        int length;
        const char *text;

        if (reply == NULL)
            continue;
        length = xcb_get_atom_name_name_length(reply);
        text = xcb_get_atom_name_name(reply);
        /* UTF8_STRING and STRING are how X11 spells text, so they are reported
         * as the mimetype every other backend reports for it. The X11 target
         * names themselves are not mimetypes and would mean nothing to a
         * consumer that asked for one. */
        if ((length == 11 && memcmp(text, "UTF8_STRING", 11u) == 0)
            || (length == 6 && memcmp(text, "STRING", 6u) == 0)) {
            offers_text = true;
        } else if (length > 0 && (size_t)length <= KSD_MAX_MIMETYPE_BYTES
                   && memchr(text, '/', (size_t)length) != NULL
                   && ksd_utf8_valid((const uint8_t *)text, (size_t)length,
                                     false)) {
            /* A target with a slash in it is a mimetype. TARGETS, MULTIPLE,
             * TIMESTAMP and the rest of the ICCCM vocabulary are not, and
             * reporting them as formats would invent capabilities. */
            names[count] = malloc((size_t)length + 1u);
            if (names[count] != NULL) {
                memcpy(names[count], text, (size_t)length);
                names[count][length] = '\0';
                count++;
            }
        }
        free(reply);
    }

    ksd_buffer_init(&tail, KSD_MAX_TEXT_BYTES);
    ok = ksd_buffer_u32(&tail, (uint32_t)(count + (offers_text ? 1u : 0u)))
        && ksd_buffer_u32(&tail, 0u);
    if (ok && offers_text) {
        size_t length = strlen(KSD_CLIPBOARD_TEXT_MIMETYPE);
        ok = ksd_buffer_u32(&tail, (uint32_t)length)
            && ksd_buffer_bytes(&tail, KSD_CLIPBOARD_TEXT_MIMETYPE, length);
    }
    for (size_t index = 0u; ok && index < count; index++) {
        size_t length = strlen(names[index]);
        ok = ksd_buffer_u32(&tail, (uint32_t)length)
            && ksd_buffer_bytes(&tail, names[index], length);
    }
    if (!ok || !ksd_result_copy(result, tail.data, (uint32_t)tail.length))
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "clipboard format list is invalid or too large");
    for (size_t index = 0u; index < count; index++)
        free(names[index]);
    free(names);
    ksd_buffer_clear(&tail);
    ksd_buffer_clear(&value);
    xcb_destroy_window(c, window);
    xcb_flush(c);
}
