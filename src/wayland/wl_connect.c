#include "wl_internal.h"
#include "wl_outputs.h"
#include "wl_pointer.h"
#include "wl_hypr.h"
#include "wl_cosmic_windows.h"
#include "wl_keyboard.h"
#include "transport.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
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
        /* Attached here, at the instant of binding, and not one step later.
         * The compositor sends a toplevel event per existing window as soon as
         * the global is bound, and those arrive in the same burst as this
         * registry event: a listener added after the round trip that carried
         * this binding would miss every window that already existed. */
        ksd_wayland_toplevels_attach(connection);
    } else if (strcmp(interface,
                      zwlr_foreign_toplevel_manager_v1_interface.name) == 0
               && connection->toplevel_manager == NULL) {
        uint32_t bind_version = version < 3u ? version : 3u;
        connection->toplevel_manager = wl_registry_bind(registry, name,
            &zwlr_foreign_toplevel_manager_v1_interface, bind_version);
        ksd_wayland_wlr_toplevels_attach(connection);
    } else if (strcmp(interface, zcosmic_toplevel_info_v1_interface.name)
                   == 0) {
        ksd_wayland_cosmic_bind_info(connection, registry, name, version);
    } else if (strcmp(interface, zcosmic_toplevel_manager_v1_interface.name)
                   == 0) {
        ksd_wayland_cosmic_bind_manager(connection, registry, name, version);
    } else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name)
                   == 0
               && connection->screencopy_manager == NULL) {
        connection->screencopy_version = version < 3u ? version : 3u;
        connection->screencopy_manager = wl_registry_bind(registry, name,
            &zwlr_screencopy_manager_v1_interface,
            connection->screencopy_version);
    } else if (strcmp(interface,
                      ext_output_image_capture_source_manager_v1_interface.name)
                   == 0 && connection->output_source_manager == NULL) {
        connection->output_source_manager = wl_registry_bind(
            registry, name,
            &ext_output_image_capture_source_manager_v1_interface, 1u);
    } else if (strcmp(interface,
                      ext_image_copy_capture_manager_v1_interface.name) == 0
               && connection->image_copy_manager == NULL) {
        connection->image_copy_manager = wl_registry_bind(
            registry, name, &ext_image_copy_capture_manager_v1_interface,
            1u);
    } else if (strcmp(interface,
                      zwlr_virtual_pointer_manager_v1_interface.name) == 0
               && connection->pointer_manager == NULL) {
        uint32_t bind_version = version < 2u ? version : 2u;
        connection->pointer_manager = wl_registry_bind(registry, name,
            &zwlr_virtual_pointer_manager_v1_interface, bind_version);
    } else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0
               && connection->xdg_output_manager == NULL) {
        uint32_t bind_version = version < 3u ? version : 3u;
        connection->xdg_output_manager = wl_registry_bind(registry, name,
            &zxdg_output_manager_v1_interface, bind_version);
        ksd_wayland_outputs_bind_xdg(connection);
    } else if (strcmp(interface, wl_shm_interface.name) == 0
               && connection->shm == NULL) {
        connection->shm = wl_registry_bind(registry, name, &wl_shm_interface,
                                           1u);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        ksd_wayland_output_add(connection, registry, name, version);
    } else if (strcmp(interface, wl_seat_interface.name) == 0
               && connection->seat == NULL) {
        connection->seat_name = name;
        connection->seat = wl_registry_bind(registry, name,
                                            &wl_seat_interface, 1u);
        if (connection->seat != NULL)
            ksd_wayland_keyboard_attach(connection);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name)
{
    ksd_wayland_output_remove(data, name);
    (void)registry;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static int remaining_milliseconds(uint64_t deadline)
{
    uint64_t now = ksd_monotonic_milliseconds();

    if (now == 0u || now >= deadline)
        return 0;
    uint64_t remaining = deadline - now;
    return remaining > INT_MAX ? INT_MAX : (int)remaining;
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

bool ksd_wayland_dispatch_until(ksd_wayland *connection,
                                bool (*complete)(void *), void *data,
                                int timeout_ms)
{
    uint64_t deadline;

    if (connection == NULL || connection->display == NULL || complete == NULL
        || timeout_ms <= 0)
        return false;
    deadline = ksd_monotonic_milliseconds() + (uint32_t)timeout_ms;
    while (!complete(data)) {
        struct pollfd item = {
            .fd = wl_display_get_fd(connection->display),
            .events = POLLIN,
        };
        int remaining = remaining_milliseconds(deadline);
        int ready;

        if (remaining <= 0)
            return false;
        while (wl_display_prepare_read(connection->display) != 0) {
            if (wl_display_dispatch_pending(connection->display) < 0)
                return false;
            if (complete(data))
                return true;
        }
        if (wl_display_flush(connection->display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(connection->display);
            return false;
        }
        ready = poll(&item, 1u, remaining);
        if (ready <= 0) {
            wl_display_cancel_read(connection->display);
            if (ready < 0 && errno == EINTR)
                continue;
            return false;
        }
        if (wl_display_read_events(connection->display) < 0
            || wl_display_dispatch_pending(connection->display) < 0)
            return false;
    }
    return true;
}

/* wl_display_sync answers once everything sent before it has been processed,
 * which is what a round trip means. Written out rather than calling
 * wl_display_roundtrip because that call has no deadline of its own: a
 * compositor that stopped answering would hold this thread, and its worker
 * slot, for as long as it liked. Same hazard as a silent clipboard owner on
 * X11, answered the same way. */
bool ksd_wayland_roundtrip(ksd_wayland *connection, int timeout_ms)
{
    uint64_t deadline = ksd_monotonic_milliseconds() + (uint32_t)timeout_ms;
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
        int remaining = remaining_milliseconds(deadline);
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
    uint64_t deadline = ksd_monotonic_milliseconds() + (uint32_t)timeout_ms;
    size_t capacity = 4096u;
    uint8_t *buffer = malloc(capacity);
    size_t used = 0u;

    *data = NULL;
    *length = 0u;
    if (buffer == NULL)
        return false;
    for (;;) {
        struct pollfd item = { .fd = descriptor, .events = POLLIN };
        int remaining = remaining_milliseconds(deadline);
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

uint64_t ksd_wayland_new_handle(const ksd_wayland *connection)
{
    for (unsigned attempt = 0u; attempt < 16u; attempt++) {
        uint64_t value;
        ssize_t count;
        do {
            count = getrandom(&value, sizeof(value), 0);
        } while (count < 0 && errno == EINTR);
        if (count != (ssize_t)sizeof(value))
            return 0u;
        value &= INT64_MAX;
        bool duplicate = value == 0u;
        for (const ksd_wl_toplevel *item = connection->toplevels;
             !duplicate && item != NULL; item = item->next)
            duplicate = item->id == value;
        if (!duplicate)
            return value;
    }
    return 0u;
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
    connection->session_pid = getpid();
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
        || !ksd_wayland_roundtrip(connection, 2000)
        || (connection->keyboard != NULL && connection->keymap == NULL
            && !ksd_wayland_roundtrip(connection, 2000))) {
        ksd_wayland_close(connection);
        return KSD_STATUS_UNAVAILABLE;
    }
    ksd_wayland_pointer_create(connection);
    *out = connection;
    return KSD_STATUS_OK;
}

void ksd_wayland_close(ksd_wayland *connection)
{
    if (connection == NULL)
        return;
    if (connection->data_control != NULL)
        ext_data_control_manager_v1_destroy(connection->data_control);
    ksd_wayland_outputs_clear(connection);
    if (connection->xdg_output_manager != NULL)
        zxdg_output_manager_v1_destroy(connection->xdg_output_manager);
    if (connection->screencopy_manager != NULL)
        zwlr_screencopy_manager_v1_destroy(connection->screencopy_manager);
    if (connection->image_copy_manager != NULL)
        ext_image_copy_capture_manager_v1_destroy(
            connection->image_copy_manager);
    if (connection->output_source_manager != NULL)
        ext_output_image_capture_source_manager_v1_destroy(
            connection->output_source_manager);
    if (connection->shm != NULL)
        wl_shm_destroy(connection->shm);
    ksd_wayland_pointer_clear(connection);
    ksd_wayland_toplevels_clear(connection);
    ksd_wayland_cosmic_clear(connection);
    if (connection->toplevel_list != NULL)
        ext_foreign_toplevel_list_v1_destroy(connection->toplevel_list);
    if (connection->toplevel_manager != NULL)
        zwlr_foreign_toplevel_manager_v1_destroy(connection->toplevel_manager);
    ksd_wayland_keyboard_clear(connection);
    if (connection->seat != NULL)
        wl_seat_destroy(connection->seat);
    if (connection->registry != NULL)
        wl_registry_destroy(connection->registry);
    if (connection->display != NULL)
        wl_display_disconnect(connection->display);
    free(connection);
}

void ksd_wayland_set_session_pid(ksd_wayland *connection, pid_t session_pid)
{
    if (connection != NULL && session_pid > 0)
        connection->session_pid = session_pid;
}

pid_t ksd_wayland_session_pid(const ksd_wayland *connection)
{
    return connection == NULL ? 0 : connection->session_pid;
}

ksd_wayland_features ksd_wayland_supported(const ksd_wayland *connection)
{
    ksd_wayland_features features = { 0 };

    if (connection == NULL)
        return features;
    /* The clipboard needs a seat as well as the manager: a data device is got
     * for a seat, and a compositor with no seat has no selection to read. */
    features.data_control = connection->data_control != NULL
        && connection->seat != NULL;
    features.toplevel_list = connection->toplevel_list != NULL
        || connection->toplevel_manager != NULL;
    features.toplevel_control = connection->toplevel_manager != NULL
        && connection->seat != NULL;
    features.toplevel_active = connection->toplevel_manager != NULL
        || ksd_wayland_cosmic_can_list(connection);
    features.toplevel_focus = features.toplevel_control
        || ksd_wayland_cosmic_can_focus(connection);
    features.toplevel_close = connection->toplevel_manager != NULL
        || ksd_wayland_cosmic_can_close(connection);
    features.toplevel_state = connection->toplevel_manager != NULL
        || ksd_wayland_cosmic_can_set_state(connection);
    features.toplevel_control = features.toplevel_focus
        && features.toplevel_close && features.toplevel_state;
    features.screencopy = connection->shm != NULL
        && connection->outputs != NULL
        && (connection->screencopy_manager != NULL
            || (connection->output_source_manager != NULL
                && connection->image_copy_manager != NULL));
    bool hypr = ksd_wayland_hypr_available(connection->session_pid);
    features.absolute_pointer = (connection->virtual_pointer != NULL
        && connection->outputs != NULL) || hypr;
    features.cursor_position = hypr;
    features.keyboard_keymap = connection->keymap != NULL;
    return features;
}

bool ksd_wayland_connection_failed(const ksd_wayland *connection)
{
    return connection == NULL || connection->display == NULL
        || wl_display_get_error(connection->display) != 0;
}
