#include "wl_internal.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The ceiling on anything a compositor hands over through a pipe. Matches the
 * clipboard ceiling the rest of the service enforces, so a value that would be
 * refused later is refused before it is read rather than after. */
#define KSD_WL_MAX_TRANSFER KSD_MAX_TEXT_BYTES

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version)
{
    ksd_wayland *connection = data;

    (void)version;
    /* Version 1 is requested for each, not the compositor's version. These
     * protocols are used for exactly the requests version 1 defines, and
     * binding higher would accept events this code does not handle. */
    if (strcmp(interface, ext_data_control_manager_v1_interface.name) == 0
        && connection->data_control == NULL) {
        connection->data_control = wl_registry_bind(registry, name,
            &ext_data_control_manager_v1_interface, 1u);
    } else if (strcmp(interface,
                      ext_foreign_toplevel_list_v1_interface.name) == 0
               && connection->toplevel_list == NULL) {
        connection->toplevel_list = wl_registry_bind(registry, name,
            &ext_foreign_toplevel_list_v1_interface, 1u);
    } else if (strcmp(interface, wl_seat_interface.name) == 0
               && connection->seat == NULL) {
        connection->seat_name = name;
        connection->seat = wl_registry_bind(registry, name,
                                            &wl_seat_interface, 1u);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static int64_t monotonic_ms(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void sync_done(void *data, struct wl_callback *callback,
                      uint32_t serial)
{
    (void)callback;
    (void)serial;
    *(int *)data = 1;
}

static const struct wl_callback_listener sync_listener = {
    .done = sync_done,
};

/* wl_display_sync answers once everything sent before it has been processed,
 * which is what a round trip means. Written out rather than calling
 * wl_display_roundtrip because that call has no deadline of its own: a
 * compositor that stopped answering would hold this thread, and its worker
 * slot, for as long as it liked. Same hazard as a silent clipboard owner on
 * X11, answered the same way. */
bool ksd_wayland_roundtrip(ksd_wayland *connection, int timeout_ms)
{
    int64_t deadline = monotonic_ms() + timeout_ms;
    struct wl_callback *callback = wl_display_sync(connection->display);
    int done = 0;
    bool ok = false;

    if (callback == NULL)
        return false;
    wl_callback_add_listener(callback, &sync_listener, &done);

    while (!done) {
        struct pollfd item = {
            .fd = wl_display_get_fd(connection->display),
            .events = POLLIN,
        };
        int remaining = (int)(deadline - monotonic_ms());
        int ready;

        if (remaining <= 0)
            break;
        /* prepare_read must be paired with exactly one of read_events or
         * cancel_read on every path out, or the next reader deadlocks. */
        while (wl_display_prepare_read(connection->display) != 0) {
            if (wl_display_dispatch_pending(connection->display) < 0)
                goto finish;
            if (done)
                goto finish;
        }
        if (wl_display_flush(connection->display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(connection->display);
            goto finish;
        }
        ready = poll(&item, 1u, remaining);
        if (ready <= 0) {
            wl_display_cancel_read(connection->display);
            if (ready < 0 && errno == EINTR)
                continue;
            goto finish;
        }
        if (wl_display_read_events(connection->display) < 0)
            goto finish;
        if (wl_display_dispatch_pending(connection->display) < 0)
            goto finish;
    }
    ok = done != 0;

finish:
    wl_callback_destroy(callback);
    return ok;
}

bool ksd_wayland_drain(int descriptor, int timeout_ms, uint8_t **data,
                       size_t *length)
{
    int64_t deadline = monotonic_ms() + timeout_ms;
    size_t capacity = 4096u;
    uint8_t *buffer = malloc(capacity);
    size_t used = 0u;

    *data = NULL;
    *length = 0u;
    if (buffer == NULL)
        return false;
    for (;;) {
        struct pollfd item = { .fd = descriptor, .events = POLLIN };
        int remaining = (int)(deadline - monotonic_ms());
        ssize_t count;

        if (remaining <= 0) {
            free(buffer);
            return false;
        }
        int ready = poll(&item, 1u, remaining);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0) {
            free(buffer);
            return false;
        }
        if (used == capacity) {
            uint8_t *grown;

            /* Doubling, with the ceiling checked before the allocation rather
             * than after the read: a peer that keeps writing must be refused
             * without this process having first made room for it. */
            if (capacity >= KSD_WL_MAX_TRANSFER) {
                free(buffer);
                return false;
            }
            capacity = capacity * 2u > KSD_WL_MAX_TRANSFER
                ? KSD_WL_MAX_TRANSFER : capacity * 2u;
            grown = realloc(buffer, capacity);
            if (grown == NULL) {
                free(buffer);
                return false;
            }
            buffer = grown;
        }
        count = read(descriptor, buffer + used, capacity - used);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            free(buffer);
            return false;
        }
        if (count == 0)
            break;
        used += (size_t)count;
    }
    *data = buffer;
    *length = used;
    return true;
}

ksd_status ksd_wayland_open(const char *display, ksd_wayland **out)
{
    ksd_wayland *connection;

    if (out == NULL)
        return KSD_STATUS_INVALID_REQUEST;
    *out = NULL;
    connection = calloc(1u, sizeof(*connection));
    if (connection == NULL)
        return KSD_STATUS_INTERNAL;
    /* NULL means WAYLAND_DISPLAY, which is how every Wayland client finds its
     * compositor. The name is never taken from the calling client: the same
     * rule the X11 backend follows, for the same reason. */
    connection->display = wl_display_connect(display);
    if (connection->display == NULL) {
        free(connection);
        return KSD_STATUS_UNAVAILABLE;
    }
    connection->registry = wl_display_get_registry(connection->display);
    if (connection->registry == NULL) {
        wl_display_disconnect(connection->display);
        free(connection);
        return KSD_STATUS_UNAVAILABLE;
    }
    wl_registry_add_listener(connection->registry, &registry_listener,
                             connection);
    /* Two round trips, not one. The first delivers the registry globals; the
     * second lets anything those globals in turn advertise arrive before the
     * caller is told what this compositor supports. */
    if (!ksd_wayland_roundtrip(connection, 2000)
        || !ksd_wayland_roundtrip(connection, 2000)) {
        ksd_wayland_close(connection);
        return KSD_STATUS_UNAVAILABLE;
    }
    *out = connection;
    return KSD_STATUS_OK;
}

void ksd_wayland_close(ksd_wayland *connection)
{
    if (connection == NULL)
        return;
    if (connection->data_control != NULL)
        ext_data_control_manager_v1_destroy(connection->data_control);
    if (connection->toplevel_list != NULL)
        ext_foreign_toplevel_list_v1_destroy(connection->toplevel_list);
    if (connection->seat != NULL)
        wl_seat_destroy(connection->seat);
    if (connection->registry != NULL)
        wl_registry_destroy(connection->registry);
    if (connection->display != NULL)
        wl_display_disconnect(connection->display);
    free(connection);
}

ksd_wayland_features ksd_wayland_supported(const ksd_wayland *connection)
{
    ksd_wayland_features features = { false, false };

    if (connection == NULL)
        return features;
    /* The clipboard needs a seat as well as the manager: a data device is got
     * for a seat, and a compositor with no seat has no selection to read. */
    features.data_control = connection->data_control != NULL
        && connection->seat != NULL;
    features.toplevel_list = connection->toplevel_list != NULL;
    return features;
}
