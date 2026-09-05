#include "portal_capture.h"

#include "protocol.h"
#include "protocol_io.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <linux/memfd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define KSD_PORTAL_SERVICE "org.freedesktop.portal.Desktop"
#define KSD_PORTAL_PATH "/org/freedesktop/portal/desktop"
#define KSD_PORTAL_SCREENSHOT "org.freedesktop.portal.Screenshot"
#define KSD_PORTAL_REQUEST "org.freedesktop.portal.Request"
#define KSD_PORTAL_CALL_TIMEOUT_MS 5000
#define KSD_PORTAL_RESPONSE_TIMEOUT_MS 30000
#define KSD_PORTAL_SCREEN_TARGET 1u
#define KSD_CAPTURE_HEADER_SIZE 20u

typedef struct portal_response {
    char *predicted_path;
    char *request_prefix;
    char *returned_path;
    char *early_path;
    char *early_uri;
    char *uri;
    uint32_t early_code;
    uint32_t code;
    bool early_valid;
    bool done;
    bool timed_out;
} portal_response;

static bool property_u32(GDBusConnection *connection, const char *name,
                         uint32_t *value)
{
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(connection,
        KSD_PORTAL_SERVICE, KSD_PORTAL_PATH,
        "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", KSD_PORTAL_SCREENSHOT, name),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE,
        KSD_PORTAL_CALL_TIMEOUT_MS, NULL, &error);
    if (reply == NULL) {
        g_clear_error(&error);
        return false;
    }
    GVariant *boxed = NULL;
    GVariant *inner = NULL;
    g_variant_get(reply, "(@v)", &boxed);
    if (boxed != NULL)
        inner = g_variant_get_variant(boxed);
    bool valid = inner != NULL
        && g_variant_is_of_type(inner, G_VARIANT_TYPE_UINT32);
    if (valid)
        *value = g_variant_get_uint32(inner);
    if (inner != NULL)
        g_variant_unref(inner);
    if (boxed != NULL)
        g_variant_unref(boxed);
    g_variant_unref(reply);
    return valid;
}

static bool portal_screen_supported(GDBusConnection *connection,
                                    uint32_t *version)
{
    uint32_t detected;
    uint32_t targets;

    if (!property_u32(connection, "version", &detected))
        return false;
    if (detected >= 3u
        && (!property_u32(connection, "AvailableTargets", &targets)
            || (targets & KSD_PORTAL_SCREEN_TARGET) == 0u))
        return false;
    if (version != NULL)
        *version = detected;
    return true;
}

bool ksd_portal_capture_available(void)
{
    GError *error = NULL;
    GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL,
                                                  &error);
    bool available = connection != NULL
        && portal_screen_supported(connection, NULL);

    if (connection != NULL)
        g_object_unref(connection);
    g_clear_error(&error);
    return available;
}

static char *request_token(void)
{
    char *uuid = g_uuid_string_random();
    if (uuid == NULL)
        return NULL;
    for (char *cursor = uuid; *cursor != '\0'; cursor++)
        if (*cursor == '-')
            *cursor = '_';
    char *token = g_strdup_printf("keysharp_%ld_%s", (long)getpid(), uuid);
    g_free(uuid);
    return token;
}

static char *sender_component(GDBusConnection *connection)
{
    const char *unique = g_dbus_connection_get_unique_name(connection);
    if (unique == NULL || unique[0] == '\0')
        return NULL;
    char *sender = g_strdup(unique[0] == ':' ? unique + 1 : unique);
    if (sender == NULL)
        return NULL;
    for (char *cursor = sender; *cursor != '\0'; cursor++)
        if (*cursor == '.')
            *cursor = '_';
    return sender;
}

static bool parse_response(GVariant *parameters, uint32_t *code, char **uri)
{
    GVariant *results = NULL;
    GVariant *uri_value = NULL;

    if (parameters == NULL
        || !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(ua{sv})")))
        return false;
    g_variant_get(parameters, "(u@a{sv})", code, &results);
    uri_value = g_variant_lookup_value(results, "uri", G_VARIANT_TYPE_STRING);
    if (uri_value != NULL)
        *uri = g_strdup(g_variant_get_string(uri_value, NULL));
    if (uri_value != NULL)
        g_variant_unref(uri_value);
    g_variant_unref(results);
    return true;
}

static void portal_response_received(GDBusConnection *connection,
                                     const char *sender_name,
                                     const char *object_path,
                                     const char *interface_name,
                                     const char *signal_name,
                                     GVariant *parameters,
                                     void *user_data)
{
    portal_response *state = user_data;
    uint32_t code;
    char *uri = NULL;

    (void)connection;
    (void)sender_name;
    (void)interface_name;
    (void)signal_name;
    if (state->done || object_path == NULL
        || !g_str_has_prefix(object_path, state->request_prefix)
        || !parse_response(parameters, &code, &uri))
        return;
    if (strcmp(object_path, state->predicted_path) == 0
        || (state->returned_path != NULL
            && strcmp(object_path, state->returned_path) == 0)) {
        state->code = code;
        state->uri = uri;
        state->done = true;
        return;
    }
    if (state->returned_path == NULL && !state->early_valid) {
        char *path = g_strdup(object_path);
        if (path != NULL) {
            state->early_path = path;
            state->early_uri = uri;
            state->early_code = code;
            state->early_valid = true;
            return;
        }
    }
    g_free(uri);
}

static gboolean portal_response_timed_out(void *user_data)
{
    ((portal_response *)user_data)->timed_out = true;
    return G_SOURCE_REMOVE;
}

static void close_request(GDBusConnection *connection, const char *path)
{
    if (connection == NULL || path == NULL)
        return;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(connection,
        KSD_PORTAL_SERVICE, path, KSD_PORTAL_REQUEST, "Close", NULL, NULL,
        G_DBUS_CALL_FLAGS_NONE, KSD_PORTAL_CALL_TIMEOUT_MS, NULL, &error);
    if (reply != NULL)
        g_variant_unref(reply);
    g_clear_error(&error);
}

static uint32_t portal_screenshot(GDBusConnection *connection,
                                  uint32_t version, char **uri)
{
    GMainContext *context = NULL;
    GSource *timeout = NULL;
    GVariant *reply = NULL;
    GError *error = NULL;
    GVariantBuilder options;
    portal_response state = { 0 };
    char *token = request_token();
    char *sender = sender_component(connection);
    guint subscription = 0u;
    uint32_t status = KSD_STATUS_UNAVAILABLE;

    if (token == NULL || sender == NULL)
        goto done;
    state.request_prefix = g_strdup_printf(
        "/org/freedesktop/portal/desktop/request/%s/", sender);
    state.predicted_path = g_strdup_printf("%s%s", state.request_prefix,
                                           token);
    context = g_main_context_new();
    if (state.request_prefix == NULL || state.predicted_path == NULL
        || context == NULL)
        goto done;
    g_main_context_push_thread_default(context);
    subscription = g_dbus_connection_signal_subscribe(connection,
        KSD_PORTAL_SERVICE, KSD_PORTAL_REQUEST, "Response", NULL, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, portal_response_received, &state, NULL);
    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&options, "{sv}", "handle_token",
                          g_variant_new_string(token));
    g_variant_builder_add(&options, "{sv}", "interactive",
                          g_variant_new_boolean(false));
    if (version >= 3u)
        g_variant_builder_add(&options, "{sv}", "target",
            g_variant_new_uint32(KSD_PORTAL_SCREEN_TARGET));
    reply = g_dbus_connection_call_sync(connection, KSD_PORTAL_SERVICE,
        KSD_PORTAL_PATH, KSD_PORTAL_SCREENSHOT, "Screenshot",
        g_variant_new("(s@a{sv})", "", g_variant_builder_end(&options)),
        G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE,
        KSD_PORTAL_CALL_TIMEOUT_MS, NULL, &error);
    if (reply == NULL)
        goto subscribed;
    const char *returned = NULL;
    g_variant_get(reply, "(&o)", &returned);
    state.returned_path = g_strdup(returned);
    if (state.returned_path == NULL)
        goto subscribed;
    if (state.early_valid
        && strcmp(state.early_path, state.returned_path) == 0) {
        state.code = state.early_code;
        state.uri = state.early_uri;
        state.early_uri = NULL;
        state.done = true;
    }
    timeout = g_timeout_source_new(KSD_PORTAL_RESPONSE_TIMEOUT_MS);
    if (timeout == NULL)
        goto subscribed;
    g_source_set_callback(timeout, portal_response_timed_out, &state, NULL);
    g_source_attach(timeout, context);
    while (!state.done && !state.timed_out)
        (void)g_main_context_iteration(context, true);
    if (state.done) {
        if (state.code == 0u && state.uri != NULL) {
            *uri = state.uri;
            state.uri = NULL;
            status = KSD_STATUS_OK;
        } else if (state.code == 1u) {
            status = KSD_STATUS_CANCELLED;
        }
    } else if (state.timed_out) {
        status = KSD_STATUS_TIMEOUT;
    }

subscribed:
    if (!state.done && reply != NULL)
        close_request(connection, state.returned_path != NULL
            ? state.returned_path : state.predicted_path);
    if (subscription != 0u)
        g_dbus_connection_signal_unsubscribe(connection, subscription);
    if (timeout != NULL) {
        g_source_destroy(timeout);
        g_source_unref(timeout);
    }
    if (context != NULL)
        g_main_context_pop_thread_default(context);

done:
    if (reply != NULL)
        g_variant_unref(reply);
    g_clear_error(&error);
    if (context != NULL)
        g_main_context_unref(context);
    g_free(state.returned_path);
    g_free(state.early_path);
    g_free(state.early_uri);
    g_free(state.uri);
    g_free(state.request_prefix);
    g_free(state.predicted_path);
    g_free(sender);
    g_free(token);
    return status;
}

static uint32_t png_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16
        | (uint32_t)bytes[2] << 8 | bytes[3];
}

static bool png_dimensions(int descriptor, uint32_t *width,
                           uint32_t *height)
{
    static const uint8_t signature[8] = {
        0x89u, 'P', 'N', 'G', '\r', '\n', 0x1au, '\n'
    };
    uint8_t header[24];
    ssize_t count;

    do {
        count = pread(descriptor, header, sizeof(header), 0);
    } while (count < 0 && errno == EINTR);
    if (count != (ssize_t)sizeof(header)
        || memcmp(header, signature, sizeof(signature)) != 0
        || png_u32(header + 8u) != 13u
        || memcmp(header + 12u, "IHDR", 4u) != 0)
        return false;
    *width = png_u32(header + 16u);
    *height = png_u32(header + 20u);
    return *width != 0u && *height != 0u
        && *width <= KSD_MAX_CAPTURE_DIMENSION
        && *height <= KSD_MAX_CAPTURE_DIMENSION
        && (uint64_t)*width * *height <= KSD_MAX_CAPTURE_PIXELS;
}

static bool copy_png(int source, off_t length, uint32_t width,
                     uint32_t height, ksd_operation_result *result)
{
    uint8_t header[KSD_CAPTURE_HEADER_SIZE] = { 0 };
    uint8_t buffer[65536];
    uint64_t total = KSD_CAPTURE_HEADER_SIZE + (uint64_t)length;
    int descriptor = memfd_create("keysharp-desktop-portal",
        MFD_CLOEXEC | MFD_ALLOW_SEALING);
    int seals = F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;

    if (descriptor < 0 || total > UINT32_MAX
        || ftruncate(descriptor, (off_t)total) != 0) {
        if (descriptor >= 0)
            close(descriptor);
        return false;
    }
    ksd_encode_u16(header, KSD_CAPTURE_FORMAT_PNG);
    ksd_encode_u32(header + 4u, width);
    ksd_encode_u32(header + 8u, height);
    ksd_encode_u32(header + 16u, (uint32_t)length);
    size_t header_offset = 0u;
    while (header_offset < sizeof(header)) {
        ssize_t written = pwrite(descriptor, header + header_offset,
                                 sizeof(header) - header_offset,
                                 (off_t)header_offset);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            close(descriptor);
            return false;
        }
        header_offset += (size_t)written;
    }
    off_t offset = 0;
    while (offset < length) {
        size_t wanted = (uint64_t)(length - offset) < sizeof(buffer)
            ? (size_t)(length - offset) : sizeof(buffer);
        ssize_t count;
        do {
            count = pread(source, buffer, wanted, offset);
        } while (count < 0 && errno == EINTR);
        if (count <= 0) {
            close(descriptor);
            return false;
        }
        size_t buffer_offset = 0u;
        while (buffer_offset < (size_t)count) {
            ssize_t written = pwrite(descriptor, buffer + buffer_offset,
                (size_t)count - buffer_offset,
                (off_t)KSD_CAPTURE_HEADER_SIZE + offset
                    + (off_t)buffer_offset);
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0) {
                close(descriptor);
                return false;
            }
            buffer_offset += (size_t)written;
        }
        offset += count;
    }
    if (fcntl(descriptor, F_ADD_SEALS, seals) != 0) {
        close(descriptor);
        return false;
    }
    return ksd_result_take_fd(result, descriptor, (uint32_t)total);
}

static void remove_portal_temporary(const char *path,
                                    const struct stat *opened)
{
    char *canonical = g_canonicalize_filename(path, NULL);
    char *temporary = g_canonicalize_filename(g_get_tmp_dir(), NULL);
    char *directory = canonical == NULL ? NULL : g_path_get_dirname(canonical);
    char *basename = canonical == NULL ? NULL : g_path_get_basename(canonical);
    struct stat current;

    size_t length = basename == NULL ? 0u : strlen(basename);
    if (canonical != NULL && temporary != NULL && directory != NULL
        && basename != NULL && strcmp(directory, temporary) == 0
        && length > 15u && g_str_has_prefix(basename, "screenshot-")
        && g_str_has_suffix(basename, ".png")
        && lstat(canonical, &current) == 0 && S_ISREG(current.st_mode)
        && current.st_dev == opened->st_dev && current.st_ino == opened->st_ino)
        (void)unlink(canonical);
    g_free(basename);
    g_free(directory);
    g_free(temporary);
    g_free(canonical);
}

void ksd_portal_capture_desktop(ksd_operation_result *result)
{
    GError *error = NULL;
    GDBusConnection *connection = NULL;
    char *uri = NULL;
    char *path = NULL;
    uint32_t version;
    uint32_t status;
    uint32_t width;
    uint32_t height;
    int descriptor = -1;
    struct stat file_status;
    bool have_file_status = false;

    if (result == NULL)
        return;
    connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (connection == NULL || !portal_screen_supported(connection, &version)) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                         "the screenshot portal is unavailable");
        goto done;
    }
    status = portal_screenshot(connection, version, &uri);
    if (status != KSD_STATUS_OK) {
        ksd_result_error(result, status, 0u,
            status == KSD_STATUS_CANCELLED
                ? "the screenshot portal was cancelled"
                : status == KSD_STATUS_TIMEOUT
                    ? "the screenshot portal timed out"
                    : "the screenshot portal failed");
        goto done;
    }
    path = g_filename_from_uri(uri, NULL, &error);
    if (path == NULL)
        goto invalid;
    descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0 || fstat(descriptor, &file_status) != 0
        || !S_ISREG(file_status.st_mode) || file_status.st_uid != getuid()
        || file_status.st_size <= 0
        || (uint64_t)file_status.st_size > KSD_MAX_CAPTURE_BYTES
        || !png_dimensions(descriptor, &width, &height)
        || !copy_png(descriptor, file_status.st_size, width, height, result))
        goto invalid;
    have_file_status = true;
    goto done;

invalid:
    ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                     "the screenshot portal returned an invalid image");

done:
    if (descriptor >= 0) {
        if (!have_file_status && fstat(descriptor, &file_status) == 0)
            have_file_status = true;
        if (have_file_status)
            remove_portal_temporary(path, &file_status);
        close(descriptor);
    }
    g_free(path);
    g_free(uri);
    if (connection != NULL)
        g_object_unref(connection);
    g_clear_error(&error);
}
