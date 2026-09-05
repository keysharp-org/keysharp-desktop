#include "operation_result.h"
#include "portal_capture.h"
#include "protocol.h"
#include "protocol_io.h"

#include <assert.h>
#include <errno.h>
#include <gio/gio.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#define PORTAL_SERVICE "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"
#define SCREENSHOT_INTERFACE "org.freedesktop.portal.Screenshot"
#define REQUEST_INTERFACE "org.freedesktop.portal.Request"

static const uint8_t screenshot_png[] = {
    0x89u, 0x50u, 0x4eu, 0x47u, 0x0du, 0x0au, 0x1au, 0x0au,
    0x00u, 0x00u, 0x00u, 0x0du, 0x49u, 0x48u, 0x44u, 0x52u,
    0x00u, 0x00u, 0x00u, 0x08u, 0x00u, 0x00u, 0x00u, 0x04u,
    0x08u, 0x06u, 0x00u, 0x00u, 0x00u, 0xb3u, 0xcdu, 0x7eu, 0xf0u,
    0x00u, 0x00u, 0x00u, 0x12u, 0x49u, 0x44u, 0x41u, 0x54u,
    0x78u, 0x9cu, 0x63u, 0x30u, 0x56u, 0x12u, 0xfcu, 0x8fu,
    0x0fu, 0x33u, 0xd0u, 0x5eu, 0x01u, 0x00u, 0x5du, 0x39u,
    0x2cu, 0xa1u, 0x8bu, 0x56u, 0xd0u, 0x16u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x49u, 0x45u, 0x4eu, 0x44u, 0xaeu, 0x42u,
    0x60u, 0x82u,
};

static const char portal_xml[] =
    "<node>"
    "<interface name='org.freedesktop.portal.Screenshot'>"
    "<method name='Screenshot'>"
    "<arg type='s' direction='in'/><arg type='a{sv}' direction='in'/>"
    "<arg type='o' direction='out'/></method>"
    "<property name='version' type='u' access='read'/>"
    "<property name='AvailableTargets' type='u' access='read'/>"
    "</interface></node>";

static const char request_xml[] =
    "<node><interface name='org.freedesktop.portal.Request'>"
    "<method name='Close'/><signal name='Response'>"
    "<arg type='u'/><arg type='a{sv}'/>"
    "</signal></interface></node>";

typedef struct pending_response {
    GDBusConnection *connection;
    char *destination;
    char *path;
    char *uri;
    uint32_t code;
} pending_response;

static GDBusNodeInfo *request_info;
static unsigned screenshot_calls;
static const char *screenshot_uri;

static gboolean emit_response(void *user_data)
{
    pending_response *pending = user_data;
    GVariantBuilder results;
    GError *error = NULL;

    g_variant_builder_init(&results, G_VARIANT_TYPE_VARDICT);
    if (pending->code == 0u)
        g_variant_builder_add(&results, "{sv}", "uri",
                              g_variant_new_string(pending->uri));
    bool emitted = g_dbus_connection_emit_signal(pending->connection,
        pending->destination, pending->path, REQUEST_INTERFACE, "Response",
        g_variant_new("(u@a{sv})", pending->code,
                      g_variant_builder_end(&results)), &error);
    assert(emitted);
    assert(error == NULL);
    g_object_unref(pending->connection);
    g_free(pending->destination);
    g_free(pending->path);
    g_free(pending->uri);
    free(pending);
    return G_SOURCE_REMOVE;
}

static void request_method(GDBusConnection *connection, const char *sender,
                           const char *object_path,
                           const char *interface_name,
                           const char *method_name, GVariant *parameters,
                           GDBusMethodInvocation *invocation, void *user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)parameters;
    (void)user_data;
    assert(strcmp(method_name, "Close") == 0);
    g_dbus_method_invocation_return_value(invocation, NULL);
}

static const GDBusInterfaceVTable request_vtable = {
    .method_call = request_method,
};

static void screenshot_method(GDBusConnection *connection,
                              const char *sender, const char *object_path,
                              const char *interface_name,
                              const char *method_name, GVariant *parameters,
                              GDBusMethodInvocation *invocation,
                              void *user_data)
{
    const char *parent;
    const char *token = NULL;
    GVariant *options = NULL;
    gboolean interactive = true;
    guint32 target = 0u;
    char *component;
    char *path;
    GError *error = NULL;

    (void)object_path;
    (void)interface_name;
    (void)user_data;
    assert(strcmp(method_name, "Screenshot") == 0);
    g_variant_get(parameters, "(&s@a{sv})", &parent, &options);
    assert(parent[0] == '\0');
    assert(g_variant_lookup(options, "handle_token", "&s", &token));
    assert(g_variant_lookup(options, "interactive", "b", &interactive));
    assert(!interactive);
    assert(g_variant_lookup(options, "target", "u", &target));
    assert(target == 1u);
    component = g_strdup(sender[0] == ':' ? sender + 1 : sender);
    assert(component != NULL);
    for (char *cursor = component; *cursor != '\0'; cursor++)
        if (*cursor == '.')
            *cursor = '_';
    path = g_strdup_printf("/org/freedesktop/portal/desktop/request/%s/%s",
                           component, token);
    assert(path != NULL);
    assert(g_dbus_connection_register_object(connection, path,
        request_info->interfaces[0], &request_vtable, NULL, NULL, &error)
        != 0u);
    assert(error == NULL);
    g_dbus_method_invocation_return_value(invocation,
                                          g_variant_new("(o)", path));

    pending_response *pending = calloc(1u, sizeof(*pending));
    assert(pending != NULL);
    pending->connection = g_object_ref(connection);
    pending->destination = g_strdup(sender);
    pending->path = path;
    pending->uri = g_strdup(screenshot_uri);
    pending->code = screenshot_calls++ == 0u ? 0u : 1u;
    assert(pending->destination != NULL && pending->uri != NULL);
    assert(g_idle_add(emit_response, pending) != 0u);
    g_free(component);
    g_variant_unref(options);
}

static GVariant *screenshot_property(GDBusConnection *connection,
                                     const char *sender,
                                     const char *object_path,
                                     const char *interface_name,
                                     const char *property_name,
                                     GError **error, void *user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)error;
    (void)user_data;
    if (strcmp(property_name, "version") == 0)
        return g_variant_new_uint32(3u);
    assert(strcmp(property_name, "AvailableTargets") == 0);
    return g_variant_new_uint32(1u);
}

static const GDBusInterfaceVTable screenshot_vtable = {
    .method_call = screenshot_method,
    .get_property = screenshot_property,
};

static void run_portal(int ready, const char *uri)
{
    GError *error = NULL;
    GDBusConnection *connection;
    GDBusNodeInfo *portal_info;
    GVariant *reply;
    uint32_t name_result;

    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() == 1)
        _exit(0);
    screenshot_uri = uri;
    connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    assert(connection != NULL && error == NULL);
    reply = g_dbus_connection_call_sync(connection, "org.freedesktop.DBus",
        "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
        g_variant_new("(su)", PORTAL_SERVICE, 0u), G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &error);
    assert(reply != NULL && error == NULL);
    g_variant_get(reply, "(u)", &name_result);
    assert(name_result == 1u);
    g_variant_unref(reply);
    portal_info = g_dbus_node_info_new_for_xml(portal_xml, &error);
    request_info = g_dbus_node_info_new_for_xml(request_xml, &error);
    assert(portal_info != NULL && request_info != NULL && error == NULL);
    assert(g_dbus_connection_register_object(connection, PORTAL_PATH,
        portal_info->interfaces[0], &screenshot_vtable, NULL, NULL, &error)
        != 0u);
    assert(error == NULL);
    assert(write(ready, "1", 1u) == 1);
    close(ready);
    GMainLoop *loop = g_main_loop_new(NULL, false);
    assert(loop != NULL);
    g_main_loop_run(loop);
    _exit(0);
}

static void write_all(int descriptor, const void *data, size_t length)
{
    const uint8_t *bytes = data;
    size_t offset = 0u;

    while (offset < length) {
        ssize_t count = write(descriptor, bytes + offset, length - offset);
        if (count < 0 && errno == EINTR)
            continue;
        assert(count > 0);
        offset += (size_t)count;
    }
}

int main(void)
{
    GTestDBus *bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    char template[256];
    char ready_byte;
    int ready[2];
    int image;
    int status;

    assert(bus != NULL);
    g_test_dbus_up(bus);
    int length = snprintf(template, sizeof(template),
                          "%s/screenshot-portal-test-XXXXXX.png",
                          g_get_tmp_dir());
    assert(length > 0 && (size_t)length < sizeof(template));
    image = mkstemps(template, 4);
    assert(image >= 0);
    write_all(image, screenshot_png, sizeof(screenshot_png));
    close(image);
    char *uri = g_filename_to_uri(template, NULL, NULL);
    assert(uri != NULL);
    assert(pipe(ready) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(ready[0]);
        run_portal(ready[1], uri);
    }
    close(ready[1]);
    assert(read(ready[0], &ready_byte, 1u) == 1);
    close(ready[0]);

    assert(ksd_portal_capture_available());
    ksd_operation_result result;
    ksd_result_init(&result);
    ksd_portal_capture_desktop(&result);
    assert(result.status == KSD_STATUS_OK);
    assert(result.tail == NULL && result.payload_fd >= 0);
    assert(result.tail_length == sizeof(screenshot_png) + 20u);
    uint8_t captured[sizeof(screenshot_png) + 20u];
    assert(pread(result.payload_fd, captured, sizeof(captured), 0)
           == (ssize_t)sizeof(captured));
    assert(ksd_decode_u16(captured) == KSD_CAPTURE_FORMAT_PNG);
    assert(ksd_decode_u32(captured + 4u) == 8u);
    assert(ksd_decode_u32(captured + 8u) == 4u);
    assert(ksd_decode_u32(captured + 12u) == 0u);
    assert(ksd_decode_u32(captured + 16u) == sizeof(screenshot_png));
    assert(memcmp(captured + 20u, screenshot_png, sizeof(screenshot_png))
           == 0);
    ksd_result_clear(&result);
    assert(access(template, F_OK) != 0 && errno == ENOENT);

    ksd_result_init(&result);
    ksd_portal_capture_desktop(&result);
    assert(result.status == KSD_STATUS_CANCELLED);
    ksd_result_clear(&result);

    kill(child, SIGTERM);
    assert(waitpid(child, &status, 0) == child);
    g_free(uri);
    g_test_dbus_down(bus);
    g_object_unref(bus);
    return 0;
}
