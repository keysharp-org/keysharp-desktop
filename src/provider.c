#include "provider.h"

#include "protocol.h"
#include "transport.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define KSD_PROVIDER_TIMEOUT_MS 5000
#define KSD_PROVIDER_CAPTURE_TIMEOUT_MS 30000
#define KSD_PROVIDER_WATCH_POLL_MS 250u
#define KSD_PROVIDER_OBJECT_PATH "/org/keysharp/DesktopProvider"
#define KSD_PROVIDER_INTERFACE "org.keysharp.Desktop.Provider1"
#define KSD_MAX_WINDOW_HANDLE_BYTES 128u
#define KSD_MAX_RESERVATION_TTL_MS 60000u

typedef struct provider_connection {
    struct provider_connection *next;
    uid_t uid;
    pid_t provider_pid;
    ksd_backend backend;
    GDBusConnection *connection;
} provider_connection;

typedef struct watch_state {
    ksd_provider_event_fn emit;
    void *user_data;
    bool failed;
} watch_state;

static pthread_mutex_t connection_mutex = PTHREAD_MUTEX_INITIALIZER;
static provider_connection *connections;

typedef struct provider_deadline {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pthread_t thread;
    struct timespec expires;
    GCancellable *cancellable;
    bool done;
    bool started;
} provider_deadline;

static void provider_connection_invalidate(uid_t uid, ksd_backend backend,
                                           GDBusConnection *connection);

static void *deadline_thread(void *user_data)
{
    provider_deadline *deadline = user_data;
    pthread_mutex_lock(&deadline->mutex);
    while (!deadline->done) {
        int status = pthread_cond_timedwait(&deadline->condition,
            &deadline->mutex, &deadline->expires);
        if (status == ETIMEDOUT) {
            g_cancellable_cancel(deadline->cancellable);
            break;
        }
        if (status != 0) {
            g_cancellable_cancel(deadline->cancellable);
            break;
        }
    }
    pthread_mutex_unlock(&deadline->mutex);
    return NULL;
}

static bool deadline_start(provider_deadline *deadline, uint32_t timeout_ms,
                           GError **error)
{
    memset(deadline, 0, sizeof(*deadline));
    deadline->cancellable = g_cancellable_new();
    pthread_condattr_t attributes;
    bool mutex_ready = pthread_mutex_init(&deadline->mutex, NULL) == 0;
    bool attributes_ready = mutex_ready
        && pthread_condattr_init(&attributes) == 0;
    bool condition_ready = attributes_ready
        && pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC) == 0
        && pthread_cond_init(&deadline->condition, &attributes) == 0;
    if (attributes_ready)
        pthread_condattr_destroy(&attributes);
    if (deadline->cancellable == NULL || !condition_ready
        || clock_gettime(CLOCK_MONOTONIC, &deadline->expires) != 0) {
        if (condition_ready)
            pthread_cond_destroy(&deadline->condition);
        if (mutex_ready)
            pthread_mutex_destroy(&deadline->mutex);
        g_clear_object(&deadline->cancellable);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "provider deadline could not be created");
        return false;
    }
    deadline->expires.tv_sec += (time_t)(timeout_ms / 1000u);
    deadline->expires.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (deadline->expires.tv_nsec >= 1000000000L) {
        deadline->expires.tv_sec++;
        deadline->expires.tv_nsec -= 1000000000L;
    }
    if (pthread_create(&deadline->thread, NULL, deadline_thread, deadline)
        != 0) {
        pthread_cond_destroy(&deadline->condition);
        pthread_mutex_destroy(&deadline->mutex);
        g_clear_object(&deadline->cancellable);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "provider deadline thread could not be created");
        return false;
    }
    deadline->started = true;
    return true;
}

static void deadline_finish(provider_deadline *deadline)
{
    if (!deadline->started)
        return;
    pthread_mutex_lock(&deadline->mutex);
    deadline->done = true;
    pthread_cond_signal(&deadline->condition);
    pthread_mutex_unlock(&deadline->mutex);
    (void)pthread_join(deadline->thread, NULL);
    pthread_cond_destroy(&deadline->condition);
    pthread_mutex_destroy(&deadline->mutex);
    g_clear_object(&deadline->cancellable);
    deadline->started = false;
}

static const char *socket_name(ksd_backend backend)
{
    if (backend == KSD_BACKEND_GNOME)
        return "provider-gnome.sock";
    if (backend == KSD_BACKEND_CINNAMON)
        return "provider-cinnamon.sock";
    return NULL;
}

static const char *provider_executable_name(ksd_backend backend)
{
    if (backend == KSD_BACKEND_GNOME)
        return "gnome-shell";
    if (backend == KSD_BACKEND_CINNAMON)
        return "cinnamon";
    return NULL;
}

static GCredentials *provider_peer_credentials(GDBusConnection *connection,
                                                GError **error)
{
    GCredentials *credentials =
        g_dbus_connection_get_peer_credentials(connection);
    if (credentials != NULL)
        return g_object_ref(credentials);

    /* Client-side peer connections do not always retain credentials on the
       GDBusConnection. The Unix socket still has the kernel-authenticated
       identity. */
    GIOStream *stream = g_dbus_connection_get_stream(connection);
    if (!G_IS_SOCKET_CONNECTION(stream)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "desktop provider peer credentials are unavailable");
        return NULL;
    }
    GSocket *socket = g_socket_connection_get_socket(
        G_SOCKET_CONNECTION(stream));
    return g_socket_get_credentials(socket, error);
}

static bool provider_peer_valid(GDBusConnection *connection, uid_t uid,
                                pid_t provider_pid, ksd_backend backend,
                                GError **error)
{
    GError *credential_error = NULL;
    GCredentials *credentials = provider_peer_credentials(connection,
        &credential_error);
    gint64 peer_uid = credentials == NULL ? -1
        : (gint64)g_credentials_get_unix_user(credentials, &credential_error);
    gint64 peer_pid = credential_error == NULL && credentials != NULL
        ? g_credentials_get_unix_pid(credentials, &credential_error) : -1;
    if (credentials != NULL)
        g_object_unref(credentials);
    const char *expected = provider_executable_name(backend);
    char proc_path[64];
    char executable[PATH_MAX + 1u];
    struct stat executable_status;

    if (credential_error != NULL) {
        g_propagate_error(error, credential_error);
        return false;
    }
    int length = snprintf(proc_path, sizeof(proc_path), "/proc/%lld/exe",
                          (long long)peer_pid);
    ssize_t executable_length = length > 0 && (size_t)length < sizeof(proc_path)
        ? readlink(proc_path, executable, sizeof(executable) - 1u) : -1;
    if (peer_uid != (gint64)uid || peer_pid <= 0
        || peer_pid != (gint64)provider_pid || expected == NULL
        || executable_length <= 0
        || (size_t)executable_length >= sizeof(executable)
        || stat(proc_path, &executable_status) != 0
        || !S_ISREG(executable_status.st_mode)
        || executable_status.st_uid != 0u
        || (executable_status.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "desktop provider peer identity is invalid");
        return false;
    }
    executable[executable_length] = '\0';
    const char *basename = strrchr(executable, '/');
    basename = basename == NULL ? executable : basename + 1u;
    if (strcmp(basename, expected) != 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "desktop provider executable is invalid");
        return false;
    }
    return true;
}

static GDBusConnection *provider_connection_open(uid_t uid, pid_t provider_pid,
                                                  ksd_backend backend,
                                                  GError **error)
{
    const char *name = socket_name(backend);
    char path[256];
    GSocketClient *client = NULL;
    GSocketAddress *address = NULL;
    GSocketConnection *socket_connection = NULL;
    GDBusConnection *connection = NULL;
    provider_deadline deadline;

    if (name == NULL || provider_pid <= 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "desktop provider registration is unavailable");
        return NULL;
    }
    if (!deadline_start(&deadline, KSD_PROVIDER_TIMEOUT_MS, error))
        return NULL;
    int length = snprintf(path, sizeof(path),
        "/run/user/%lu/keysharp-desktop/%s", (unsigned long)uid, name);
    if (length <= 0 || (size_t)length >= sizeof(path)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "desktop provider address is invalid");
        goto done;
    }
    client = g_socket_client_new();
    address = g_unix_socket_address_new(path);
    if (client == NULL || address == NULL) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                            "could not allocate provider connection");
        goto done;
    }
    g_socket_client_set_timeout(client,
        (guint)(KSD_PROVIDER_TIMEOUT_MS + 999) / 1000u);
    socket_connection = g_socket_client_connect(client,
        G_SOCKET_CONNECTABLE(address), deadline.cancellable, error);
    if (socket_connection == NULL)
        goto done;
    connection = g_dbus_connection_new_sync(G_IO_STREAM(socket_connection),
        NULL, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
        NULL, deadline.cancellable, error);
    if (connection != NULL
        && !provider_peer_valid(connection, uid, provider_pid, backend,
                                error)) {
        g_object_unref(connection);
        connection = NULL;
    }

done:
    deadline_finish(&deadline);
    if (socket_connection != NULL)
        g_object_unref(socket_connection);
    if (address != NULL)
        g_object_unref(address);
    if (client != NULL)
        g_object_unref(client);
    return connection;
}

static GDBusConnection *provider_connection_get(uid_t uid, pid_t provider_pid,
                                                 ksd_backend backend,
                                                 GError **error)
{
    provider_connection *entry;

    pthread_mutex_lock(&connection_mutex);
    for (entry = connections; entry != NULL; entry = entry->next)
        if (entry->uid == uid && entry->backend == backend)
            break;
    if (entry != NULL && entry->provider_pid != provider_pid) {
        if (entry->connection != NULL)
            g_object_unref(entry->connection);
        entry->connection = NULL;
        entry->provider_pid = provider_pid;
    }
    if (entry != NULL && entry->connection != NULL
        && g_dbus_connection_is_closed(entry->connection)) {
        g_object_unref(entry->connection);
        entry->connection = NULL;
    }
    if (entry != NULL && entry->connection != NULL) {
        GDBusConnection *result = g_object_ref(entry->connection);
        pthread_mutex_unlock(&connection_mutex);
        GError *validation_error = NULL;
        bool valid = provider_peer_valid(result, uid, provider_pid, backend,
                                         &validation_error);
        if (valid)
            return result;
        if (validation_error != NULL)
            g_propagate_error(error, validation_error);
        provider_connection_invalidate(uid, backend, result);
        g_object_unref(result);
        return NULL;
    }
    pthread_mutex_unlock(&connection_mutex);

    GDBusConnection *opened = provider_connection_open(uid, provider_pid,
                                                       backend, error);
    if (opened == NULL)
        return NULL;

    pthread_mutex_lock(&connection_mutex);
    for (entry = connections; entry != NULL; entry = entry->next)
        if (entry->uid == uid && entry->backend == backend)
            break;
    if (entry != NULL && entry->provider_pid != provider_pid) {
        pthread_mutex_unlock(&connection_mutex);
        g_object_unref(opened);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                            "desktop provider registration changed");
        return NULL;
    }
    if (entry == NULL) {
        entry = calloc(1u, sizeof(*entry));
        if (entry == NULL) {
            pthread_mutex_unlock(&connection_mutex);
            g_object_unref(opened);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                                "out of memory");
            return NULL;
        }
        entry->uid = uid;
        entry->provider_pid = provider_pid;
        entry->backend = backend;
        entry->next = connections;
        connections = entry;
    }
    if (entry->connection == NULL) {
        entry->connection = opened;
        opened = NULL;
    }
    GDBusConnection *result = g_object_ref(entry->connection);
    pthread_mutex_unlock(&connection_mutex);
    if (opened != NULL)
        g_object_unref(opened);
    return result;
}

static void provider_connection_invalidate(uid_t uid, ksd_backend backend,
                                            GDBusConnection *connection)
{
    pthread_mutex_lock(&connection_mutex);
    for (provider_connection *entry = connections; entry != NULL;
         entry = entry->next)
        if (entry->uid == uid && entry->backend == backend
            && entry->connection == connection) {
            g_object_unref(entry->connection);
            entry->connection = NULL;
            break;
        }
    pthread_mutex_unlock(&connection_mutex);
}

static GVariant *provider_call(uid_t uid, pid_t provider_pid,
                               ksd_backend backend,
                               const char *method, GVariant *parameters,
                               const GVariantType *reply_type,
                               int timeout_ms, GError **error)
{
    GDBusConnection *connection =
        provider_connection_get(uid, provider_pid, backend, error);
    if (connection == NULL) {
        if (parameters != NULL) {
            g_variant_ref_sink(parameters);
            g_variant_unref(parameters);
        }
        return NULL;
    }
    GVariant *reply = g_dbus_connection_call_sync(connection, NULL,
        KSD_PROVIDER_OBJECT_PATH, KSD_PROVIDER_INTERFACE, method, parameters,
        reply_type, G_DBUS_CALL_FLAGS_NONE, timeout_ms, NULL, error);
    if (reply == NULL && g_dbus_connection_is_closed(connection))
        provider_connection_invalidate(uid, backend, connection);
    g_object_unref(connection);
    return reply;
}

static uint32_t status_for_error(const GError *error)
{
    if (error == NULL)
        return KSD_STATUS_UNAVAILABLE;
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
        return KSD_STATUS_TIMEOUT;
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_BUSY))
        return KSD_STATUS_BUSY;
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        return KSD_STATUS_NOT_FOUND;
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED))
        return KSD_STATUS_DENIED;
    return KSD_STATUS_UNAVAILABLE;
}

static void provider_error(ksd_operation_result *result, GError *error)
{
    ksd_result_error(result, status_for_error(error), 0u,
        error == NULL ? "desktop provider request failed" : error->message);
    if (error != NULL)
        g_error_free(error);
}

static int sealed_capture_memfd(uint32_t width, uint32_t height,
                                const uint8_t *data, size_t length)
{
    uint8_t header[20];
    int seals = F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
    int descriptor = memfd_create("keysharp-desktop-capture",
                                  MFD_CLOEXEC | MFD_ALLOW_SEALING);

    if (descriptor < 0)
        return -1;
    ksd_encode_u16(header, KSD_CAPTURE_FORMAT_PNG);
    ksd_encode_u16(header + 2u, 0u);
    ksd_encode_u32(header + 4u, width);
    ksd_encode_u32(header + 8u, height);
    ksd_encode_u32(header + 12u, 0u);
    ksd_encode_u32(header + 16u, (uint32_t)length);
    if (!ksd_write_all(descriptor, header, sizeof header)
        || !ksd_write_all(descriptor, data, length)
        || fcntl(descriptor, F_ADD_SEALS, seals) != 0) {
        close(descriptor);
        return -1;
    }
    return descriptor;
}

static bool buffer_to_result(ksd_buffer *buffer, ksd_operation_result *result)
{
    if (buffer->length > UINT32_MAX)
        return false;
    uint8_t *data = buffer->data;
    uint32_t length = (uint32_t)buffer->length;
    buffer->data = NULL;
    buffer->length = 0u;
    buffer->capacity = 0u;
    return ksd_result_take(result, data, length);
}

static uint32_t png_u32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24u) | ((uint32_t)data[1] << 16u)
        | ((uint32_t)data[2] << 8u) | (uint32_t)data[3];
}

static bool png_dimensions(const uint8_t *data, size_t length,
                           uint32_t *width, uint32_t *height)
{
    static const uint8_t signature[8] =
        { 0x89u, 'P', 'N', 'G', '\r', '\n', 0x1au, '\n' };
    if (length < 33u || memcmp(data, signature, sizeof(signature)) != 0
        || png_u32(data + 8u) != 13u
        || memcmp(data + 12u, "IHDR", 4u) != 0)
        return false;
    *width = png_u32(data + 16u);
    *height = png_u32(data + 20u);
    return *width != 0u && *height != 0u
        && *width <= KSD_MAX_CAPTURE_DIMENSION
        && *height <= KSD_MAX_CAPTURE_DIMENSION
        && (uint64_t)*width * *height <= KSD_MAX_CAPTURE_PIXELS;
}

static void capture_result(GVariant *reply, GError *error,
                           ksd_operation_result *result)
{
    GVariant *bytes = NULL;
    const uint8_t *data = NULL;
    gsize length = 0u;
    uint32_t width;
    uint32_t height;

    if (reply == NULL) {
        provider_error(result, error);
        return;
    }
    g_variant_get(reply, "(@ay)", &bytes);
    data = g_variant_get_fixed_array(bytes, &length, sizeof(uint8_t));
    if (data == NULL || length == 0u || length > KSD_MAX_CAPTURE_BYTES
        || !png_dimensions(data, (size_t)length, &width, &height)) {
        ksd_result_error(result,
            length > KSD_MAX_CAPTURE_BYTES
                ? KSD_STATUS_RESOURCE_EXHAUSTED : KSD_STATUS_UNAVAILABLE,
            0u, "desktop provider returned an invalid PNG capture");
        goto done;
    }
    int payload_fd = sealed_capture_memfd(width, height, data, (size_t)length);
    if (payload_fd < 0
        || !ksd_result_take_fd(result, payload_fd,
                               (uint32_t)(20u + (size_t)length)))
        ksd_result_error(result, KSD_STATUS_INTERNAL, 0u,
                         "capture buffer is unavailable");

done:
    g_variant_unref(bytes);
    g_variant_unref(reply);
    if (error != NULL)
        g_error_free(error);
}

static void string_result(GVariant *reply, GError *error,
                          ksd_operation_result *result)
{
    const gchar *value;
    gsize length;
    ksd_buffer tail;

    if (reply == NULL) {
        provider_error(result, error);
        return;
    }
    g_variant_get(reply, "(&s)", &value);
    length = strlen(value);
    if (length > KSD_MAX_TEXT_BYTES
        || !ksd_utf8_valid((const uint8_t *)value, (size_t)length, false)) {
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "desktop provider text is invalid or too large");
        g_variant_unref(reply);
        return;
    }
    ksd_buffer_init(&tail, KSD_MAX_TEXT_BYTES + 4u);
    if (!ksd_buffer_u32(&tail, (uint32_t)length)
        || !ksd_buffer_bytes(&tail, value, (size_t)length)
        || !buffer_to_result(&tail, result))
        ksd_result_error(result, KSD_STATUS_INTERNAL, 0u, "out of memory");
    ksd_buffer_clear(&tail);
    g_variant_unref(reply);
    if (error != NULL)
        g_error_free(error);
}

static void boolean_result(GVariant *reply, GError *error,
                           ksd_operation_result *result)
{
    gboolean value = FALSE;
    if (reply == NULL) {
        provider_error(result, error);
        return;
    }
    g_variant_get(reply, "(b)", &value);
    if (value == TRUE)
        (void)ksd_result_take(result, NULL, 0u);
    else
        ksd_result_error(result, KSD_STATUS_NOT_FOUND, 0u,
                         "desktop provider could not apply the operation");
    g_variant_unref(reply);
    if (error != NULL)
        g_error_free(error);
}

static bool parse_empty(const ksd_frame *request)
{
    return request->payload_length == 0u;
}

static bool parse_handle(const ksd_frame *request, uint64_t *handle)
{
    ksd_cursor cursor;
    ksd_cursor_init(&cursor, request->payload, request->payload_length);
    return ksd_cursor_u64(&cursor, handle) && *handle != 0u
        && ksd_cursor_finished(&cursor);
}

static bool valid_rectangle(int32_t x, int32_t y,
                            uint32_t width, uint32_t height)
{
    return width != 0u && height != 0u
        && width <= KSD_MAX_CAPTURE_DIMENSION
        && height <= KSD_MAX_CAPTURE_DIMENSION
        && (int64_t)x + width <= INT32_MAX
        && (int64_t)y + height <= INT32_MAX;
}

static bool valid_capture_rectangle(int32_t x, int32_t y,
                                    uint32_t width, uint32_t height)
{
    return valid_rectangle(x, y, width, height)
        && (uint64_t)width * height <= KSD_MAX_CAPTURE_PIXELS;
}

static bool valid_window_geometry(int32_t x, int32_t y,
                                  uint32_t width, uint32_t height)
{
    return !(x == INT32_MIN && y == INT32_MIN
             && width == 0u && height == 0u)
        && width <= KSD_MAX_CAPTURE_DIMENSION
        && height <= KSD_MAX_CAPTURE_DIMENSION
        && (x == INT32_MIN || width == 0u
            || (int64_t)x + width <= INT32_MAX)
        && (y == INT32_MIN || height == 0u
            || (int64_t)y + height <= INT32_MAX);
}

static bool parse_decimal_handle(const uint8_t *bytes, uint32_t length,
                                 uint64_t *handle)
{
    char text[KSD_MAX_WINDOW_HANDLE_BYTES + 1u];
    if (length == 0u || length > KSD_MAX_WINDOW_HANDLE_BYTES
        || !ksd_utf8_valid(bytes, length, false))
        return false;
    memcpy(text, bytes, length);
    text[length] = '\0';
    for (uint32_t index = 0u; index < length; index++)
        if (text[index] < '0' || text[index] > '9')
            return false;
    if (length > 1u && text[0] == '0')
        return false;
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0u)
        return false;
    *handle = (uint64_t)value;
    return true;
}

static void invalid_request(ksd_operation_result *result)
{
    ksd_result_error(result, KSD_STATUS_INVALID_REQUEST, 0u,
                     "invalid desktop operation payload");
}

bool ksd_provider_capture_supported(ksd_backend backend, uint16_t opcode)
{
    bool window = opcode == KSD_OP_CAPTURE_WINDOW;
    bool area = opcode == KSD_OP_CAPTURE_AREA;
    if (backend == KSD_BACKEND_GNOME)
        return area || window;
    return backend == KSD_BACKEND_CINNAMON && window;
}

static void execute_capture(uid_t uid, pid_t provider_pid,
                            ksd_backend backend,
                            const ksd_frame *request,
                            ksd_operation_result *result)
{
    GError *error = NULL;
    GVariant *reply;
    ksd_cursor cursor;
    ksd_cursor_init(&cursor, request->payload, request->payload_length);
    if (!ksd_provider_capture_supported(backend, request->opcode)) {
        ksd_result_error(result, KSD_STATUS_UNSUPPORTED, 0u,
                         "capture is unavailable on this provider");
        return;
    }
    if (request->opcode == KSD_OP_CAPTURE_WINDOW) {
        uint32_t flags;
        uint32_t length;
        const uint8_t *bytes;
        uint64_t handle;
        if (!ksd_cursor_u32(&cursor, &flags)
            || !ksd_cursor_u32(&cursor, &length)
            || (flags & ~KSD_CAPTURE_WINDOW_INCLUDE_DECORATION) != 0u
            || length == 0u || length > KSD_MAX_WINDOW_HANDLE_BYTES
            || !ksd_cursor_bytes(&cursor, length, &bytes)
            || !ksd_cursor_finished(&cursor)
            || !parse_decimal_handle(bytes, length, &handle)) {
            invalid_request(result);
            return;
        }
        reply = provider_call(uid, provider_pid, backend, "CaptureWindow",
            g_variant_new("(tb)", (guint64)handle,
                (flags & KSD_CAPTURE_WINDOW_INCLUDE_DECORATION) != 0u),
            G_VARIANT_TYPE("(ay)"), KSD_PROVIDER_CAPTURE_TIMEOUT_MS, &error);
        capture_result(reply, error, result);
        return;
    }
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    if (!ksd_cursor_i32(&cursor, &x) || !ksd_cursor_i32(&cursor, &y)
        || !ksd_cursor_u32(&cursor, &width)
        || !ksd_cursor_u32(&cursor, &height)
        || !ksd_cursor_finished(&cursor)
        || !valid_capture_rectangle(x, y, width, height)) {
        invalid_request(result);
        return;
    }
    reply = provider_call(uid, provider_pid, backend, "CaptureArea",
        g_variant_new("(iiii)", x, y, (gint32)width, (gint32)height),
        G_VARIANT_TYPE("(ay)"), KSD_PROVIDER_CAPTURE_TIMEOUT_MS, &error);
    capture_result(reply, error, result);
}

static void execute_query(uid_t uid, pid_t provider_pid, ksd_backend backend,
                          const ksd_frame *request,
                          ksd_operation_result *result)
{
    if (!parse_empty(request)) {
        invalid_request(result);
        return;
    }
    GError *error = NULL;
    GVariant *reply;
    uint8_t encoded[16];
    if (request->opcode == KSD_OP_CURSOR_POSITION) {
        gint32 x;
        gint32 y;
        reply = provider_call(uid, provider_pid, backend,
            "GetCursorPosition", NULL,
            G_VARIANT_TYPE("(ii)"), KSD_PROVIDER_TIMEOUT_MS, &error);
        if (reply == NULL) {
            provider_error(result, error);
            return;
        }
        g_variant_get(reply, "(ii)", &x, &y);
        ksd_encode_u32(encoded, (uint32_t)x);
        ksd_encode_u32(encoded + 4u, (uint32_t)y);
        if (!ksd_result_copy(result, encoded, 8u))
            ksd_result_error(result, KSD_STATUS_INTERNAL, 0u, "out of memory");
        g_variant_unref(reply);
        return;
    }
    if (request->opcode == KSD_OP_WORK_AREA) {
        gint32 x;
        gint32 y;
        gint32 width;
        gint32 height;
        reply = provider_call(uid, provider_pid, backend, "GetWorkArea", NULL,
            G_VARIANT_TYPE("(iiii)"), KSD_PROVIDER_TIMEOUT_MS, &error);
        if (reply == NULL) {
            provider_error(result, error);
            return;
        }
        g_variant_get(reply, "(iiii)", &x, &y, &width, &height);
        if (width <= 0 || height <= 0
            || (int64_t)x + width > INT32_MAX
            || (int64_t)y + height > INT32_MAX) {
            ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                             "desktop provider returned an invalid work area");
        } else {
            ksd_encode_u32(encoded, (uint32_t)x);
            ksd_encode_u32(encoded + 4u, (uint32_t)y);
            ksd_encode_u32(encoded + 8u, (uint32_t)width);
            ksd_encode_u32(encoded + 12u, (uint32_t)height);
            if (!ksd_result_copy(result, encoded, sizeof(encoded)))
                ksd_result_error(result, KSD_STATUS_INTERNAL, 0u,
                                 "out of memory");
        }
        g_variant_unref(reply);
        return;
    }
    invalid_request(result);
}

static const char *handle_method(uint16_t opcode)
{
    switch (opcode) {
        case KSD_OP_WINDOW_FOCUS: return "FocusWindow";
        case KSD_OP_WINDOW_RAISE: return "RaiseWindow";
        case KSD_OP_WINDOW_LOWER: return "LowerWindow";
        case KSD_OP_WINDOW_CLOSE: return "CloseWindow";
        case KSD_OP_WINDOW_KILL: return "KillWindow";
        default: return NULL;
    }
}

static void execute_window(uid_t uid, pid_t pid, pid_t provider_pid,
                           ksd_backend backend,
                           const ksd_frame *request,
                           ksd_operation_result *result)
{
    GError *error = NULL;
    GVariant *reply = NULL;
    ksd_cursor cursor;
    uint64_t handle;
    const char *method = handle_method(request->opcode);

    if (request->opcode == KSD_OP_WINDOW_LIST) {
        uint32_t include_hidden;
        uint32_t reserved;
        ksd_cursor_init(&cursor, request->payload, request->payload_length);
        if (!ksd_cursor_u32(&cursor, &include_hidden)
            || !ksd_cursor_u32(&cursor, &reserved)
            || include_hidden > 1u || reserved != 0u
            || !ksd_cursor_finished(&cursor)) {
            invalid_request(result);
            return;
        }
        reply = provider_call(uid, provider_pid, backend, "GetWindowList",
            g_variant_new("(b)", include_hidden != 0u),
            G_VARIANT_TYPE("(s)"), KSD_PROVIDER_TIMEOUT_MS, &error);
        string_result(reply, error, result);
        return;
    }
    if (request->opcode == KSD_OP_WINDOW_ACTIVE) {
        if (!parse_empty(request)) {
            invalid_request(result);
            return;
        }
        reply = provider_call(uid, provider_pid, backend,
            "GetActiveWindow", NULL,
            G_VARIANT_TYPE("(s)"), KSD_PROVIDER_TIMEOUT_MS, &error);
        string_result(reply, error, result);
        return;
    }
    if (request->opcode == KSD_OP_WINDOW_QUERY) {
        if (!parse_handle(request, &handle)) {
            invalid_request(result);
            return;
        }
        reply = provider_call(uid, provider_pid, backend, "QueryWindow",
            g_variant_new("(t)", (guint64)handle), G_VARIANT_TYPE("(s)"),
            KSD_PROVIDER_TIMEOUT_MS, &error);
        string_result(reply, error, result);
        return;
    }
    if (method != NULL) {
        if (!parse_handle(request, &handle)) {
            invalid_request(result);
            return;
        }
        reply = provider_call(uid, provider_pid, backend, method,
            g_variant_new("(t)", (guint64)handle), G_VARIANT_TYPE("(b)"),
            KSD_PROVIDER_TIMEOUT_MS, &error);
        boolean_result(reply, error, result);
        return;
    }

    ksd_cursor_init(&cursor, request->payload, request->payload_length);
    if (request->opcode == KSD_OP_WINDOW_MOVE_RESIZE
        || request->opcode == KSD_OP_WINDOW_MOVE_RESIZE_XID) {
        int32_t x;
        int32_t y;
        uint32_t width;
        uint32_t height;
        if (!ksd_cursor_u64(&cursor, &handle) || handle == 0u
            || !ksd_cursor_i32(&cursor, &x) || !ksd_cursor_i32(&cursor, &y)
            || !ksd_cursor_u32(&cursor, &width)
            || !ksd_cursor_u32(&cursor, &height)
            || !ksd_cursor_finished(&cursor)
            || !valid_window_geometry(x, y, width, height)) {
            invalid_request(result);
            return;
        }
        reply = provider_call(uid, provider_pid, backend,
            request->opcode == KSD_OP_WINDOW_MOVE_RESIZE
                ? "MoveResizeWindow" : "MoveResizeWindowByXid",
            g_variant_new("(tiiii)", (guint64)handle, x, y,
                          (gint32)width, (gint32)height),
            G_VARIANT_TYPE("(b)"), KSD_PROVIDER_TIMEOUT_MS, &error);
        boolean_result(reply, error, result);
        return;
    }
    if (request->opcode == KSD_OP_WINDOW_RESERVE) {
        uint64_t cookie;
        int32_t x;
        int32_t y;
        uint32_t ttl;
        uint32_t reserved;
        if (!ksd_cursor_u64(&cursor, &cookie) || cookie == 0u
            || !ksd_cursor_i32(&cursor, &x)
            || !ksd_cursor_i32(&cursor, &y)
            || !ksd_cursor_u32(&cursor, &ttl)
            || !ksd_cursor_u32(&cursor, &reserved)
            || ttl == 0u || ttl > KSD_MAX_RESERVATION_TTL_MS
            || reserved != 0u || !ksd_cursor_finished(&cursor)) {
            invalid_request(result);
            return;
        }
        reply = provider_call(uid, provider_pid, backend, "ReserveWindow",
            g_variant_new("(itiii)", (gint32)pid, (guint64)cookie,
                          x, y, (gint32)ttl),
            G_VARIANT_TYPE("(b)"), KSD_PROVIDER_TIMEOUT_MS, &error);
        boolean_result(reply, error, result);
        return;
    }
    if (request->opcode == KSD_OP_WINDOW_GET_RESERVED) {
        uint64_t cookie;
        if (!ksd_cursor_u64(&cursor, &cookie) || cookie == 0u
            || !ksd_cursor_finished(&cursor)) {
            invalid_request(result);
            return;
        }
        reply = provider_call(uid, provider_pid, backend,
            "GetReservedWindow",
            g_variant_new("(it)", (gint32)pid, (guint64)cookie),
            G_VARIANT_TYPE("(s)"), KSD_PROVIDER_TIMEOUT_MS, &error);
        if (reply == NULL) {
            provider_error(result, error);
            return;
        }
        const gchar *text;
        g_variant_get(reply, "(&s)", &text);
        if (text[0] == '\0') {
            ksd_result_error(result, KSD_STATUS_NOT_FOUND, 0u,
                             "reserved window is not available");
        } else {
            uint64_t reserved_handle;
            if (!parse_decimal_handle((const uint8_t *)text,
                                      (uint32_t)strlen(text),
                                      &reserved_handle)) {
                ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                                 "desktop provider returned an invalid handle");
            } else {
                uint8_t encoded[8];
                ksd_encode_u64(encoded, reserved_handle);
                if (!ksd_result_copy(result, encoded, sizeof(encoded)))
                    ksd_result_error(result, KSD_STATUS_INTERNAL, 0u,
                                     "out of memory");
            }
        }
        g_variant_unref(reply);
        if (error != NULL)
            g_error_free(error);
        return;
    }

    uint32_t value;
    uint32_t reserved;
    if (!ksd_cursor_u64(&cursor, &handle) || handle == 0u
        || !ksd_cursor_u32(&cursor, &value)
        || !ksd_cursor_u32(&cursor, &reserved)
        || reserved != 0u || !ksd_cursor_finished(&cursor)) {
        invalid_request(result);
        return;
    }
    if (request->opcode == KSD_OP_WINDOW_SET_STATE) {
        method = "SetWindowState";
        if (value > 2u)
            method = NULL;
    } else if (request->opcode == KSD_OP_WINDOW_SET_OPACITY) {
        method = "SetWindowOpacity";
        if (value > 255u)
            method = NULL;
    } else if (request->opcode == KSD_OP_WINDOW_SET_ABOVE) {
        method = "SetWindowAbove";
        if (value > 1u)
            method = NULL;
    } else if (request->opcode == KSD_OP_WINDOW_SET_DECORATED) {
        method = "SetWindowDecorated";
        if (value > 1u)
            method = NULL;
    }
    if (method == NULL) {
        invalid_request(result);
        return;
    }
    reply = provider_call(uid, provider_pid, backend, method,
        request->opcode == KSD_OP_WINDOW_SET_ABOVE
            || request->opcode == KSD_OP_WINDOW_SET_DECORATED
            ? g_variant_new("(tb)", (guint64)handle, value != 0u)
            : g_variant_new("(ti)", (guint64)handle, (gint32)value),
        G_VARIANT_TYPE("(b)"), KSD_PROVIDER_TIMEOUT_MS, &error);
    boolean_result(reply, error, result);
}

static void execute_clipboard(uid_t uid, pid_t provider_pid,
                              ksd_backend backend,
                              const ksd_frame *request,
                              ksd_operation_result *result)
{
    GError *error = NULL;
    GVariant *reply;

    if (request->opcode == KSD_OP_CLIPBOARD_MIMETYPES) {
        if (!parse_empty(request)) {
            invalid_request(result);
            return;
        }
        reply = provider_call(uid, provider_pid, backend,
            "GetClipboardMimetypes", NULL,
            G_VARIANT_TYPE("(as)"), KSD_PROVIDER_TIMEOUT_MS, &error);
        if (reply == NULL) {
            provider_error(result, error);
            return;
        }
        gchar **values = NULL;
        g_variant_get(reply, "(^as)", &values);
        size_t count = g_strv_length(values);
        ksd_buffer tail;
        ksd_buffer_init(&tail, KSD_MAX_TEXT_BYTES);
        bool ok = count <= KSD_MAX_MIMETYPES
            && ksd_buffer_u32(&tail, (uint32_t)count)
            && ksd_buffer_u32(&tail, 0u);
        for (size_t index = 0u; ok && index < count; index++) {
            size_t length = strlen(values[index]);
            ok = length != 0u && length <= KSD_MAX_MIMETYPE_BYTES
                && ksd_utf8_valid((const uint8_t *)values[index],
                                  length, false)
                && ksd_buffer_u32(&tail, (uint32_t)length)
                && ksd_buffer_bytes(&tail, values[index], length);
        }
        if (!ok || !buffer_to_result(&tail, result))
            ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                             "clipboard format list is invalid or too large");
        ksd_buffer_clear(&tail);
        g_strfreev(values);
        g_variant_unref(reply);
        if (error != NULL)
            g_error_free(error);
        return;
    }
    if (request->opcode == KSD_OP_CLIPBOARD_TEXT) {
        if (!parse_empty(request)) {
            invalid_request(result);
            return;
        }
        reply = provider_call(uid, provider_pid, backend,
            "GetClipboardText", NULL,
            G_VARIANT_TYPE("(s)"), KSD_PROVIDER_TIMEOUT_MS, &error);
        string_result(reply, error, result);
        return;
    }
    if (request->opcode == KSD_OP_CLIPBOARD_CONTENT) {
        ksd_cursor cursor;
        uint32_t length;
        const uint8_t *bytes;
        ksd_cursor_init(&cursor, request->payload, request->payload_length);
        if (!ksd_cursor_u32(&cursor, &length) || length == 0u
            || length > KSD_MAX_MIMETYPE_BYTES
            || !ksd_cursor_bytes(&cursor, length, &bytes)
            || !ksd_cursor_finished(&cursor)
            || !ksd_utf8_valid(bytes, length, false)) {
            invalid_request(result);
            return;
        }
        char mimetype[KSD_MAX_MIMETYPE_BYTES + 1u];
        memcpy(mimetype, bytes, length);
        mimetype[length] = '\0';
        reply = provider_call(uid, provider_pid, backend,
            "GetClipboardContent",
            g_variant_new("(s)", mimetype), G_VARIANT_TYPE("(ay)"),
            KSD_PROVIDER_TIMEOUT_MS, &error);
        if (reply == NULL) {
            provider_error(result, error);
            return;
        }
        GVariant *byte_array = NULL;
        const uint8_t *data;
        gsize data_length;
        g_variant_get(reply, "(@ay)", &byte_array);
        data = g_variant_get_fixed_array(byte_array, &data_length,
                                         sizeof(uint8_t));
        ksd_buffer tail;
        ksd_buffer_init(&tail, KSD_MAX_TEXT_BYTES + 4u);
        if (data_length > KSD_MAX_TEXT_BYTES
            || !ksd_buffer_u32(&tail, (uint32_t)data_length)
            || !ksd_buffer_bytes(&tail, data, (size_t)data_length)
            || !buffer_to_result(&tail, result))
            ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                             "clipboard content is too large");
        ksd_buffer_clear(&tail);
        g_variant_unref(byte_array);
        g_variant_unref(reply);
        if (error != NULL)
            g_error_free(error);
        return;
    }
    if (request->opcode == KSD_OP_CLIPBOARD_SET_CONTENT) {
        static const uint8_t empty = 0u;
        ksd_cursor cursor;
        uint32_t length;
        uint32_t content_length;
        const uint8_t *bytes;
        const uint8_t *content;
        ksd_cursor_init(&cursor, request->payload, request->payload_length);
        if (!ksd_cursor_u32(&cursor, &length) || length == 0u
            || length > KSD_MAX_MIMETYPE_BYTES
            || !ksd_cursor_bytes(&cursor, length, &bytes)
            || !ksd_utf8_valid(bytes, length, false)
            || !ksd_cursor_u32(&cursor, &content_length)
            || content_length > KSD_MAX_CLIPBOARD_WRITE_BYTES
            || !ksd_cursor_bytes(&cursor, content_length, &content)
            || !ksd_cursor_finished(&cursor)) {
            invalid_request(result);
            return;
        }
        char mimetype[KSD_MAX_MIMETYPE_BYTES + 1u];
        memcpy(mimetype, bytes, length);
        mimetype[length] = '\0';
        if (strcmp(mimetype, KSD_CLIPBOARD_TEXT_MIMETYPE) == 0
            && !ksd_utf8_valid(content, content_length, false)) {
            invalid_request(result);
            return;
        }
        reply = provider_call(uid, provider_pid, backend,
            "SetClipboardContent",
            g_variant_new("(s@ay)", mimetype,
                g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,
                    content_length == 0u ? &empty : content,
                    (gsize)content_length, sizeof(uint8_t))),
            G_VARIANT_TYPE("(b)"), KSD_PROVIDER_TIMEOUT_MS, &error);
        boolean_result(reply, error, result);
        return;
    }
    invalid_request(result);
}

static bool supported_pointer_button(uint32_t button)
{
    return button == 1u || button == 2u || button == 3u
        || button == 8u || button == 9u;
}

static void execute_pointer(uid_t uid, pid_t provider_pid,
                            ksd_backend backend,
                            const ksd_frame *request,
                            ksd_operation_result *result)
{
    GError *error = NULL;
    GVariant *reply = NULL;
    ksd_cursor cursor;
    ksd_cursor_init(&cursor, request->payload, request->payload_length);

    if (request->opcode == KSD_OP_MOUSE_MOVE_ABSOLUTE
        || request->opcode == KSD_OP_MOUSE_MOVE_RELATIVE) {
        int32_t x;
        int32_t y;
        if (!ksd_cursor_i32(&cursor, &x) || !ksd_cursor_i32(&cursor, &y)
            || !ksd_cursor_finished(&cursor)) {
            invalid_request(result);
            return;
        }
        reply = provider_call(uid, provider_pid, backend,
            request->opcode == KSD_OP_MOUSE_MOVE_ABSOLUTE
                ? "SendMouseMoveAbsolute" : "SendMouseMoveRelative",
            g_variant_new("(ii)", x, y), G_VARIANT_TYPE("(b)"),
            KSD_PROVIDER_TIMEOUT_MS, &error);
    } else if (request->opcode == KSD_OP_MOUSE_BUTTON) {
        uint32_t button;
        uint32_t pressed;
        if (!ksd_cursor_u32(&cursor, &button)
            || !ksd_cursor_u32(&cursor, &pressed)
            || !ksd_cursor_finished(&cursor)
            || !supported_pointer_button(button) || pressed > 1u) {
            invalid_request(result);
            return;
        }
        reply = provider_call(uid, provider_pid, backend, "SendMouseButton",
            g_variant_new("(ub)", button, pressed != 0u),
            G_VARIANT_TYPE("(b)"), KSD_PROVIDER_TIMEOUT_MS, &error);
    } else if (request->opcode == KSD_OP_MOUSE_SCROLL) {
        int32_t delta;
        uint32_t vertical;
        if (!ksd_cursor_i32(&cursor, &delta)
            || !ksd_cursor_u32(&cursor, &vertical)
            || !ksd_cursor_finished(&cursor) || delta == 0
            || delta < -KSD_MAX_MOUSE_SCROLL_DELTA
            || delta > KSD_MAX_MOUSE_SCROLL_DELTA || vertical > 1u) {
            invalid_request(result);
            return;
        }
        reply = provider_call(uid, provider_pid, backend, "SendMouseScroll",
            g_variant_new("(ib)", delta, vertical != 0u),
            G_VARIANT_TYPE("(b)"), KSD_PROVIDER_TIMEOUT_MS, &error);
    } else {
        invalid_request(result);
        return;
    }
    boolean_result(reply, error, result);
}

#ifdef KSD_AUTHORITY_TESTING
int ksd_provider_test_capture_memfd(uint32_t width, uint32_t height,
                                    const uint8_t *data, size_t length)
{
    return sealed_capture_memfd(width, height, data, length);
}
#endif

void ksd_provider_execute(uid_t uid, pid_t pid, pid_t provider_pid,
                          ksd_backend backend,
                          const ksd_frame *request,
                          ksd_operation_result *result)
{
    if (result == NULL)
        return;
    ksd_result_init(result);
    if (request == NULL
        || (backend != KSD_BACKEND_GNOME
            && backend != KSD_BACKEND_CINNAMON)) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                         "desktop provider is unavailable");
        return;
    }
    if (request->opcode == KSD_OP_CAPTURE_AREA
        || request->opcode == KSD_OP_CAPTURE_WINDOW) {
        execute_capture(uid, provider_pid, backend, request, result);
        return;
    }
    if ((request->opcode >= KSD_OP_WINDOW_LIST
         && request->opcode <= KSD_OP_WINDOW_WATCH)
        || request->opcode == KSD_OP_WINDOW_QUERY
        || (request->opcode >= KSD_OP_WINDOW_FOCUS
            && request->opcode <= KSD_OP_WINDOW_GET_RESERVED)) {
        execute_window(uid, pid, provider_pid, backend, request, result);
        return;
    }
    if ((request->opcode >= KSD_OP_CLIPBOARD_MIMETYPES
         && request->opcode <= KSD_OP_CLIPBOARD_TEXT)
        || request->opcode == KSD_OP_CLIPBOARD_SET_CONTENT) {
        execute_clipboard(uid, provider_pid, backend, request, result);
        return;
    }
    if (request->opcode >= KSD_OP_MOUSE_MOVE_ABSOLUTE
        && request->opcode <= KSD_OP_MOUSE_SCROLL) {
        execute_pointer(uid, provider_pid, backend, request, result);
        return;
    }
    if (request->opcode == KSD_OP_CURSOR_POSITION
        || request->opcode == KSD_OP_WORK_AREA) {
        execute_query(uid, provider_pid, backend, request, result);
        return;
    }
    invalid_request(result);
}

static uint16_t event_kind(const char *name)
{
    if (strcmp(name, "create") == 0) return KSD_WINDOW_EVENT_CREATE;
    if (strcmp(name, "close") == 0) return KSD_WINDOW_EVENT_CLOSE;
    if (strcmp(name, "active") == 0) return KSD_WINDOW_EVENT_ACTIVE;
    if (strcmp(name, "title") == 0) return KSD_WINDOW_EVENT_TITLE;
    if (strcmp(name, "minimize") == 0) return KSD_WINDOW_EVENT_MINIMIZE;
    if (strcmp(name, "restore") == 0) return KSD_WINDOW_EVENT_RESTORE;
    if (strcmp(name, "move") == 0) return KSD_WINDOW_EVENT_MOVE;
    if (strcmp(name, "active-state") == 0)
        return KSD_WINDOW_EVENT_ACTIVE_STATE;
    return 0u;
}

static void window_event(GDBusConnection *connection, const gchar *sender,
                         const gchar *path, const gchar *interface,
                         const gchar *signal, GVariant *parameters,
                         gpointer user_data)
{
    watch_state *watch = user_data;
    const gchar *name;
    const gchar *json;
    ksd_buffer payload;
    (void)connection;
    (void)sender;
    (void)path;
    (void)interface;
    (void)signal;
    g_variant_get(parameters, "(&s&s)", &name, &json);
    uint16_t kind = event_kind(name);
    size_t length = strlen(json);
    if (kind == 0u || length > KSD_MAX_TEXT_BYTES
        || !ksd_utf8_valid((const uint8_t *)json, length, false)) {
        watch->failed = true;
        return;
    }
    ksd_buffer_init(&payload, KSD_MAX_TEXT_BYTES + 8u);
    bool ok = ksd_buffer_u16(&payload, kind)
        && ksd_buffer_u16(&payload, 0u)
        && ksd_buffer_u32(&payload, (uint32_t)length)
        && ksd_buffer_bytes(&payload, json, length)
        && watch->emit(KSD_OP_WINDOW_EVENT, payload.data,
                       (uint32_t)payload.length, watch->user_data);
    ksd_buffer_clear(&payload);
    if (!ok)
        watch->failed = true;
}

static void clipboard_event(GDBusConnection *connection, const gchar *sender,
                            const gchar *path, const gchar *interface,
                            const gchar *signal, GVariant *parameters,
                            gpointer user_data)
{
    watch_state *watch = user_data;
    const gchar *text;
    gchar **mimetypes = NULL;
    ksd_buffer payload;
    (void)connection;
    (void)sender;
    (void)path;
    (void)interface;
    (void)signal;
    g_variant_get(parameters, "(&s^as)", &text, &mimetypes);
    size_t text_length = strlen(text);
    size_t count = g_strv_length(mimetypes);
    ksd_buffer_init(&payload, KSD_MAX_TEXT_BYTES);
    bool ok = text_length <= KSD_MAX_TEXT_BYTES
        && count <= KSD_MAX_MIMETYPES
        && ksd_utf8_valid((const uint8_t *)text, text_length, false)
        && ksd_buffer_u32(&payload, (uint32_t)text_length)
        && ksd_buffer_u32(&payload, (uint32_t)count)
        && ksd_buffer_bytes(&payload, text, text_length);
    for (size_t index = 0u; ok && index < count; index++) {
        size_t length = strlen(mimetypes[index]);
        ok = length != 0u && length <= KSD_MAX_MIMETYPE_BYTES
            && ksd_utf8_valid((const uint8_t *)mimetypes[index],
                              length, false)
            && ksd_buffer_u32(&payload, (uint32_t)length)
            && ksd_buffer_bytes(&payload, mimetypes[index], length);
    }
    if (ok)
        ok = watch->emit(KSD_OP_CLIPBOARD_EVENT, payload.data,
                         (uint32_t)payload.length, watch->user_data);
    ksd_buffer_clear(&payload);
    g_strfreev(mimetypes);
    if (!ok)
        watch->failed = true;
}

static gboolean watch_wakeup(gpointer user_data)
{
    (void)user_data;
    return G_SOURCE_CONTINUE;
}

int ksd_provider_watch(uid_t uid, pid_t provider_pid, ksd_backend backend,
                       bool clipboard,
                       ksd_provider_event_fn emit,
                       ksd_provider_cancel_fn cancelled,
                       void *user_data, char *diagnostic,
                       size_t diagnostic_capacity)
{
    GError *error = NULL;
    GMainContext *context = NULL;
    GDBusConnection *connection = NULL;
    GSource *timer = NULL;
    guint subscription = 0u;
    watch_state watch = {
        .emit = emit,
        .user_data = user_data,
    };
    int result = -1;

    if (emit == NULL || cancelled == NULL || diagnostic == NULL
        || diagnostic_capacity == 0u)
        return -1;
    diagnostic[0] = '\0';
    connection = provider_connection_get(uid, provider_pid, backend, &error);
    if (connection == NULL)
        goto done;
    context = g_main_context_new();
    if (context == NULL)
        goto done;
    g_main_context_push_thread_default(context);
    subscription = g_dbus_connection_signal_subscribe(connection, NULL,
        KSD_PROVIDER_INTERFACE,
        clipboard ? "ClipboardChanged" : "WindowEvent",
        KSD_PROVIDER_OBJECT_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
        clipboard ? clipboard_event : window_event, &watch, NULL);
    timer = g_timeout_source_new(KSD_PROVIDER_WATCH_POLL_MS);
    if (subscription == 0u || timer == NULL)
        goto popped;
    g_source_set_callback(timer, watch_wakeup, NULL, NULL);
    g_source_attach(timer, context);
    while (!watch.failed && !cancelled(user_data)) {
        (void)g_main_context_iteration(context, TRUE);
        if (g_dbus_connection_is_closed(connection)) {
            watch.failed = true;
            break;
        }
    }
    result = watch.failed ? -1 : 0;

popped:
    if (timer != NULL) {
        g_source_destroy(timer);
        g_source_unref(timer);
    }
    if (subscription != 0u)
        g_dbus_connection_signal_unsubscribe(connection, subscription);
    g_main_context_pop_thread_default(context);
done:
    if (result != 0) {
        const char *message = error == NULL
            ? "desktop provider event stream ended" : error->message;
        size_t length = strlen(message);
        if (length >= diagnostic_capacity)
            length = diagnostic_capacity - 1u;
        memcpy(diagnostic, message, length);
        diagnostic[length] = '\0';
    }
    if (connection != NULL) {
        if (g_dbus_connection_is_closed(connection))
            provider_connection_invalidate(uid, backend, connection);
        g_object_unref(connection);
    }
    if (context != NULL)
        g_main_context_unref(context);
    if (error != NULL)
        g_error_free(error);
    return result;
}
