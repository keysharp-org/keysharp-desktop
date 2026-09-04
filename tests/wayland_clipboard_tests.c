#include "ext-data-control-v1-server-protocol.h"
#include "operation_result.h"
#include "protocol.h"
#include "protocol_io.h"
#include "wl_clipboard.h"
#include "wl_connect.h"

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
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
    (void)c; (void)r; (void)id;
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

static void bind_seat(struct wl_client *client, void *data, uint32_t version,
                      uint32_t id)
{
    struct wl_resource *resource = wl_resource_create(client,
        &wl_seat_interface, (int)version, id);

    (void)data;
    if (resource == NULL)
        return;
    wl_resource_set_implementation(resource, &seat_impl, NULL, NULL);
    wl_seat_send_capabilities(resource, 0u);
}

/* Runs the compositor. Never returns. */
static void run_server(const char *socket_name, int ready)
{
    struct wl_display *display = wl_display_create();

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
    check_empty(socket_name);
    check_silent(socket_name);
    check_wedged(socket_name);
    ksd_wayland_close(NULL);
    return 0;
}
