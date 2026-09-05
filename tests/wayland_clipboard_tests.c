#include "ext-data-control-v1-server-protocol.h"
#include "ext-foreign-toplevel-list-v1-server-protocol.h"
#include "ext-image-capture-source-v1-server-protocol.h"
#include "ext-image-copy-capture-v1-server-protocol.h"
#include "cosmic-toplevel-info-v2-server-protocol.h"
#include "cosmic-toplevel-management-v1-server-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-server-protocol.h"
#include "wlr-screencopy-unstable-v1-server-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-server-protocol.h"
#include "xdg-output-unstable-v1-server-protocol.h"
#include "operation_result.h"
#include "protocol.h"
#include "protocol_io.h"
#include "wl_clipboard.h"
#include "wl_capture.h"
#include "wl_pointer.h"
#include "wl_connect.h"
#include "wl_windows.h"
#include "wl_keyboard.h"

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server.h>

/* There is no Wayland compositor on a build machine, and a backend that is
 * only compiled is not a backend anyone should trust. So this test IS the
 * compositor: a real Wayland server that speaks exactly the part of
 * ext-data-control the client under test uses, run in a child process while
 * the parent drives the verbs. The same shape as the fake window manager the
 * X11 control tests stand up, for the same reason. */

#define CLIP_TEXT "wayland \xc3\xa4\xc3\xb6 text"
#define CLIP_CSV "x,y,z"
#define TEXT_MIME "text/plain;charset=utf-8"
#define CSV_MIME "text/csv"

extern int ksd_wayland_clipboard_timeout_ms;

/* ---------------------------------------------------------------- server -- */

static void offer_receive(struct wl_client *client, struct wl_resource *offer,
                          const char *mime_type, int32_t fd)
{
    const char *body = strcmp(mime_type, CSV_MIME) == 0 ? CLIP_CSV : CLIP_TEXT;

    (void)client;
    (void)offer;
    /* The owner writes and closes. The reader sees end-of-file because of the
     * close, which is the whole transfer protocol here. */
    if (write(fd, body, strlen(body)) < 0)
        perror("write");
    close(fd);
}

static void offer_destroy(struct wl_client *client, struct wl_resource *offer)
{
    (void)client;
    wl_resource_destroy(offer);
}

static const struct ext_data_control_offer_v1_interface offer_impl = {
    .receive = offer_receive,
    .destroy = offer_destroy,
};

static void device_set_selection(struct wl_client *client,
                                 struct wl_resource *device,
                                 struct wl_resource *source)
{
    (void)client;
    (void)device;
    (void)source;
}

static void device_destroy(struct wl_client *client,
                           struct wl_resource *device)
{
    (void)client;
    wl_resource_destroy(device);
}

static void device_set_primary(struct wl_client *client,
                               struct wl_resource *device,
                               struct wl_resource *source)
{
    (void)client;
    (void)device;
    (void)source;
}

static const struct ext_data_control_device_v1_interface device_impl = {
    .set_selection = device_set_selection,
    .destroy = device_destroy,
    .set_primary_selection = device_set_primary,
};

/* Set by the parent through the environment so one server binary can present
 * either an ordinary clipboard, an empty one, or one that never answers. */
static const char *server_mode(void)
{
    const char *mode = getenv("KSD_TEST_WL_MODE");

    return mode == NULL ? "normal" : mode;
}

static void manager_create_source(struct wl_client *client,
                                  struct wl_resource *manager, uint32_t id)
{
    (void)manager;
    /* Nothing in this test writes the clipboard, but the request must still be
     * answered with a live object or the client sees a protocol error. */
    struct wl_resource *source = wl_resource_create(client,
        &ext_data_control_source_v1_interface, 1, id);

    if (source != NULL)
        wl_resource_set_implementation(source, NULL, NULL, NULL);
}

static void manager_get_device(struct wl_client *client,
                               struct wl_resource *manager, uint32_t id,
                               struct wl_resource *seat)
{
    struct wl_resource *device;
    struct wl_resource *offer;
    const char *mode = server_mode();

    (void)manager;
    (void)seat;
    device = wl_resource_create(client,
        &ext_data_control_device_v1_interface, 1, id);
    if (device == NULL)
        return;
    wl_resource_set_implementation(device, &device_impl, NULL, NULL);

    if (strcmp(mode, "silent") == 0) {
        /* Bound, and then no selection event ever. This is NOT a timeout: the
         * compositor is still answering, so the round trips complete and the
         * honest report is an empty clipboard, exactly as if it were empty.
         * A client cannot tell "not sent yet" from "nothing there", and
         * pretending otherwise would invent a distinction the protocol does
         * not offer. */
        return;
    }
    if (strcmp(mode, "wedged") == 0) {
        /* Stops answering altogether, mid-request. Nothing further leaves the
         * server, including the sync reply, which is what the deadline is
         * actually for: without it this thread and the worker slot it holds
         * are the compositor's for as long as it likes. */
        for (;;)
            pause();
    }
    if (strcmp(mode, "empty") == 0) {
        /* An explicit empty clipboard: the selection event with no offer. */
        ext_data_control_device_v1_send_selection(device, NULL);
        return;
    }
    offer = wl_resource_create(client, &ext_data_control_offer_v1_interface,
                               1, 0);
    if (offer == NULL)
        return;
    wl_resource_set_implementation(offer, &offer_impl, NULL, NULL);
    /* The order the protocol specifies: the offer object, then the types it
     * carries, and only then which offer is the selection. */
    ext_data_control_device_v1_send_data_offer(device, offer);
    ext_data_control_offer_v1_send_offer(offer, TEXT_MIME);
    ext_data_control_offer_v1_send_offer(offer, CSV_MIME);
    ext_data_control_device_v1_send_selection(device, offer);
}

static void manager_destroy(struct wl_client *client,
                            struct wl_resource *manager)
{
    (void)client;
    wl_resource_destroy(manager);
}

static const struct ext_data_control_manager_v1_interface manager_impl = {
    .create_data_source = manager_create_source,
    .get_data_device = manager_get_device,
    .destroy = manager_destroy,
};

static void bind_manager(struct wl_client *client, void *data,
                         uint32_t version, uint32_t id)
{
    struct wl_resource *resource = wl_resource_create(client,
        &ext_data_control_manager_v1_interface, (int)version, id);

    (void)data;
    if (resource != NULL)
        wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

static void seat_get_pointer(struct wl_client *c, struct wl_resource *r,
                             uint32_t id)
{
    (void)c; (void)r; (void)id;
}
static void seat_get_keyboard(struct wl_client *c, struct wl_resource *r,
                              uint32_t id)
{
    (void)r;
    static const char map[] =
        "xkb_keymap { xkb_keycodes { minimum=8; maximum=255; <AD01>=24; };"
        "xkb_types { type \"ONE_LEVEL\" { modifiers=None; map[None]=Level1; }; };"
        "xkb_compatibility {}; xkb_symbols { name[Group1]=\"Test layout\";"
        "key <AD01> { type=\"ONE_LEVEL\", [q] }; }; };";
    struct wl_resource *keyboard = wl_resource_create(c,
        &wl_keyboard_interface, 1, id);
    assert(keyboard != NULL);
    int descriptor = memfd_create("ksd-test-keymap", MFD_CLOEXEC);
    assert(descriptor >= 0);
    assert(write(descriptor, map, sizeof(map)) == (ssize_t)sizeof(map));
    wl_keyboard_send_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                             descriptor, sizeof(map));
    wl_keyboard_send_modifiers(keyboard, 1u, 1u, 2u, 4u, 3u);
    if (strcmp(getenv("KSD_TEST_WL_MODE"), "keyboard-invalid") == 0)
        wl_keyboard_send_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP,
                                 descriptor, sizeof(map));
    close(descriptor);
}
static void seat_get_touch(struct wl_client *c, struct wl_resource *r,
                           uint32_t id)
{
    (void)c; (void)r; (void)id;
}
static void seat_release(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_destroy(r);
}

static const struct wl_seat_interface seat_impl = {
    .get_pointer = seat_get_pointer,
    .get_keyboard = seat_get_keyboard,
    .get_touch = seat_get_touch,
    .release = seat_release,
};

/* ------------------------------------------------------- toplevel list -- */

static void handle_destroy(struct wl_client *client, struct wl_resource *r)
{
    (void)client;
    wl_resource_destroy(r);
}

static const struct ext_foreign_toplevel_handle_v1_interface handle_impl = {
    .destroy = handle_destroy,
};

static void list_stop(struct wl_client *client, struct wl_resource *r)
{
    (void)client;
    (void)r;
}

static void list_destroy(struct wl_client *client, struct wl_resource *r)
{
    (void)client;
    wl_resource_destroy(r);
}

static const struct ext_foreign_toplevel_list_v1_interface list_impl = {
    .stop = list_stop,
    .destroy = list_destroy,
};

static unsigned next_test_toplevel;

static void send_toplevel(struct wl_client *client, struct wl_resource *list,
                          const char *title, const char *app_id,
                          const char *identifier)
{
    struct wl_resource *handle = wl_resource_create(client,
        &ext_foreign_toplevel_handle_v1_interface, 1, 0);

    if (handle == NULL)
        return;
    next_test_toplevel++;
    wl_resource_set_implementation(handle, &handle_impl,
        (void *)(uintptr_t)next_test_toplevel, NULL);
    ext_foreign_toplevel_list_v1_send_toplevel(list, handle);
    ext_foreign_toplevel_handle_v1_send_identifier(handle, identifier);
    ext_foreign_toplevel_handle_v1_send_title(handle, title);
    ext_foreign_toplevel_handle_v1_send_app_id(handle, app_id);
    ext_foreign_toplevel_handle_v1_send_done(handle);
}

static void bind_list(struct wl_client *client, void *data, uint32_t version,
                      uint32_t id)
{
    struct wl_resource *resource = wl_resource_create(client,
        &ext_foreign_toplevel_list_v1_interface, (int)version, id);

    (void)data;
    if (resource == NULL)
        return;
    next_test_toplevel = 0u;
    wl_resource_set_implementation(resource, &list_impl, NULL, NULL);
    /* Every existing window is reported as soon as the global is bound, which
     * is why the client attaches its listener before the first round trip. */
    send_toplevel(client, resource, "first \"quoted\" window", "org.test.One",
                  "id-one");
    send_toplevel(client, resource, "second", "org.test.Two", "id-two");
}

/* ------------------------------------------------------------- COSMIC -- */

typedef struct test_cosmic_toplevel {
    struct wl_resource *foreign;
    struct wl_resource *info;
} test_cosmic_toplevel;

static struct wl_resource *cosmic_output_resource;

static void destroy_test_cosmic_toplevel(struct wl_resource *resource)
{
    free(wl_resource_get_user_data(resource));
}

static void cosmic_handle_destroy(struct wl_client *client,
                                  struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static const struct zcosmic_toplevel_handle_v1_interface cosmic_handle_impl = {
    .destroy = cosmic_handle_destroy,
};

static void cosmic_send_state(struct wl_resource *handle, uint32_t state)
{
    struct wl_array states;

    wl_array_init(&states);
    if (state < 32u) {
        uint32_t *slot = wl_array_add(&states, sizeof(*slot));
        assert(slot != NULL);
        *slot = state;
    }
    zcosmic_toplevel_handle_v1_send_state(handle, &states);
    wl_array_release(&states);
}

static void cosmic_info_stop(struct wl_client *client,
                             struct wl_resource *resource)
{
    (void)client;
    (void)resource;
}

static void cosmic_info_get_toplevel(struct wl_client *client,
                                     struct wl_resource *info, uint32_t id,
                                     struct wl_resource *foreign)
{
    struct wl_resource *handle = wl_resource_create(
        client, &zcosmic_toplevel_handle_v1_interface, 2, id);
    test_cosmic_toplevel *state = calloc(1u, sizeof(*state));
    uintptr_t index = (uintptr_t)wl_resource_get_user_data(foreign);

    assert(handle != NULL && state != NULL);
    assert(cosmic_output_resource != NULL);
    state->foreign = foreign;
    state->info = info;
    wl_resource_set_implementation(handle, &cosmic_handle_impl, state,
                                   destroy_test_cosmic_toplevel);
    cosmic_send_state(handle, index == 1u
        ? ZCOSMIC_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED
        : ZCOSMIC_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED);
    zcosmic_toplevel_handle_v1_send_geometry(
        handle, cosmic_output_resource, (int32_t)index, 0, 2, 1);
    zcosmic_toplevel_info_v1_send_done(info);
}

static const struct zcosmic_toplevel_info_v1_interface cosmic_info_impl = {
    .stop = cosmic_info_stop,
    .get_cosmic_toplevel = cosmic_info_get_toplevel,
};

static void bind_cosmic_info(struct wl_client *client, void *data,
                             uint32_t version, uint32_t id)
{
    uint32_t supported = version < 2u ? version : 2u;
    struct wl_resource *resource = wl_resource_create(
        client, &zcosmic_toplevel_info_v1_interface, (int)supported, id);

    (void)data;
    assert(resource != NULL);
    wl_resource_set_implementation(resource, &cosmic_info_impl, NULL, NULL);
}

static test_cosmic_toplevel *cosmic_state(struct wl_resource *toplevel)
{
    test_cosmic_toplevel *state = wl_resource_get_user_data(toplevel);
    assert(state != NULL);
    return state;
}

static void cosmic_manager_destroy(struct wl_client *client,
                                   struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static void cosmic_manager_close(struct wl_client *client,
                                 struct wl_resource *resource,
                                 struct wl_resource *toplevel)
{
    test_cosmic_toplevel *state = cosmic_state(toplevel);
    (void)client;
    (void)resource;
    ext_foreign_toplevel_handle_v1_send_closed(state->foreign);
}

static void cosmic_manager_activate(struct wl_client *client,
                                    struct wl_resource *resource,
                                    struct wl_resource *toplevel,
                                    struct wl_resource *seat)
{
    test_cosmic_toplevel *state = cosmic_state(toplevel);
    (void)client;
    (void)resource;
    (void)seat;
    cosmic_send_state(toplevel,
        ZCOSMIC_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED);
    zcosmic_toplevel_info_v1_send_done(state->info);
}

static void cosmic_manager_set_maximized(struct wl_client *client,
                                         struct wl_resource *resource,
                                         struct wl_resource *toplevel)
{
    test_cosmic_toplevel *state = cosmic_state(toplevel);
    (void)client;
    (void)resource;
    cosmic_send_state(toplevel,
        ZCOSMIC_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED);
    zcosmic_toplevel_info_v1_send_done(state->info);
}

static void cosmic_manager_unset_maximized(struct wl_client *client,
                                           struct wl_resource *resource,
                                           struct wl_resource *toplevel)
{
    test_cosmic_toplevel *state = cosmic_state(toplevel);
    (void)client;
    (void)resource;
    cosmic_send_state(toplevel, UINT32_MAX);
    zcosmic_toplevel_info_v1_send_done(state->info);
}

static void cosmic_manager_set_minimized(struct wl_client *client,
                                         struct wl_resource *resource,
                                         struct wl_resource *toplevel)
{
    test_cosmic_toplevel *state = cosmic_state(toplevel);
    (void)client;
    (void)resource;
    cosmic_send_state(toplevel,
        ZCOSMIC_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED);
    zcosmic_toplevel_info_v1_send_done(state->info);
}

static void cosmic_manager_unset_minimized(struct wl_client *client,
                                           struct wl_resource *resource,
                                           struct wl_resource *toplevel)
{
    cosmic_manager_unset_maximized(client, resource, toplevel);
}

static void cosmic_manager_set_fullscreen(struct wl_client *client,
                                          struct wl_resource *resource,
                                          struct wl_resource *toplevel,
                                          struct wl_resource *output)
{
    (void)client;
    (void)resource;
    (void)toplevel;
    (void)output;
}

static void cosmic_manager_unset_fullscreen(struct wl_client *client,
                                            struct wl_resource *resource,
                                            struct wl_resource *toplevel)
{
    (void)client;
    (void)resource;
    (void)toplevel;
}

static void cosmic_manager_set_rectangle(
    struct wl_client *client, struct wl_resource *resource,
    struct wl_resource *toplevel, struct wl_resource *surface,
    int32_t x, int32_t y, int32_t width, int32_t height)
{
    (void)client;
    (void)resource;
    (void)toplevel;
    (void)surface;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

static const struct zcosmic_toplevel_manager_v1_interface cosmic_manager_impl = {
    .destroy = cosmic_manager_destroy,
    .close = cosmic_manager_close,
    .activate = cosmic_manager_activate,
    .set_maximized = cosmic_manager_set_maximized,
    .unset_maximized = cosmic_manager_unset_maximized,
    .set_minimized = cosmic_manager_set_minimized,
    .unset_minimized = cosmic_manager_unset_minimized,
    .set_fullscreen = cosmic_manager_set_fullscreen,
    .unset_fullscreen = cosmic_manager_unset_fullscreen,
    .set_rectangle = cosmic_manager_set_rectangle,
};

static void bind_cosmic_manager(struct wl_client *client, void *data,
                                uint32_t version, uint32_t id)
{
    struct wl_resource *resource = wl_resource_create(
        client, &zcosmic_toplevel_manager_v1_interface,
        version < 1u ? (int)version : 1, id);
    struct wl_array capabilities;
    uint32_t values[] = {
        ZCOSMIC_TOPLEVEL_MANAGER_V1_CAPABILITY_CLOSE,
        ZCOSMIC_TOPLEVEL_MANAGER_V1_CAPABILITY_ACTIVATE,
        ZCOSMIC_TOPLEVEL_MANAGER_V1_CAPABILITY_MAXIMIZE,
        ZCOSMIC_TOPLEVEL_MANAGER_V1_CAPABILITY_MINIMIZE,
    };

    (void)data;
    assert(resource != NULL);
    wl_resource_set_implementation(resource, &cosmic_manager_impl, NULL,
                                   NULL);
    wl_array_init(&capabilities);
    void *slots = wl_array_add(&capabilities, sizeof(values));
    assert(slots != NULL);
    memcpy(slots, values, sizeof(values));
    zcosmic_toplevel_manager_v1_send_capabilities(resource, &capabilities);
    wl_array_release(&capabilities);
}

static void bind_seat(struct wl_client *client, void *data, uint32_t version,
                      uint32_t id)
{
    struct wl_resource *resource = wl_resource_create(client,
        &wl_seat_interface, (int)version, id);

    (void)data;
    if (resource == NULL)
        return;
    wl_resource_set_implementation(resource, &seat_impl, NULL, NULL);
    const char *mode = getenv("KSD_TEST_WL_MODE");
    wl_seat_send_capabilities(resource,
        mode != NULL && strncmp(mode, "keyboard", 8u) == 0
            ? WL_SEAT_CAPABILITY_KEYBOARD : 0u);
}

/* ------------------------------------------ wlroots foreign toplevels -- */

static void wlr_send_state(struct wl_resource *resource, uint32_t state)
{
    struct wl_array states;

    wl_array_init(&states);
    if (state < 32u) {
        uint32_t *slot = wl_array_add(&states, sizeof(*slot));
        if (slot != NULL)
            *slot = state;
    }
    zwlr_foreign_toplevel_handle_v1_send_state(resource, &states);
    zwlr_foreign_toplevel_handle_v1_send_done(resource);
    wl_array_release(&states);
}

static void wlr_set_maximized(struct wl_client *client,
                              struct wl_resource *resource)
{
    (void)client;
    wlr_send_state(resource,
        ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED);
}

static void wlr_unset_maximized(struct wl_client *client,
                                struct wl_resource *resource)
{
    (void)client;
    wlr_send_state(resource, UINT32_MAX);
}

static void wlr_set_minimized(struct wl_client *client,
                              struct wl_resource *resource)
{
    (void)client;
    wlr_send_state(resource,
        ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED);
}

static void wlr_unset_minimized(struct wl_client *client,
                                struct wl_resource *resource)
{
    (void)client;
    wlr_send_state(resource, UINT32_MAX);
}

static void wlr_activate(struct wl_client *client,
                         struct wl_resource *resource,
                         struct wl_resource *seat)
{
    (void)client;
    (void)seat;
    wlr_send_state(resource,
        ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED);
}

static void wlr_close(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    zwlr_foreign_toplevel_handle_v1_send_closed(resource);
}

static void wlr_set_rectangle(struct wl_client *client,
                              struct wl_resource *resource,
                              struct wl_resource *surface, int32_t x,
                              int32_t y, int32_t width, int32_t height)
{
    (void)client;
    (void)resource;
    (void)surface;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

static void wlr_destroy(struct wl_client *client,
                        struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static void wlr_set_fullscreen(struct wl_client *client,
                               struct wl_resource *resource,
                               struct wl_resource *output)
{
    (void)client;
    (void)resource;
    (void)output;
}

static void wlr_unset_fullscreen(struct wl_client *client,
                                 struct wl_resource *resource)
{
    (void)client;
    (void)resource;
}

static const struct zwlr_foreign_toplevel_handle_v1_interface wlr_handle_impl = {
    .set_maximized = wlr_set_maximized,
    .unset_maximized = wlr_unset_maximized,
    .set_minimized = wlr_set_minimized,
    .unset_minimized = wlr_unset_minimized,
    .activate = wlr_activate,
    .close = wlr_close,
    .set_rectangle = wlr_set_rectangle,
    .destroy = wlr_destroy,
    .set_fullscreen = wlr_set_fullscreen,
    .unset_fullscreen = wlr_unset_fullscreen,
};

static void send_wlr_toplevel(struct wl_client *client,
                              struct wl_resource *manager,
                              const char *title, const char *app_id,
                              uint32_t state)
{
    struct wl_resource *handle = wl_resource_create(client,
        &zwlr_foreign_toplevel_handle_v1_interface, 3, 0);

    assert(handle != NULL);
    wl_resource_set_implementation(handle, &wlr_handle_impl, NULL, NULL);
    zwlr_foreign_toplevel_manager_v1_send_toplevel(manager, handle);
    zwlr_foreign_toplevel_handle_v1_send_title(handle, title);
    zwlr_foreign_toplevel_handle_v1_send_app_id(handle, app_id);
    wlr_send_state(handle, state);
}

static void wlr_stop(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    (void)resource;
}

static const struct zwlr_foreign_toplevel_manager_v1_interface wlr_manager_impl = {
    .stop = wlr_stop,
};

static void bind_wlr_manager(struct wl_client *client, void *data,
                             uint32_t version, uint32_t id)
{
    uint32_t supported = version < 3u ? version : 3u;
    struct wl_resource *resource = wl_resource_create(client,
        &zwlr_foreign_toplevel_manager_v1_interface, (int)supported, id);

    (void)data;
    assert(resource != NULL);
    wl_resource_set_implementation(resource, &wlr_manager_impl, NULL, NULL);
    send_wlr_toplevel(client, resource, "wlr active", "org.test.Active",
        ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED);
    send_wlr_toplevel(client, resource, "wlr second", "org.test.Second",
                      UINT32_MAX);
}

/* ------------------------------------------------------- screencopy -- */

static void output_release(struct wl_client *client,
                           struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static const struct wl_output_interface output_impl = {
    .release = output_release,
};

static void bind_output(struct wl_client *client, void *data,
                        uint32_t version, uint32_t id)
{
    uint32_t supported = version < 4u ? version : 4u;
    struct wl_resource *resource = wl_resource_create(
        client, &wl_output_interface, (int)supported, id);

    (void)data;
    assert(resource != NULL);
    cosmic_output_resource = resource;
    wl_resource_set_implementation(resource, &output_impl, NULL, NULL);
    wl_output_send_geometry(resource, 0, 0, 300, 200,
                            WL_OUTPUT_SUBPIXEL_UNKNOWN, "test", "output",
                            WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(resource, WL_OUTPUT_MODE_CURRENT, 8, 4, 60000);
    if (supported >= 2u) {
        wl_output_send_scale(resource, 2);
        wl_output_send_done(resource);
    }
    if (supported >= 4u) {
        wl_output_send_name(resource, "TEST-1");
        wl_output_send_description(resource, "test output");
    }
}

static void xdg_output_destroy(struct wl_client *client,
                               struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static const struct zxdg_output_v1_interface xdg_output_impl = {
    .destroy = xdg_output_destroy,
};

static void xdg_manager_destroy(struct wl_client *client,
                                struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static void xdg_manager_get_output(struct wl_client *client,
                                   struct wl_resource *manager, uint32_t id,
                                   struct wl_resource *output)
{
    uint32_t version = (uint32_t)wl_resource_get_version(manager);
    struct wl_resource *resource = wl_resource_create(
        client, &zxdg_output_v1_interface, (int)version, id);

    (void)output;
    assert(resource != NULL);
    wl_resource_set_implementation(resource, &xdg_output_impl, NULL, NULL);
    zxdg_output_v1_send_logical_position(resource, 0, 0);
    zxdg_output_v1_send_logical_size(resource, 4, 2);
    zxdg_output_v1_send_done(resource);
    if (version >= 2u) {
        zxdg_output_v1_send_name(resource, "TEST-1");
        zxdg_output_v1_send_description(resource, "test output");
    }
}

static const struct zxdg_output_manager_v1_interface xdg_manager_impl = {
    .destroy = xdg_manager_destroy,
    .get_xdg_output = xdg_manager_get_output,
};

static void bind_xdg_manager(struct wl_client *client, void *data,
                             uint32_t version, uint32_t id)
{
    uint32_t supported = version < 3u ? version : 3u;
    struct wl_resource *resource = wl_resource_create(
        client, &zxdg_output_manager_v1_interface, (int)supported, id);

    (void)data;
    assert(resource != NULL);
    wl_resource_set_implementation(resource, &xdg_manager_impl, NULL, NULL);
}

typedef struct test_frame {
    uint32_t width;
    uint32_t height;
} test_frame;

static void destroy_test_frame(struct wl_resource *resource)
{
    free(wl_resource_get_user_data(resource));
}

static void screencopy_frame_copy(struct wl_client *client,
                                  struct wl_resource *resource,
                                  struct wl_resource *buffer_resource)
{
    test_frame *frame = wl_resource_get_user_data(resource);
    struct wl_shm_buffer *buffer = wl_shm_buffer_get(buffer_resource);

    (void)client;
    assert(frame != NULL && buffer != NULL);
    assert((uint32_t)wl_shm_buffer_get_width(buffer) == frame->width);
    assert((uint32_t)wl_shm_buffer_get_height(buffer) == frame->height);
    assert(wl_shm_buffer_get_format(buffer) == WL_SHM_FORMAT_XRGB8888);
    wl_shm_buffer_begin_access(buffer);
    uint8_t *pixels = wl_shm_buffer_get_data(buffer);
    int stride = wl_shm_buffer_get_stride(buffer);
    for (uint32_t row = 0u; row < frame->height; row++)
        for (uint32_t column = 0u; column < frame->width; column++) {
            uint8_t *pixel = pixels + (size_t)row * (size_t)stride
                + (size_t)column * 4u;
            pixel[0] = 0x11u;
            pixel[1] = 0x22u;
            pixel[2] = 0x33u;
            pixel[3] = 0u;
        }
    wl_shm_buffer_end_access(buffer);
    zwlr_screencopy_frame_v1_send_ready(resource, 0u, 0u, 0u);
}

static void screencopy_frame_destroy(struct wl_client *client,
                                     struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static const struct zwlr_screencopy_frame_v1_interface screencopy_frame_impl = {
    .copy = screencopy_frame_copy,
    .destroy = screencopy_frame_destroy,
    .copy_with_damage = screencopy_frame_copy,
};

static void send_screencopy_frame(struct wl_client *client,
                                  struct wl_resource *manager,
                                  uint32_t id, int32_t width, int32_t height)
{
    uint32_t version = (uint32_t)wl_resource_get_version(manager);
    struct wl_resource *resource;
    test_frame *frame;

    assert(width > 0 && height > 0);
    resource = wl_resource_create(client, &zwlr_screencopy_frame_v1_interface,
                                  (int)version, id);
    frame = calloc(1u, sizeof(*frame));
    assert(resource != NULL && frame != NULL);
    frame->width = (uint32_t)width * 2u;
    frame->height = (uint32_t)height * 2u;
    wl_resource_set_implementation(resource, &screencopy_frame_impl, frame,
                                   destroy_test_frame);
    zwlr_screencopy_frame_v1_send_buffer(resource, WL_SHM_FORMAT_XRGB8888,
                                         frame->width, frame->height,
                                         frame->width * 4u);
    if (version >= 3u)
        zwlr_screencopy_frame_v1_send_buffer_done(resource);
}

static void screencopy_output(struct wl_client *client,
                              struct wl_resource *manager, uint32_t id,
                              int32_t overlay_cursor,
                              struct wl_resource *output)
{
    (void)overlay_cursor;
    (void)output;
    send_screencopy_frame(client, manager, id, 4, 2);
}

static void screencopy_region(struct wl_client *client,
                              struct wl_resource *manager, uint32_t id,
                              int32_t overlay_cursor,
                              struct wl_resource *output, int32_t x,
                              int32_t y, int32_t width, int32_t height)
{
    (void)overlay_cursor;
    (void)output;
    (void)x;
    (void)y;
    send_screencopy_frame(client, manager, id, width, height);
}

static void screencopy_manager_destroy(struct wl_client *client,
                                       struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static const struct zwlr_screencopy_manager_v1_interface screencopy_impl = {
    .capture_output = screencopy_output,
    .capture_output_region = screencopy_region,
    .destroy = screencopy_manager_destroy,
};

static void bind_screencopy(struct wl_client *client, void *data,
                            uint32_t version, uint32_t id)
{
    uint32_t supported = version < 3u ? version : 3u;
    struct wl_resource *resource = wl_resource_create(
        client, &zwlr_screencopy_manager_v1_interface, (int)supported, id);

    (void)data;
    assert(resource != NULL);
    wl_resource_set_implementation(resource, &screencopy_impl, NULL, NULL);
}

/* ------------------------------------------------------ image copy -- */

static void image_source_destroy(struct wl_client *client,
                                 struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static const struct ext_image_capture_source_v1_interface image_source_impl = {
    .destroy = image_source_destroy,
};

static void output_source_create(struct wl_client *client,
                                 struct wl_resource *manager, uint32_t id,
                                 struct wl_resource *output)
{
    struct wl_resource *resource = wl_resource_create(
        client, &ext_image_capture_source_v1_interface, 1, id);

    (void)manager;
    assert(resource != NULL && output != NULL);
    wl_resource_set_implementation(resource, &image_source_impl, output,
                                   NULL);
}

static void output_source_destroy(struct wl_client *client,
                                  struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static const struct ext_output_image_capture_source_manager_v1_interface
output_source_impl = {
    .create_source = output_source_create,
    .destroy = output_source_destroy,
};

static void bind_output_source(struct wl_client *client, void *data,
                               uint32_t version, uint32_t id)
{
    struct wl_resource *resource = wl_resource_create(
        client, &ext_output_image_capture_source_manager_v1_interface,
        version < 1u ? (int)version : 1, id);

    (void)data;
    assert(resource != NULL);
    wl_resource_set_implementation(resource, &output_source_impl, NULL,
                                   NULL);
}

typedef struct test_image_frame {
    struct wl_resource *buffer;
} test_image_frame;

static void destroy_test_image_frame(struct wl_resource *resource)
{
    free(wl_resource_get_user_data(resource));
}

static void image_frame_destroy(struct wl_client *client,
                                struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static void image_frame_attach(struct wl_client *client,
                               struct wl_resource *resource,
                               struct wl_resource *buffer)
{
    test_image_frame *frame = wl_resource_get_user_data(resource);

    (void)client;
    assert(frame != NULL && buffer != NULL);
    frame->buffer = buffer;
}

static void image_frame_damage(struct wl_client *client,
                               struct wl_resource *resource, int32_t x,
                               int32_t y, int32_t width, int32_t height)
{
    (void)client;
    (void)resource;
    assert(x == 0 && y == 0 && width == 8 && height == 4);
}

static void image_frame_capture(struct wl_client *client,
                                struct wl_resource *resource)
{
    test_image_frame *frame = wl_resource_get_user_data(resource);
    struct wl_shm_buffer *buffer;
    uint8_t *pixels;
    int stride;

    (void)client;
    assert(frame != NULL && frame->buffer != NULL);
    buffer = wl_shm_buffer_get(frame->buffer);
    assert(buffer != NULL);
    assert(wl_shm_buffer_get_width(buffer) == 8);
    assert(wl_shm_buffer_get_height(buffer) == 4);
    assert(wl_shm_buffer_get_format(buffer) == WL_SHM_FORMAT_ABGR8888);
    wl_shm_buffer_begin_access(buffer);
    pixels = wl_shm_buffer_get_data(buffer);
    stride = wl_shm_buffer_get_stride(buffer);
    for (int row = 0; row < 4; row++)
        for (int column = 0; column < 8; column++) {
            uint8_t *pixel = pixels + (size_t)row * (size_t)stride
                + (size_t)column * 4u;
            pixel[0] = 0x33u;
            pixel[1] = 0x22u;
            pixel[2] = 0x11u;
            pixel[3] = 0x7fu;
        }
    wl_shm_buffer_end_access(buffer);
    ext_image_copy_capture_frame_v1_send_transform(
        resource, WL_OUTPUT_TRANSFORM_NORMAL);
    ext_image_copy_capture_frame_v1_send_ready(resource);
}

static const struct ext_image_copy_capture_frame_v1_interface
image_copy_frame_impl = {
    .destroy = image_frame_destroy,
    .attach_buffer = image_frame_attach,
    .damage_buffer = image_frame_damage,
    .capture = image_frame_capture,
};

static void image_session_create_frame(struct wl_client *client,
                                       struct wl_resource *session,
                                       uint32_t id)
{
    struct wl_resource *resource = wl_resource_create(
        client, &ext_image_copy_capture_frame_v1_interface, 1, id);
    test_image_frame *frame = calloc(1u, sizeof(*frame));

    (void)session;
    assert(resource != NULL && frame != NULL);
    wl_resource_set_implementation(resource, &image_copy_frame_impl, frame,
                                   destroy_test_image_frame);
}

static void image_session_destroy(struct wl_client *client,
                                  struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static const struct ext_image_copy_capture_session_v1_interface
image_copy_session_impl = {
    .create_frame = image_session_create_frame,
    .destroy = image_session_destroy,
};

static void image_copy_create_session(struct wl_client *client,
                                      struct wl_resource *manager,
                                      uint32_t id,
                                      struct wl_resource *source,
                                      uint32_t options)
{
    struct wl_resource *resource = wl_resource_create(
        client, &ext_image_copy_capture_session_v1_interface, 1, id);

    (void)manager;
    assert(resource != NULL && source != NULL && options == 0u);
    wl_resource_set_implementation(resource, &image_copy_session_impl, NULL,
                                   NULL);
    ext_image_copy_capture_session_v1_send_buffer_size(resource, 8u, 4u);
    ext_image_copy_capture_session_v1_send_shm_format(
        resource, WL_SHM_FORMAT_ABGR8888);
    ext_image_copy_capture_session_v1_send_done(resource);
}

static void image_copy_create_cursor_session(
    struct wl_client *client, struct wl_resource *manager, uint32_t id,
    struct wl_resource *source, struct wl_resource *pointer)
{
    (void)client;
    (void)manager;
    (void)id;
    (void)source;
    (void)pointer;
    assert(false);
}

static void image_copy_destroy(struct wl_client *client,
                               struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static const struct ext_image_copy_capture_manager_v1_interface
image_copy_impl = {
    .create_session = image_copy_create_session,
    .create_pointer_cursor_session = image_copy_create_cursor_session,
    .destroy = image_copy_destroy,
};

static void bind_image_copy(struct wl_client *client, void *data,
                            uint32_t version, uint32_t id)
{
    struct wl_resource *resource = wl_resource_create(
        client, &ext_image_copy_capture_manager_v1_interface,
        version < 1u ? (int)version : 1, id);

    (void)data;
    assert(resource != NULL);
    wl_resource_set_implementation(resource, &image_copy_impl, NULL, NULL);
}

static bool pointer_motion_seen;

static void pointer_motion(struct wl_client *client,
                           struct wl_resource *resource, uint32_t time,
                           wl_fixed_t dx, wl_fixed_t dy)
{
    (void)client;
    (void)resource;
    (void)time;
    (void)dx;
    (void)dy;
}

static void pointer_motion_absolute(struct wl_client *client,
                                    struct wl_resource *resource,
                                    uint32_t time, uint32_t x, uint32_t y,
                                    uint32_t x_extent, uint32_t y_extent)
{
    (void)client;
    (void)resource;
    (void)time;
    assert(x == 3u && y == 1u && x_extent == 4u && y_extent == 2u);
    pointer_motion_seen = true;
}

static void pointer_button(struct wl_client *client,
                           struct wl_resource *resource, uint32_t time,
                           uint32_t button, uint32_t state)
{
    (void)client;
    (void)resource;
    (void)time;
    (void)button;
    (void)state;
}

static void pointer_axis(struct wl_client *client,
                         struct wl_resource *resource, uint32_t time,
                         uint32_t axis, wl_fixed_t value)
{
    (void)client;
    (void)resource;
    (void)time;
    (void)axis;
    (void)value;
}

static void pointer_frame(struct wl_client *client,
                          struct wl_resource *resource)
{
    (void)client;
    (void)resource;
    assert(pointer_motion_seen);
}

static void pointer_axis_source(struct wl_client *client,
                                struct wl_resource *resource,
                                uint32_t source)
{
    (void)client;
    (void)resource;
    (void)source;
}

static void pointer_axis_stop(struct wl_client *client,
                              struct wl_resource *resource, uint32_t time,
                              uint32_t axis)
{
    (void)client;
    (void)resource;
    (void)time;
    (void)axis;
}

static void pointer_axis_discrete(struct wl_client *client,
                                  struct wl_resource *resource,
                                  uint32_t time, uint32_t axis,
                                  wl_fixed_t value, int32_t discrete)
{
    (void)client;
    (void)resource;
    (void)time;
    (void)axis;
    (void)value;
    (void)discrete;
}

static void pointer_destroy(struct wl_client *client,
                            struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static const struct zwlr_virtual_pointer_v1_interface pointer_impl = {
    .motion = pointer_motion,
    .motion_absolute = pointer_motion_absolute,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
    .destroy = pointer_destroy,
};

static void pointer_manager_create(struct wl_client *client,
                                   struct wl_resource *manager,
                                   struct wl_resource *seat, uint32_t id)
{
    uint32_t version = (uint32_t)wl_resource_get_version(manager);
    struct wl_resource *resource = wl_resource_create(
        client, &zwlr_virtual_pointer_v1_interface, (int)version, id);

    (void)seat;
    assert(resource != NULL);
    pointer_motion_seen = false;
    wl_resource_set_implementation(resource, &pointer_impl, NULL, NULL);
}

static void pointer_manager_destroy(struct wl_client *client,
                                    struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static void pointer_manager_create_for_output(
    struct wl_client *client, struct wl_resource *manager,
    struct wl_resource *seat, struct wl_resource *output, uint32_t id)
{
    (void)output;
    pointer_manager_create(client, manager, seat, id);
}

static const struct zwlr_virtual_pointer_manager_v1_interface pointer_manager_impl = {
    .create_virtual_pointer = pointer_manager_create,
    .destroy = pointer_manager_destroy,
    .create_virtual_pointer_with_output = pointer_manager_create_for_output,
};

static void bind_pointer_manager(struct wl_client *client, void *data,
                                 uint32_t version, uint32_t id)
{
    uint32_t supported = version < 2u ? version : 2u;
    struct wl_resource *resource = wl_resource_create(
        client, &zwlr_virtual_pointer_manager_v1_interface, (int)supported,
        id);

    (void)data;
    assert(resource != NULL);
    wl_resource_set_implementation(resource, &pointer_manager_impl, NULL,
                                   NULL);
}

/* Runs the compositor. Never returns. */
static void run_server(const char *socket_name, int ready)
{
    struct wl_display *display = wl_display_create();
    const char *mode = getenv("KSD_TEST_WL_MODE");

    /* Dies with the parent. Without this an aborted assertion leaves a server
     * holding the test's stdout pipe, and ctest waits for EOF on it rather
     * than reporting the failure -- the exact defect the X11 tests hit. */
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() == 1)
        _exit(0);
    if (display == NULL)
        _exit(1);
    if (wl_display_add_socket(display, socket_name) != 0)
        _exit(1);
    if (wl_global_create(display, &ext_data_control_manager_v1_interface, 1,
                         NULL, bind_manager) == NULL)
        _exit(1);
    if (wl_global_create(display, &wl_seat_interface, 1, NULL, bind_seat)
        == NULL)
        _exit(1);
    if (mode != NULL && strcmp(mode, "cosmic") == 0) {
        if (wl_global_create(display, &wl_output_interface, 4, NULL,
                             bind_output) == NULL
            || wl_global_create(display,
                    &zxdg_output_manager_v1_interface, 3, NULL,
                    bind_xdg_manager) == NULL
            || wl_global_create(display,
                    &ext_foreign_toplevel_list_v1_interface, 1, NULL,
                    bind_list) == NULL
            || wl_global_create(display,
                    &zcosmic_toplevel_info_v1_interface, 2, NULL,
                    bind_cosmic_info) == NULL
            || wl_global_create(display,
                    &zcosmic_toplevel_manager_v1_interface, 1, NULL,
                    bind_cosmic_manager) == NULL)
            _exit(1);
    } else if (mode != NULL && strcmp(mode, "capture") == 0) {
        if (wl_display_init_shm(display) != 0
            || wl_global_create(display, &wl_output_interface, 4, NULL,
                                bind_output) == NULL
            || wl_global_create(display,
                    &zxdg_output_manager_v1_interface, 3, NULL,
                    bind_xdg_manager) == NULL
            || wl_global_create(display,
                    &zwlr_screencopy_manager_v1_interface, 3, NULL,
                    bind_screencopy) == NULL
            || wl_global_create(display,
                    &zwlr_virtual_pointer_manager_v1_interface, 2, NULL,
                    bind_pointer_manager) == NULL)
            _exit(1);
    } else if (mode != NULL && strcmp(mode, "image-copy") == 0) {
        if (wl_display_init_shm(display) != 0
            || wl_display_add_shm_format(display,
                                         WL_SHM_FORMAT_ABGR8888) == NULL
            || wl_global_create(display, &wl_output_interface, 4, NULL,
                                bind_output) == NULL
            || wl_global_create(display,
                    &zxdg_output_manager_v1_interface, 3, NULL,
                    bind_xdg_manager) == NULL
            || wl_global_create(display,
                    &ext_output_image_capture_source_manager_v1_interface,
                    1, NULL, bind_output_source) == NULL
            || wl_global_create(display,
                    &ext_image_copy_capture_manager_v1_interface, 1, NULL,
                    bind_image_copy) == NULL)
            _exit(1);
    } else if (mode != NULL && strcmp(mode, "wlr") == 0) {
        if (wl_global_create(display,
                &zwlr_foreign_toplevel_manager_v1_interface, 3, NULL,
                bind_wlr_manager) == NULL)
            _exit(1);
    } else if (wl_global_create(display,
                   &ext_foreign_toplevel_list_v1_interface, 1, NULL,
                   bind_list) == NULL) {
        _exit(1);
    }
    /* The parent waits on this rather than sleeping: the socket exists only
     * once add_socket has returned, and connecting before that fails. */
    if (write(ready, "1", 1u) != 1)
        _exit(1);
    close(ready);
    wl_display_run(display);
    _exit(0);
}

/* ---------------------------------------------------------------- client -- */

static pid_t start_server(const char *socket_name, const char *mode)
{
    int pair[2];
    char byte = 0;
    pid_t child;

    assert(pipe(pair) == 0);
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(pair[0]);
        setenv("KSD_TEST_WL_MODE", mode, 1);
        run_server(socket_name, pair[1]);
    }
    close(pair[1]);
    assert(read(pair[0], &byte, 1u) == 1);
    close(pair[0]);
    return child;
}

static void stop_server(pid_t child)
{
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
}

static uint32_t framed_length(const ksd_operation_result *result)
{
    assert(result->tail != NULL && result->tail_length >= 4u);
    return ksd_decode_u32(result->tail);
}

static bool lists_type(const ksd_operation_result *result, const char *wanted)
{
    const uint8_t *tail = result->tail;
    uint32_t count;
    size_t offset = 8u;

    assert(result->tail_length >= 8u);
    count = ksd_decode_u32(tail);
    assert(ksd_decode_u32(tail + 4u) == 0u);
    for (uint32_t index = 0u; index < count; index++) {
        uint32_t length;

        assert(offset + 4u <= result->tail_length);
        length = ksd_decode_u32(tail + offset);
        offset += 4u;
        assert(offset + length <= result->tail_length);
        if (strlen(wanted) == length
            && memcmp(tail + offset, wanted, length) == 0)
            return true;
        offset += length;
    }
    assert(offset == result->tail_length);
    return false;
}

static void check_normal(const char *socket_name)
{
    ksd_wayland *connection = NULL;
    ksd_operation_result result;
    pid_t child = start_server(socket_name, "normal");

    assert(ksd_wayland_open(socket_name, &connection) == KSD_STATUS_OK);
    /* The compositor advertised the protocol, so the backend reports it. A
     * compositor that did not would be reported as not supporting it rather
     * than failing when a verb is called. */
    assert(ksd_wayland_supported(connection).data_control);

    ksd_result_init(&result);
    ksd_wayland_clipboard_mimetypes(connection, &result);
    assert(result.status == KSD_STATUS_OK);
    assert(lists_type(&result, TEXT_MIME));
    assert(lists_type(&result, CSV_MIME));
    ksd_result_clear(&result);

    /* Text comes back byte for byte, including the non-ASCII: a transfer that
     * mangled encoding would show here and not on plain ASCII. */
    ksd_result_init(&result);
    ksd_wayland_clipboard_text(connection, &result);
    assert(result.status == KSD_STATUS_OK);
    assert(framed_length(&result) == strlen(CLIP_TEXT));
    assert(memcmp(result.tail + 4u, CLIP_TEXT, strlen(CLIP_TEXT)) == 0);
    ksd_result_clear(&result);

    ksd_result_init(&result);
    ksd_wayland_clipboard_content(connection, (const uint8_t *)CSV_MIME,
                                  (uint32_t)strlen(CSV_MIME), &result);
    assert(result.status == KSD_STATUS_OK);
    assert(framed_length(&result) == strlen(CLIP_CSV));
    assert(memcmp(result.tail + 4u, CLIP_CSV, strlen(CLIP_CSV)) == 0);
    ksd_result_clear(&result);

    /* A format the owner never offered is refused rather than asked for. The
     * protocol answers an unoffered type with an empty transfer, which cannot
     * be told apart from an empty clipboard entry; refusing says which. */
    ksd_result_init(&result);
    ksd_wayland_clipboard_content(connection, (const uint8_t *)"image/png",
                                  9u, &result);
    assert(result.status == KSD_STATUS_UNSUPPORTED);
    ksd_result_clear(&result);

    ksd_wayland_close(connection);
    stop_server(child);
}

/* The list carries what this protocol actually reports and nothing more. */
static void check_window_list(const char *socket_name)
{
    ksd_wayland *connection = NULL;
    ksd_operation_result result;
    pid_t child = start_server(socket_name, "normal");
    const char *body;
    uint32_t length;

    assert(ksd_wayland_open(socket_name, &connection) == KSD_STATUS_OK);
    assert(ksd_wayland_supported(connection).toplevel_list);

    ksd_result_init(&result);
    ksd_wayland_window_list(connection, true, &result);
    assert(result.status == KSD_STATUS_OK);
    length = ksd_decode_u32(result.tail);
    body = (const char *)result.tail + 4u;
    assert(length == result.tail_length - 4u);

    /* Both windows, with the fields the protocol carries. */
    assert(memmem(body, length, "\"compositorId\":\"id-one\"", 23u) != NULL);
    assert(memmem(body, length, "\"compositorId\":\"id-two\"", 23u) != NULL);
    assert(memmem(body, length, "org.test.One", 12u) != NULL);
    assert(memmem(body, length, "\"appId\":\"org.test.Two\"", 22u) != NULL);

    /* A quote inside a title is escaped. A window title is arbitrary text
     * chosen by another application, and it is being pasted into a document
     * the caller will parse. */
    assert(memmem(body, length, "\\\"quoted\\\"", 10u) != NULL);

    /* And what this protocol cannot report is ABSENT, not zero. A zero would
     * be read as a fact: a window at the origin with no size, owned by process
     * zero. The consumer already treats an absent field as unknown. */
    assert(memmem(body, length, "\"pid\"", 5u) == NULL);
    assert(memmem(body, length, "\"frame\"", 7u) == NULL);
    assert(memmem(body, length, "\"client\"", 8u) == NULL);
    assert(memmem(body, length, "\"minimized\"", 11u) == NULL);
    ksd_result_clear(&result);

    ksd_wayland_close(connection);
    stop_server(child);
}

static uint64_t window_handle_at(const ksd_operation_result *result,
                                  unsigned index)
{
    const char *cursor = (const char *)result->tail + 4u;
    size_t length = ksd_decode_u32(result->tail);
    const char *end = cursor + length;
    uint64_t handle = 0u;
    for (unsigned current = 0u; current <= index; current++) {
        cursor = memmem(cursor, (size_t)(end - cursor), "\"id\":\"", 6u);
        assert(cursor != NULL);
        cursor += 6u;
    }
    while (cursor < end && *cursor >= '0' && *cursor <= '9')
        handle = handle * 10u + (uint64_t)(*cursor++ - '0');
    assert(cursor < end && *cursor == '"' && handle != 0u);
    return handle;
}

static void check_window_query(const char *socket_name, const char *mode)
{
    ksd_wayland *connection = NULL;
    ksd_operation_result result;
    pid_t child = start_server(socket_name, mode);
    assert(ksd_wayland_open(socket_name, &connection) == KSD_STATUS_OK);
    ksd_result_init(&result);
    ksd_wayland_window_list(connection, true, &result);
    assert(result.status == KSD_STATUS_OK);
    uint64_t first = window_handle_at(&result, 0u);
    uint64_t second = window_handle_at(&result, 1u);
    assert(first != second);
    ksd_result_clear(&result);
    ksd_wayland_window_query(connection, second, &result);
    assert(result.status == KSD_STATUS_OK);
    assert(window_handle_at(&result, 0u) == second);
    assert(memmem(result.tail + 4u, framed_length(&result),
                   "\"windows\":", 10u) == NULL);
    ksd_result_clear(&result);
    ksd_wayland_window_query(connection, UINT64_MAX, &result);
    assert(result.status == KSD_STATUS_NOT_FOUND);
    ksd_result_clear(&result);
    ksd_wayland_close(connection);
    assert(ksd_wayland_open(socket_name, &connection) == KSD_STATUS_OK);
    ksd_wayland_window_query(connection, second, &result);
    assert(result.status == KSD_STATUS_NOT_FOUND);
    ksd_result_clear(&result);
    ksd_wayland_close(connection);
    stop_server(child);
}

static void check_wlr_windows(const char *socket_name)
{
    ksd_wayland *connection = NULL;
    ksd_operation_result result;
    pid_t child = start_server(socket_name, "wlr");
    const char *body;
    uint32_t length;

    assert(ksd_wayland_open(socket_name, &connection) == KSD_STATUS_OK);
    assert(ksd_wayland_supported(connection).toplevel_control);

    ksd_result_init(&result);
    ksd_wayland_window_list(connection, true, &result);
    assert(result.status == KSD_STATUS_OK);
    length = ksd_decode_u32(result.tail);
    body = (const char *)result.tail + 4u;
    assert(memmem(body, length, "\"title\":\"wlr active\"", 20u) != NULL);
    assert(memmem(body, length, "\"active\":true", 13u) != NULL);
    uint64_t second = window_handle_at(&result, 1u);
    ksd_result_clear(&result);

    ksd_result_init(&result);
    ksd_wayland_active_window(connection, &result);
    assert(result.status == KSD_STATUS_OK);
    length = ksd_decode_u32(result.tail);
    body = (const char *)result.tail + 4u;
    assert(memmem(body, length, "\"title\":\"wlr active\"", 20u) != NULL);
    ksd_result_clear(&result);

    ksd_result_init(&result);
    ksd_wayland_window_action(connection, KSD_OP_WINDOW_SET_STATE, second, 2u,
                              &result);
    assert(result.status == KSD_STATUS_OK);
    ksd_result_clear(&result);

    ksd_result_init(&result);
    ksd_wayland_window_list(connection, true, &result);
    assert(result.status == KSD_STATUS_OK);
    length = ksd_decode_u32(result.tail);
    body = (const char *)result.tail + 4u;
    assert(memmem(body, length, "\"maximized\":true", 16u) != NULL);
    ksd_result_clear(&result);

    ksd_result_init(&result);
    ksd_wayland_window_action(connection, KSD_OP_WINDOW_CLOSE, second, 0u,
                              &result);
    assert(result.status == KSD_STATUS_OK);
    ksd_result_clear(&result);

    ksd_result_init(&result);
    ksd_wayland_window_list(connection, true, &result);
    assert(result.status == KSD_STATUS_OK);
    length = ksd_decode_u32(result.tail);
    body = (const char *)result.tail + 4u;
    assert(memmem(body, length, "wlr second", 10u) == NULL);
    ksd_result_clear(&result);

    ksd_wayland_close(connection);
    connection = NULL;
    assert(ksd_wayland_open(socket_name, &connection) == KSD_STATUS_OK);
    ksd_result_init(&result);
    ksd_wayland_window_action(connection, KSD_OP_WINDOW_CLOSE, second, 0u,
                              &result);
    assert(result.status == KSD_STATUS_NOT_FOUND);
    ksd_result_clear(&result);
    ksd_wayland_close(connection);
    stop_server(child);
}

static void check_cosmic_windows(const char *socket_name)
{
    ksd_wayland *connection = NULL;
    ksd_operation_result result;
    pid_t child = start_server(socket_name, "cosmic");
    const char *body;
    uint32_t length;

    assert(ksd_wayland_open(socket_name, &connection) == KSD_STATUS_OK);
    assert(ksd_wayland_supported(connection).toplevel_list);
    assert(ksd_wayland_supported(connection).toplevel_active);
    assert(ksd_wayland_supported(connection).toplevel_focus);
    assert(ksd_wayland_supported(connection).toplevel_close);
    assert(ksd_wayland_supported(connection).toplevel_state);

    ksd_result_init(&result);
    ksd_wayland_window_list(connection, true, &result);
    assert(result.status == KSD_STATUS_OK);
    uint64_t second = window_handle_at(&result, 1u);
    ksd_result_clear(&result);

    ksd_result_init(&result);
    ksd_wayland_window_list(connection, false, &result);
    assert(result.status == KSD_STATUS_OK);
    length = ksd_decode_u32(result.tail);
    body = (const char *)result.tail + 4u;
    assert(memmem(body, length, "first \\\"quoted\\\" window",
                  sizeof("first \\\"quoted\\\" window") - 1u)
           != NULL);
    assert(memmem(body, length, "second", 6u) == NULL);
    assert(memmem(body, length, "\"active\":true", 13u) != NULL);
    assert(memmem(body, length,
                  "\"frame\":{\"x\":1,\"y\":0,\"width\":2,\"height\":1}",
                  sizeof("\"frame\":{\"x\":1,\"y\":0,\"width\":2,\"height\":1}")
                      - 1u) != NULL);
    ksd_result_clear(&result);

    ksd_result_init(&result);
    ksd_wayland_active_window(connection, &result);
    assert(result.status == KSD_STATUS_OK);
    length = ksd_decode_u32(result.tail);
    body = (const char *)result.tail + 4u;
    assert(memmem(body, length, "org.test.One", 12u) != NULL);
    ksd_result_clear(&result);

    ksd_result_init(&result);
    ksd_wayland_window_action(connection, KSD_OP_WINDOW_FOCUS, second, 0u,
                              &result);
    assert(result.status == KSD_STATUS_OK);
    ksd_result_clear(&result);

    ksd_result_init(&result);
    ksd_wayland_window_action(connection, KSD_OP_WINDOW_SET_STATE, second, 2u,
                              &result);
    assert(result.status == KSD_STATUS_OK);
    ksd_result_clear(&result);

    ksd_result_init(&result);
    ksd_wayland_window_list(connection, true, &result);
    assert(result.status == KSD_STATUS_OK);
    length = ksd_decode_u32(result.tail);
    body = (const char *)result.tail + 4u;
    assert(memmem(body, length, "second", 6u) != NULL);
    assert(memmem(body, length, "\"maximized\":true",
                  sizeof("\"maximized\":true") - 1u) != NULL);
    ksd_result_clear(&result);

    ksd_result_init(&result);
    ksd_wayland_window_action(connection, KSD_OP_WINDOW_CLOSE, second, 0u,
                              &result);
    assert(result.status == KSD_STATUS_OK);
    ksd_result_clear(&result);

    ksd_result_init(&result);
    ksd_wayland_window_list(connection, true, &result);
    assert(result.status == KSD_STATUS_OK);
    length = ksd_decode_u32(result.tail);
    body = (const char *)result.tail + 4u;
    assert(memmem(body, length, "second", 6u) == NULL);
    ksd_result_clear(&result);

    ksd_wayland_close(connection);
    stop_server(child);
}

static void check_screencopy(const char *socket_name)
{
    ksd_wayland *connection = NULL;
    ksd_operation_result result;
    pid_t child = start_server(socket_name, "capture");
    uint8_t capture[52];

    assert(ksd_wayland_open(socket_name, &connection) == KSD_STATUS_OK);
    assert(ksd_wayland_supported(connection).screencopy);
    ksd_result_init(&result);
    ksd_wayland_capture_area(connection, 1, 0, 2u, 1u, &result);
    assert(result.status == KSD_STATUS_OK);
    assert(result.tail == NULL);
    assert(result.payload_fd >= 0);
    assert(result.tail_length == sizeof(capture));
    assert(pread(result.payload_fd, capture, sizeof(capture), 0)
           == (ssize_t)sizeof(capture));
    assert(ksd_decode_u16(capture) == KSD_CAPTURE_FORMAT_BGRA8_PREMULTIPLIED);
    assert(ksd_decode_u32(capture + 4u) == 4u);
    assert(ksd_decode_u32(capture + 8u) == 2u);
    assert(ksd_decode_u32(capture + 12u) == 16u);
    assert(ksd_decode_u32(capture + 16u) == 32u);
    for (size_t offset = 20u; offset < sizeof(capture); offset += 4u) {
        assert(capture[offset] == 0x11u);
        assert(capture[offset + 1u] == 0x22u);
        assert(capture[offset + 2u] == 0x33u);
        assert(capture[offset + 3u] == 0xffu);
    }
    ksd_result_clear(&result);
    ksd_wayland_close(connection);
    stop_server(child);
}

static void check_image_copy(const char *socket_name)
{
    ksd_wayland *connection = NULL;
    ksd_operation_result result;
    pid_t child = start_server(socket_name, "image-copy");
    uint8_t capture[52];

    assert(ksd_wayland_open(socket_name, &connection) == KSD_STATUS_OK);
    assert(ksd_wayland_supported(connection).screencopy);
    ksd_result_init(&result);
    ksd_wayland_capture_area(connection, 1, 0, 2u, 1u, &result);
    assert(result.status == KSD_STATUS_OK);
    assert(result.tail == NULL);
    assert(result.payload_fd >= 0);
    assert(result.tail_length == sizeof(capture));
    assert(pread(result.payload_fd, capture, sizeof(capture), 0)
           == (ssize_t)sizeof(capture));
    assert(ksd_decode_u16(capture) == KSD_CAPTURE_FORMAT_BGRA8_PREMULTIPLIED);
    assert(ksd_decode_u32(capture + 4u) == 4u);
    assert(ksd_decode_u32(capture + 8u) == 2u);
    assert(ksd_decode_u32(capture + 12u) == 16u);
    assert(ksd_decode_u32(capture + 16u) == 32u);
    for (size_t offset = 20u; offset < sizeof(capture); offset += 4u) {
        assert(capture[offset] == 0x11u);
        assert(capture[offset + 1u] == 0x22u);
        assert(capture[offset + 2u] == 0x33u);
        assert(capture[offset + 3u] == 0x7fu);
    }
    ksd_result_clear(&result);
    ksd_wayland_close(connection);
    stop_server(child);
}

static void check_absolute_pointer(const char *socket_name)
{
    ksd_wayland *connection = NULL;
    ksd_operation_result result;
    pid_t child = start_server(socket_name, "capture");

    assert(ksd_wayland_open(socket_name, &connection) == KSD_STATUS_OK);
    assert(ksd_wayland_supported(connection).absolute_pointer);
    ksd_result_init(&result);
    ksd_wayland_move_absolute(connection, 3, 1, &result);
    assert(result.status == KSD_STATUS_OK);
    ksd_result_clear(&result);

    /* This round trip makes the server consume the one-way pointer request,
     * so its coordinate assertions are part of this test rather than queued
     * work killed during teardown. */
    ksd_result_init(&result);
    ksd_wayland_clipboard_text(connection, &result);
    assert(result.status == KSD_STATUS_OK);
    ksd_result_clear(&result);
    ksd_wayland_close(connection);
    stop_server(child);
}

static void check_empty(const char *socket_name)
{
    ksd_wayland *connection = NULL;
    ksd_operation_result result;
    pid_t child = start_server(socket_name, "empty");

    assert(ksd_wayland_open(socket_name, &connection) == KSD_STATUS_OK);

    /* An empty clipboard is a state, not a failure, and the answer has the
     * same shape the providers give for it. */
    ksd_result_init(&result);
    ksd_wayland_clipboard_mimetypes(connection, &result);
    assert(result.status == KSD_STATUS_OK);
    assert(ksd_decode_u32(result.tail) == 0u);
    ksd_result_clear(&result);

    ksd_result_init(&result);
    ksd_wayland_clipboard_text(connection, &result);
    assert(result.status == KSD_STATUS_OK);
    assert(framed_length(&result) == 0u);
    ksd_result_clear(&result);

    ksd_wayland_close(connection);
    stop_server(child);
}

/* A compositor that answers, but never says what the selection is. The client
 * cannot distinguish that from an empty clipboard, so it must report the same
 * thing rather than waiting for an event that may never come. */
static void check_silent(const char *socket_name)
{
    ksd_wayland *connection = NULL;
    ksd_operation_result result;
    pid_t child = start_server(socket_name, "silent");

    assert(ksd_wayland_open(socket_name, &connection) == KSD_STATUS_OK);

    ksd_result_init(&result);
    ksd_wayland_clipboard_text(connection, &result);
    assert(result.status == KSD_STATUS_OK);
    assert(framed_length(&result) == 0u);
    ksd_result_clear(&result);

    ksd_wayland_close(connection);
    stop_server(child);
}

/* A compositor that stops answering mid-request. This is what the deadline is
 * for, and the only shape that actually exercises it. */
static void check_wedged(const char *socket_name)
{
    ksd_wayland *connection = NULL;
    ksd_operation_result result;
    pid_t child;

    /* Lowered so proving it costs a fraction of a second rather than the whole
     * production budget. */
    ksd_wayland_clipboard_timeout_ms = 250;
    child = start_server(socket_name, "wedged");
    assert(ksd_wayland_open(socket_name, &connection) == KSD_STATUS_OK);

    ksd_result_init(&result);
    ksd_wayland_clipboard_text(connection, &result);
    assert(result.status == KSD_STATUS_TIMEOUT);
    assert(result.tail == NULL);
    ksd_result_clear(&result);

    ksd_wayland_close(connection);
    stop_server(child);
    ksd_wayland_clipboard_timeout_ms = 5000;
}

static void check_keyboard(const char *socket_name)
{
    ksd_wayland *connection = NULL;
    ksd_operation_result result;
    pid_t child = start_server(socket_name, "keyboard");
    assert(ksd_wayland_open(socket_name, &connection) == KSD_STATUS_OK);
    assert(ksd_wayland_supported(connection).keyboard_keymap);
    ksd_result_init(&result);
    ksd_wayland_keyboard_state(connection, &result);
    assert(result.status == KSD_STATUS_OK);
    char *json = strndup((const char *)result.tail + 4u,
                         framed_length(&result));
    assert(json != NULL && strstr(json, "\"keymap\":") != NULL);
    assert(strstr(json, "\"layouts\":[\"Test layout\"]") != NULL);
    assert(strstr(json, "\"group\":") == NULL);
    assert(strstr(json, "\"locked\":") == NULL);
    const char *start = strstr(json, "\"mapRevision\":\"");
    assert(start != NULL);
    char revision[64];
    memcpy(revision, start + strlen("\"mapRevision\":\""), sizeof(revision));
    free(json);
    ksd_result_clear(&result);
    ksd_wayland_keyboard_state_since(connection, (const uint8_t *)revision,
                                      sizeof(revision), &result);
    assert(result.status == KSD_STATUS_OK);
    json = strndup((const char *)result.tail + 4u, framed_length(&result));
    assert(json != NULL && strstr(json, "\"keymap\":") == NULL);
    assert(strstr(json, "\"mapRevision\":") != NULL);
    free(json);
    ksd_result_clear(&result);
    ksd_wayland_close(connection);
    stop_server(child);

    child = start_server(socket_name, "keyboard-invalid");
    assert(ksd_wayland_open(socket_name, &connection) == KSD_STATUS_OK);
    assert(!ksd_wayland_supported(connection).keyboard_keymap);
    ksd_wayland_keyboard_state(connection, &result);
    assert(result.status == KSD_STATUS_UNAVAILABLE);
    ksd_result_clear(&result);
    ksd_wayland_close(connection);
    stop_server(child);
}

int main(void)
{
    char socket_name[64];
    ksd_wayland *connection = NULL;

    if (getenv("XDG_RUNTIME_DIR") == NULL) {
        fputs("XDG_RUNTIME_DIR unset: skipping Wayland tests\n", stderr);
        return 77;
    }
    /* Named for this process so concurrent runs cannot collide on one socket,
     * which is how the X11 suite came to adopt a leftover server. */
    snprintf(socket_name, sizeof(socket_name), "ksd-test-wl-%ld",
             (long)getpid());

    /* No compositor at all is UNAVAILABLE, not a crash and not a hang. This is
     * the ordinary case on an X11 session and must be cheap. */
    assert(ksd_wayland_open("ksd-test-wl-nothing-here", &connection)
           == KSD_STATUS_UNAVAILABLE);
    assert(connection == NULL);

    check_normal(socket_name);
    check_keyboard(socket_name);
    check_window_list(socket_name);
    check_window_query(socket_name, "normal");
    check_window_query(socket_name, "wlr");
    check_window_query(socket_name, "cosmic");
    check_wlr_windows(socket_name);
    check_cosmic_windows(socket_name);
    check_screencopy(socket_name);
    check_image_copy(socket_name);
    check_absolute_pointer(socket_name);
    check_empty(socket_name);
    check_silent(socket_name);
    check_wedged(socket_name);
    ksd_wayland_close(NULL);
    return 0;
}
