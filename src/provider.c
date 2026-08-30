#include "provider.h"

#include "common.h"
#include "keysharp_desktop/protocol.h"

#include <errno.h>
#include <gio/gio.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KSD_PROVIDER_TIMEOUT_MS 5000
#define KSD_PROVIDER_MAX_BLOB (256u * 1024u * 1024u)

typedef struct extension_target {
    const char *name;
    const char *path;
    const char *interface;
} extension_target;

typedef struct watch_context {
    int output_fd;
    bool failed;
} watch_context;

static const extension_target gnome_target = {
    "io.github.keysharp.GnomeShell",
    "/io/github/keysharp/GnomeShell",
    "io.github.keysharp.GnomeShell1",
};
static const extension_target cinnamon_target = {
    "io.github.keysharp.CinnamonShell",
    "/io/github/keysharp/CinnamonShell",
    "io.github.keysharp.CinnamonShell1",
};

static pthread_once_t bus_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t operation_mutex = PTHREAD_MUTEX_INITIALIZER;
static GDBusConnection *session_bus;

static void initialize_session_bus(void)
{
    GError *error = NULL;

    session_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (session_bus == NULL && error != NULL) {
        fprintf(stderr, "keysharp-desktop serve: session bus: %s\n",
                error->message);
        g_error_free(error);
    }
}

static GDBusConnection *get_session_bus(void)
{
    (void)pthread_once(&bus_once, initialize_session_bus);
    return session_bus;
}

static const extension_target *target_for(ksd_backend backend)
{
    if (backend == KSD_BACKEND_GNOME)
        return &gnome_target;
    if (backend == KSD_BACKEND_CINNAMON)
        return &cinnamon_target;
    return NULL;
}

static GVariant *provider_call(ksd_backend backend, const char *method,
                               GVariant *parameters,
                               const GVariantType *reply_type,
                               GError **error)
{
    const extension_target *target = target_for(backend);
    GDBusConnection *connection = get_session_bus();
    GVariant *reply;

    if (target == NULL || connection == NULL) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "provider operation is unavailable on this backend");
        return NULL;
    }

    pthread_mutex_lock(&operation_mutex);
    reply = g_dbus_connection_call_sync(connection, target->name, target->path,
        target->interface, method, parameters, reply_type,
        G_DBUS_CALL_FLAGS_NONE, KSD_PROVIDER_TIMEOUT_MS, NULL, error);
    pthread_mutex_unlock(&operation_mutex);
    return reply;
}

static bool write_blob_frame(int output_fd, const void *data, size_t length)
{
    uint8_t status = KSD_RESPONSE_OK;
    uint32_t wire_length;

    if (length > UINT32_MAX || length > KSD_PROVIDER_MAX_BLOB)
        return false;
    wire_length = (uint32_t)length;
    return ksd_write_all(output_fd, &status, sizeof(status))
        && ksd_write_all(output_fd, &wire_length, sizeof(wire_length))
        && (wire_length == 0u
            || ksd_write_all(output_fd, data, (size_t)wire_length));
}

static bool write_provider_error(int output_fd, GError *error)
{
    return ksd_write_error_response(output_fd,
        error != NULL ? error->message : "provider request failed");
}

static bool write_string_reply(int output_fd, GVariant *reply, GError *error)
{
    gchar *value = NULL;
    bool result;

    if (reply == NULL)
        return write_provider_error(output_fd, error);
    g_variant_get(reply, "(s)", &value);
    result = write_blob_frame(output_fd, value, strlen(value));
    g_free(value);
    g_variant_unref(reply);
    return result;
}

static bool write_boolean_reply(int output_fd, GVariant *reply, GError *error)
{
    gboolean value = FALSE;
    uint8_t encoded;
    bool result;

    if (reply == NULL)
        return write_provider_error(output_fd, error);
    g_variant_get(reply, "(b)", &value);
    encoded = value == TRUE ? 1u : 0u;
    result = write_blob_frame(output_fd, &encoded, sizeof(encoded));
    g_variant_unref(reply);
    return result;
}

bool ksd_provider_window_list(ksd_backend backend, int output_fd,
                              bool include_hidden)
{
    GError *error = NULL;
    GVariant *reply = provider_call(backend, "GetWindowList",
        g_variant_new("(b)", include_hidden ? TRUE : FALSE),
        G_VARIANT_TYPE("(s)"), &error);
    bool result = write_string_reply(output_fd, reply, error);
    if (error != NULL)
        g_error_free(error);
    return result;
}

bool ksd_provider_active_window(ksd_backend backend, int output_fd)
{
    GError *error = NULL;
    GVariant *reply = provider_call(backend, "GetActiveWindow", NULL,
                                    G_VARIANT_TYPE("(s)"), &error);
    bool result = write_string_reply(output_fd, reply, error);
    if (error != NULL)
        g_error_free(error);
    return result;
}

bool ksd_provider_window_handle_command(ksd_backend backend, int output_fd,
                                        const char *method, uint64_t handle)
{
    GError *error = NULL;
    GVariant *reply = provider_call(backend, method,
        g_variant_new("(t)", (guint64)handle), G_VARIANT_TYPE("(b)"), &error);
    bool result = write_boolean_reply(output_fd, reply, error);
    if (error != NULL)
        g_error_free(error);
    return result;
}

bool ksd_provider_window_move_resize(ksd_backend backend, int output_fd,
                                     const char *method, uint64_t handle,
                                     int x, int y, int width, int height)
{
    GError *error = NULL;
    GVariant *reply = provider_call(backend, method,
        g_variant_new("(tiiii)", (guint64)handle, x, y, width, height),
        G_VARIANT_TYPE("(b)"), &error);
    bool result = write_boolean_reply(output_fd, reply, error);
    if (error != NULL)
        g_error_free(error);
    return result;
}

bool ksd_provider_window_integer_command(ksd_backend backend, int output_fd,
                                         const char *method, uint64_t handle,
                                         int value)
{
    GError *error = NULL;
    GVariant *reply = provider_call(backend, method,
        g_variant_new("(ti)", (guint64)handle, value),
        G_VARIANT_TYPE("(b)"), &error);
    bool result = write_boolean_reply(output_fd, reply, error);
    if (error != NULL)
        g_error_free(error);
    return result;
}

bool ksd_provider_window_boolean_command(ksd_backend backend, int output_fd,
                                         const char *method, uint64_t handle,
                                         bool value)
{
    GError *error = NULL;
    GVariant *reply = provider_call(backend, method,
        g_variant_new("(tb)", (guint64)handle, value ? TRUE : FALSE),
        G_VARIANT_TYPE("(b)"), &error);
    bool result = write_boolean_reply(output_fd, reply, error);
    if (error != NULL)
        g_error_free(error);
    return result;
}

bool ksd_provider_clipboard_mimetypes(ksd_backend backend, int output_fd)
{
    GError *error = NULL;
    GVariant *reply = provider_call(backend, "GetClipboardMimetypes", NULL,
                                    G_VARIANT_TYPE("(as)"), &error);
    gchar **values = NULL;
    uint8_t *encoded = NULL;
    size_t length = 0u;
    bool result;

    if (reply == NULL) {
        result = write_provider_error(output_fd, error);
        goto done;
    }
    g_variant_get(reply, "(^as)", &values);
    for (size_t index = 0u; values[index] != NULL; index++) {
        size_t item_length = strlen(values[index]) + 1u;
        if (length > KSD_PROVIDER_MAX_BLOB - item_length) {
            result = ksd_write_error_response(output_fd,
                "clipboard mimetype response is too large");
            goto done;
        }
        length += item_length;
    }
    if (length != 0u) {
        encoded = malloc(length);
        if (encoded == NULL) {
            result = ksd_write_error_response(output_fd, "out of memory");
            goto done;
        }
        size_t offset = 0u;
        for (size_t index = 0u; values[index] != NULL; index++) {
            size_t item_length = strlen(values[index]) + 1u;
            memcpy(encoded + offset, values[index], item_length);
            offset += item_length;
        }
    }
    result = write_blob_frame(output_fd, encoded, length);

done:
    free(encoded);
    g_strfreev(values);
    if (reply != NULL)
        g_variant_unref(reply);
    if (error != NULL)
        g_error_free(error);
    return result;
}

bool ksd_provider_clipboard_content(ksd_backend backend, int output_fd,
                                    const char *mimetype)
{
    GError *error = NULL;
    GVariant *reply = provider_call(backend, "GetClipboardContent",
        g_variant_new("(s)", mimetype), G_VARIANT_TYPE("(ay)"), &error);
    GVariant *bytes = NULL;
    const uint8_t *data = NULL;
    gsize length = 0u;
    bool result;

    if (reply == NULL) {
        result = write_provider_error(output_fd, error);
        goto done;
    }
    g_variant_get(reply, "(@ay)", &bytes);
    data = g_variant_get_fixed_array(bytes, &length, sizeof(uint8_t));
    result = write_blob_frame(output_fd, data, (size_t)length);

done:
    if (bytes != NULL)
        g_variant_unref(bytes);
    if (reply != NULL)
        g_variant_unref(reply);
    if (error != NULL)
        g_error_free(error);
    return result;
}

bool ksd_provider_clipboard_text(ksd_backend backend, int output_fd)
{
    GError *error = NULL;
    GVariant *reply = provider_call(backend, "GetClipboardText", NULL,
                                    G_VARIANT_TYPE("(s)"), &error);
    bool result = write_string_reply(output_fd, reply, error);
    if (error != NULL)
        g_error_free(error);
    return result;
}

static bool register_broker(ksd_backend backend)
{
    GError *error = NULL;
    GVariant *reply = provider_call(backend, "RegisterBroker", NULL,
                                    G_VARIANT_TYPE("(b)"), &error);
    gboolean registered = FALSE;

    if (reply != NULL) {
        g_variant_get(reply, "(b)", &registered);
        g_variant_unref(reply);
    }
    if (error != NULL)
        g_error_free(error);
    return registered == TRUE;
}

static void window_event(GDBusConnection *connection, const gchar *sender_name,
                         const gchar *object_path, const gchar *interface_name,
                         const gchar *signal_name, GVariant *parameters,
                         gpointer user_data)
{
    watch_context *watch = user_data;
    const gchar *type;
    const gchar *json;
    uint8_t *encoded;
    size_t type_length;
    size_t json_length;
    size_t length;

    (void)connection;
    (void)sender_name;
    (void)object_path;
    (void)interface_name;
    (void)signal_name;
    g_variant_get(parameters, "(&s&s)", &type, &json);
    type_length = strlen(type) + 1u;
    json_length = strlen(json);
    if (type_length > KSD_PROVIDER_MAX_BLOB
        || json_length > KSD_PROVIDER_MAX_BLOB - type_length) {
        watch->failed = true;
        return;
    }
    length = type_length + json_length;
    encoded = malloc(length == 0u ? 1u : length);
    if (encoded == NULL) {
        watch->failed = true;
        return;
    }
    memcpy(encoded, type, type_length);
    memcpy(encoded + type_length, json, json_length);
    if (!write_blob_frame(watch->output_fd, encoded, length))
        watch->failed = true;
    free(encoded);
}

static void clipboard_event(GDBusConnection *connection,
                            const gchar *sender_name,
                            const gchar *object_path,
                            const gchar *interface_name,
                            const gchar *signal_name, GVariant *parameters,
                            gpointer user_data)
{
    watch_context *watch = user_data;
    GVariant *text_value = NULL;
    GVariant *mimetypes = NULL;
    const gchar *clipboard_text;
    gsize text_length;
    size_t length;
    uint8_t *encoded;
    size_t offset;

    (void)connection;
    (void)sender_name;
    (void)object_path;
    (void)interface_name;
    (void)signal_name;
    text_value = g_variant_get_child_value(parameters, 0u);
    mimetypes = g_variant_get_child_value(parameters, 1u);
    clipboard_text = g_variant_get_string(text_value, &text_length);
    if ((size_t)text_length >= KSD_PROVIDER_MAX_BLOB) {
        watch->failed = true;
        goto done;
    }
    length = (size_t)text_length + 1u;
    for (gsize index = 0u; index < g_variant_n_children(mimetypes); index++) {
        GVariant *item = g_variant_get_child_value(mimetypes, index);
        gsize item_length;
        (void)g_variant_get_string(item, &item_length);
        g_variant_unref(item);
        if ((size_t)item_length + 1u > KSD_PROVIDER_MAX_BLOB - length) {
            watch->failed = true;
            goto done;
        }
        length += (size_t)item_length + 1u;
    }
    encoded = malloc(length);
    if (encoded == NULL) {
        watch->failed = true;
        goto done;
    }
    memcpy(encoded, clipboard_text, (size_t)text_length);
    encoded[text_length] = '\0';
    offset = (size_t)text_length + 1u;
    for (gsize index = 0u; index < g_variant_n_children(mimetypes); index++) {
        GVariant *item = g_variant_get_child_value(mimetypes, index);
        gsize item_length;
        const gchar *value = g_variant_get_string(item, &item_length);
        memcpy(encoded + offset, value, (size_t)item_length);
        encoded[offset + item_length] = '\0';
        offset += (size_t)item_length + 1u;
        g_variant_unref(item);
    }
    if (!write_blob_frame(watch->output_fd, encoded, length))
        watch->failed = true;
    free(encoded);

done:
    if (text_value != NULL)
        g_variant_unref(text_value);
    if (mimetypes != NULL)
        g_variant_unref(mimetypes);
}

bool ksd_provider_watch(ksd_backend backend, int output_fd, bool clipboard)
{
    const extension_target *target = target_for(backend);
    GDBusConnection *connection = get_session_bus();
    GMainContext *context;
    watch_context watch = {
        .output_fd = output_fd,
    };
    guint subscription;
    bool result = true;

    if (target == NULL || connection == NULL)
        return ksd_write_error_response(output_fd,
            "provider event stream is unavailable on this backend");
    context = g_main_context_new();
    if (context == NULL)
        return ksd_write_error_response(output_fd, "out of memory");
    g_main_context_push_thread_default(context);
    subscription = g_dbus_connection_signal_subscribe(connection,
        target->name, target->interface,
        clipboard ? "ClipboardChanged" : "WindowEvent", target->path, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        clipboard ? clipboard_event : window_event, &watch, NULL);
    if (subscription == 0u) {
        result = ksd_write_error_response(output_fd,
            "could not subscribe to provider events");
        goto done;
    }
    if (!register_broker(backend)) {
        result = ksd_write_error_response(output_fd,
            "provider rejected broker registration");
        goto done;
    }
    if (!write_blob_frame(output_fd, NULL, 0u)) {
        result = false;
        goto done;
    }

    while (!watch.failed) {
        struct pollfd descriptor = {
            .fd = output_fd,
            .events = POLLIN | POLLHUP | POLLERR,
        };
        while (g_main_context_iteration(context, FALSE)) {
        }
        if (watch.failed)
            break;
        int ready = poll(&descriptor, 1u, 50);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0 || (ready > 0
            && (descriptor.revents & (POLLIN | POLLHUP | POLLERR)) != 0))
            break;
    }
    result = !watch.failed;

done:
    if (subscription != 0u)
        g_dbus_connection_signal_unsubscribe(connection, subscription);
    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);
    return result;
}
