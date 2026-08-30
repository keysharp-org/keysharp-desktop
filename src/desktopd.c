#include "authority_client.h"
#include "capture.h"
#include "common.h"
#include "keysharp_desktop/protocol.h"
#include "provider.h"
#include "roles.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define KSD_MAX_CLIENTS 32u
#define KSD_QUEUE_CAPACITY 32u
#define KSD_DEFAULT_IDLE_SECONDS 60u
#define KSD_REVOCATION_POLL_MILLISECONDS 250

typedef struct client_context client_context;

typedef enum job_kind {
    JOB_AREA,
    JOB_WINDOW,
} job_kind;

typedef struct capture_job {
    struct capture_job *next;
    pthread_cond_t completed;
    bool done;
    job_kind kind;
    int output_fd;
    ksd_backend backend;
    int x;
    int y;
    int width;
    int height;
    bool include_decoration;
    char handle[256];
} capture_job;

typedef struct service_state {
    pthread_mutex_t mutex;
    pthread_cond_t queued;
    capture_job *queue_head;
    capture_job *queue_tail;
    size_t queue_count;
    size_t clients;
    client_context *client_head;
    uint64_t revoke_generation;
    bool revoke_generation_valid;
    bool stopping;
    time_t idle_since;
} service_state;

struct client_context {
    service_state *service;
    int descriptor;
    struct ucred credentials;
    uint64_t start_time;
    ksd_backend backend;
    uint32_t granted_capabilities;
    client_context *next;
};

static unsigned idle_timeout_seconds(void)
{
    const char *configured = getenv("KEYSHARP_DESKTOP_IDLE_SECONDS");
    char *end = NULL;
    unsigned long value;

    if (configured == NULL || configured[0] == '\0')
        return KSD_DEFAULT_IDLE_SECONDS;
    errno = 0;
    value = strtoul(configured, &end, 10);
    return errno == 0 && end != configured && *end == '\0' && value <= 3600u
        ? (unsigned)value : KSD_DEFAULT_IDLE_SECONDS;
}

static bool running_unprivileged(void)
{
    return getuid() != 0 && getuid() == geteuid() && getgid() == getegid();
}

static int systemd_listener(void)
{
    const char *listen_pid = getenv("LISTEN_PID");
    const char *listen_fds = getenv("LISTEN_FDS");
    if (listen_pid != NULL && listen_fds != NULL
        && (pid_t)strtol(listen_pid, NULL, 10) == getpid()
        && strtol(listen_fds, NULL, 10) >= 1)
        return 3;
    return -1;
}

static int create_listener(char *path, size_t path_size)
{
    const char *configured = getenv("KEYSHARP_DESKTOP_SOCKET");
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    struct sockaddr_un address;
    int descriptor;

    if (configured != NULL && configured[0] != '\0')
        (void)snprintf(path, path_size, "%s", configured);
    else {
        char fallback[64];
        if (runtime == NULL || runtime[0] == '\0') {
            (void)snprintf(fallback, sizeof(fallback), "/run/user/%lu", (unsigned long)getuid());
            runtime = fallback;
        }
        (void)snprintf(path, path_size, "%s/%s", runtime, KSD_DEFAULT_SOCKET_SUFFIX);
    }
    if (strlen(path) >= sizeof(address.sun_path) || ksd_make_parent_directories(path, 0700) != 0)
        return -1;

    descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (descriptor < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    (void)snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    (void)unlink(path);
    if (bind(descriptor, (const struct sockaddr *)&address, sizeof(address)) != 0
        || chmod(path, 0600) != 0 || listen(descriptor, (int)KSD_MAX_CLIENTS) != 0) {
        close(descriptor);
        return -1;
    }
    return descriptor;
}

static ssize_t read_line(int descriptor, char *buffer, size_t capacity)
{
    size_t used = 0u;

    while (used + 1u < capacity) {
        char character;
        ssize_t count = read(descriptor, &character, 1u);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return used == 0u ? count : -1;
        if (character == '\n') {
            buffer[used] = '\0';
            return (ssize_t)used;
        }
        if ((unsigned char)character < 0x20u || character == 0x7f)
            return -1;
        buffer[used++] = character;
    }
    return -1;
}

static bool write_handshake_error(int descriptor, const char *code, const char *message)
{
    char line[512];
    int length = snprintf(line, sizeof(line), KSD_HANDSHAKE_PREFIX " ERROR %s %s\n", code, message);
    return length > 0 && (size_t)length < sizeof(line)
        && ksd_write_all(descriptor, line, (size_t)length);
}

static bool perform_handshake(client_context *client)
{
    char line[512];
    char prefix[32];
    char hello[16];
    char backend_name[32];
    char mode[16];
    char capability_text[9];
    char extra;
    unsigned capabilities;
    bool automatic;
    bool interactive;
    ksd_backend requested;
    uint32_t granted = 0u;

    if (read_line(client->descriptor, line, sizeof(line)) <= 0
        || sscanf(line, "%31s %15s %31s %8[0-9A-Fa-f] %15s %c",
                  prefix, hello, backend_name, capability_text, mode, &extra) != 5) {
        (void)write_handshake_error(client->descriptor, "protocol", "invalid handshake");
        return false;
    }
    if (strcmp(prefix, KSD_HANDSHAKE_PREFIX) != 0) {
        (void)write_handshake_error(client->descriptor, "protocol",
            strncmp(prefix, "KSDP/", sizeof("KSDP/") - 1u) == 0
                ? "incompatible protocol version" : "invalid handshake");
        return false;
    }
    if (strcmp(hello, "HELLO") != 0
        || strlen(capability_text) != 8u
        || sscanf(capability_text, "%x%c", &capabilities, &extra) != 1) {
        (void)write_handshake_error(client->descriptor, "protocol", "invalid handshake");
        return false;
    }
    if ((capabilities & ~KSD_CAP_ALL) != 0u) {
        (void)write_handshake_error(client->descriptor, "protocol", "unknown capability bits");
        return false;
    }
    automatic = strcmp(backend_name, "auto") == 0;
    requested = ksd_backend_parse(backend_name);
    if ((!automatic && requested == KSD_BACKEND_NONE) || (int)requested < 0) {
        (void)write_handshake_error(client->descriptor, "protocol", "unknown backend");
        return false;
    }
    if (strcmp(mode, "request") == 0)
        interactive = true;
    else if (strcmp(mode, "check") == 0)
        interactive = false;
    else {
        (void)write_handshake_error(client->descriptor, "protocol", "unknown authorization mode");
        return false;
    }

    if (capabilities != 0u) {
        int authorization = -1;
        bool stable = false;
        for (unsigned attempt = 0u; attempt < 3u; attempt++) {
            uint64_t before, after;
            if (ksd_revoke_generation_read(getuid(), &before) != 0)
                break;
            authorization = ksd_authority_check(client->descriptor, capabilities,
                                                interactive, &granted);
            interactive = false;
            if (ksd_revoke_generation_read(getuid(), &after) != 0)
                break;
            if (before == after) {
                pthread_mutex_lock(&client->service->mutex);
                if (client->service->revoke_generation_valid
                    && client->service->revoke_generation == after) {
                    if (authorization == 0)
                        client->granted_capabilities = granted;
                    stable = true;
                }
                pthread_mutex_unlock(&client->service->mutex);
                if (stable)
                    break;
            }
        }
        if (!stable)
            authorization = -1;
        if (authorization > 0) {
            (void)write_handshake_error(client->descriptor, "denied", "permission is not granted");
            return false;
        }
        if (authorization < 0) {
            (void)write_handshake_error(client->descriptor, "internal", "authorization service unavailable");
            return false;
        }
    }

    client->backend = ksd_backend_resolve(requested);
    if (!automatic && client->backend == KSD_BACKEND_NONE) {
        (void)write_handshake_error(client->descriptor, "unsupported", "requested backend unavailable");
        return false;
    }

    int length = snprintf(line, sizeof(line), KSD_HANDSHAKE_PREFIX
        " READY %s %08x capture-area,capture-window,window-query,window-control,clipboard-read,events,authorize,version\n",
        ksd_backend_name(client->backend), granted);
    return length > 0 && (size_t)length < sizeof(line)
        && ksd_write_all(client->descriptor, line, (size_t)length);
}

static bool enqueue_and_wait(client_context *client, capture_job *job)
{
    service_state *service = client->service;

    pthread_cond_init(&job->completed, NULL);
    job->next = NULL;
    job->done = false;
    job->output_fd = client->descriptor;
    job->backend = client->backend;

    pthread_mutex_lock(&service->mutex);
    if (service->stopping || service->queue_count >= KSD_QUEUE_CAPACITY) {
        pthread_mutex_unlock(&service->mutex);
        pthread_cond_destroy(&job->completed);
        return ksd_write_error_response(client->descriptor, "capture queue is busy");
    }
    if (service->queue_tail == NULL)
        service->queue_head = job;
    else
        service->queue_tail->next = job;
    service->queue_tail = job;
    service->queue_count++;
    pthread_cond_signal(&service->queued);
    while (!job->done)
        pthread_cond_wait(&job->completed, &service->mutex);
    pthread_mutex_unlock(&service->mutex);
    pthread_cond_destroy(&job->completed);
    return true;
}

static bool handle_command(client_context *client, char *line)
{
    capture_job job;
    char command[32];
    char encoded[1024];
    char extra;
    uint32_t granted_capabilities;
    uint64_t handle;
    int x, y, width, height, value;

    memset(&job, 0, sizeof(job));
    if (strcmp(line, "ping") == 0) {
        uint8_t status = KSD_RESPONSE_OK;
        return ksd_write_all(client->descriptor, &status, sizeof(status));
    }
    if (strcmp(line, "quit") == 0)
        return false;
    if (strcmp(line, "version") == 0) {
        static const char version[] = KSD_PROTOCOL_LABEL " " KSD_PROTOCOL_VERSION;
        uint8_t status = KSD_RESPONSE_OK;
        uint32_t length = (uint32_t)(sizeof(version) - 1u);
        return ksd_write_all(client->descriptor, &status, sizeof(status))
            && ksd_write_all(client->descriptor, &length, sizeof(length))
            && ksd_write_all(client->descriptor, version, length);
    }
    pthread_mutex_lock(&client->service->mutex);
    granted_capabilities = client->granted_capabilities;
    pthread_mutex_unlock(&client->service->mutex);
    if (sscanf(line, "%31s", command) != 1)
        return ksd_write_error_response(client->descriptor, "empty command");
    if (strcmp(command, "area") == 0) {
        if ((granted_capabilities & KSD_CAP_SCREEN_CAPTURE) == 0u)
            return ksd_write_error_response(client->descriptor,
                "screen-capture capability is not granted");
        if (sscanf(line, "area %d %d %d %d %c",
                   &job.x, &job.y, &job.width, &job.height, &extra) != 4)
            return ksd_write_error_response(client->descriptor, "bad area coordinates");
        job.kind = JOB_AREA;
        return enqueue_and_wait(client, &job);
    }
    if (strcmp(command, "window") == 0) {
        int decorated = 1;
        if ((granted_capabilities & KSD_CAP_SCREEN_CAPTURE) == 0u)
            return ksd_write_error_response(client->descriptor,
                "screen-capture capability is not granted");
        int fields = sscanf(line, "window %255s %d %c", job.handle, &decorated, &extra);
        if (fields < 1 || fields > 2 || (decorated != 0 && decorated != 1))
            return ksd_write_error_response(client->descriptor, "bad window request");
        job.kind = JOB_WINDOW;
        job.include_decoration = decorated != 0;
        return enqueue_and_wait(client, &job);
    }
    if (strcmp(command, "window-list") == 0) {
        if ((granted_capabilities & KSD_CAP_WINDOW_MONITORING) == 0u)
            return ksd_write_error_response(client->descriptor,
                "window-monitoring capability is not granted");
        if (sscanf(line, "window-list %d %c", &value, &extra) != 1
            || (value != 0 && value != 1))
            return ksd_write_error_response(client->descriptor,
                "bad window-list request");
        return ksd_provider_window_list(client->backend, client->descriptor,
                                        value != 0);
    }
    if (strcmp(command, "active-window") == 0) {
        if ((granted_capabilities & KSD_CAP_WINDOW_MONITORING) == 0u)
            return ksd_write_error_response(client->descriptor,
                "window-monitoring capability is not granted");
        if (strcmp(line, "active-window") != 0)
            return ksd_write_error_response(client->descriptor,
                "bad active-window request");
        return ksd_provider_active_window(client->backend, client->descriptor);
    }
    if (strcmp(command, "watch-window") == 0) {
        if ((granted_capabilities & KSD_CAP_WINDOW_MONITORING) == 0u)
            return ksd_write_error_response(client->descriptor,
                "window-monitoring capability is not granted");
        if (strcmp(line, "watch-window") != 0)
            return ksd_write_error_response(client->descriptor,
                "bad watch-window request");
        return ksd_provider_watch(client->backend, client->descriptor, false);
    }
    if (strncmp(command, "window-", 7u) == 0) {
        if ((granted_capabilities & KSD_CAP_WINDOW_CONTROL) == 0u)
            return ksd_write_error_response(client->descriptor,
                "window-control capability is not granted");
        if (strcmp(command, "window-focus") == 0
            || strcmp(command, "window-raise") == 0
            || strcmp(command, "window-lower") == 0
            || strcmp(command, "window-close") == 0
            || strcmp(command, "window-kill") == 0) {
            const char *method = strcmp(command, "window-focus") == 0 ? "FocusWindow"
                : strcmp(command, "window-raise") == 0 ? "RaiseWindow"
                : strcmp(command, "window-lower") == 0 ? "LowerWindow"
                : strcmp(command, "window-close") == 0 ? "CloseWindow"
                : "KillWindow";
            if (sscanf(line, "%*31s %" SCNu64 " %c", &handle, &extra) != 1)
                return ksd_write_error_response(client->descriptor,
                    "bad window-control request");
            return ksd_provider_window_handle_command(client->backend,
                client->descriptor, method, handle);
        }
        if (strcmp(command, "window-move-resize") == 0
            || strcmp(command, "window-move-resize-xid") == 0) {
            if (sscanf(line, "%*31s %" SCNu64 " %d %d %d %d %c",
                       &handle, &x, &y, &width, &height, &extra) != 5)
                return ksd_write_error_response(client->descriptor,
                    "bad window move/resize request");
            return ksd_provider_window_move_resize(client->backend,
                client->descriptor,
                strcmp(command, "window-move-resize") == 0
                    ? "MoveResizeWindow" : "MoveResizeWindowByXid",
                handle, x, y, width, height);
        }
        if (strcmp(command, "window-state") == 0
            || strcmp(command, "window-opacity") == 0) {
            if (sscanf(line, "%*31s %" SCNu64 " %d %c",
                       &handle, &value, &extra) != 2)
                return ksd_write_error_response(client->descriptor,
                    "bad window integer request");
            return ksd_provider_window_integer_command(client->backend,
                client->descriptor,
                strcmp(command, "window-state") == 0
                    ? "SetWindowState" : "SetWindowOpacity",
                handle, value);
        }
        if (strcmp(command, "window-above") == 0
            || strcmp(command, "window-decorated") == 0) {
            if (sscanf(line, "%*31s %" SCNu64 " %d %c",
                       &handle, &value, &extra) != 2
                || (value != 0 && value != 1))
                return ksd_write_error_response(client->descriptor,
                    "bad window boolean request");
            return ksd_provider_window_boolean_command(client->backend,
                client->descriptor,
                strcmp(command, "window-above") == 0
                    ? "SetWindowAbove" : "SetWindowDecorated",
                handle, value != 0);
        }
    }
    if (strcmp(command, "clipboard-mimetypes") == 0
        || strcmp(command, "clipboard-text") == 0
        || strcmp(command, "clipboard-content") == 0
        || strcmp(command, "watch-clipboard") == 0) {
        if ((granted_capabilities & KSD_CAP_CLIPBOARD_MONITORING) == 0u)
            return ksd_write_error_response(client->descriptor,
                "clipboard-monitoring capability is not granted");
        if (strcmp(command, "clipboard-mimetypes") == 0) {
            if (strcmp(line, "clipboard-mimetypes") != 0)
                return ksd_write_error_response(client->descriptor,
                    "bad clipboard-mimetypes request");
            return ksd_provider_clipboard_mimetypes(client->backend,
                                                     client->descriptor);
        }
        if (strcmp(command, "clipboard-text") == 0) {
            if (strcmp(line, "clipboard-text") != 0)
                return ksd_write_error_response(client->descriptor,
                    "bad clipboard-text request");
            return ksd_provider_clipboard_text(client->backend,
                                                client->descriptor);
        }
        if (strcmp(command, "watch-clipboard") == 0) {
            if (strcmp(line, "watch-clipboard") != 0)
                return ksd_write_error_response(client->descriptor,
                    "bad watch-clipboard request");
            return ksd_provider_watch(client->backend, client->descriptor, true);
        }
        if (sscanf(line, "clipboard-content %1023s %c", encoded, &extra) != 1)
            return ksd_write_error_response(client->descriptor,
                "bad clipboard-content request");
        size_t encoded_length = strlen(encoded);
        if ((encoded_length & 1u) != 0u)
            return ksd_write_error_response(client->descriptor,
                "bad clipboard mimetype encoding");
        char *mimetype = malloc(encoded_length / 2u + 1u);
        if (mimetype == NULL)
            return ksd_write_error_response(client->descriptor, "out of memory");
        bool valid = true;
        for (size_t index = 0u; index < encoded_length; index += 2u) {
            unsigned high, low;
            char first = encoded[index];
            char second = encoded[index + 1u];
            high = first >= '0' && first <= '9' ? (unsigned)(first - '0')
                : first >= 'a' && first <= 'f' ? (unsigned)(first - 'a' + 10)
                : first >= 'A' && first <= 'F' ? (unsigned)(first - 'A' + 10)
                : 16u;
            low = second >= '0' && second <= '9' ? (unsigned)(second - '0')
                : second >= 'a' && second <= 'f' ? (unsigned)(second - 'a' + 10)
                : second >= 'A' && second <= 'F' ? (unsigned)(second - 'A' + 10)
                : 16u;
            if (high > 15u || low > 15u || (high == 0u && low == 0u)) {
                valid = false;
                break;
            }
            mimetype[index / 2u] = (char)((high << 4u) | low);
        }
        mimetype[encoded_length / 2u] = '\0';
        bool result = valid
            ? ksd_provider_clipboard_content(client->backend,
                client->descriptor, mimetype)
            : ksd_write_error_response(client->descriptor,
                "bad clipboard mimetype encoding");
        free(mimetype);
        return result;
    }
    return ksd_write_error_response(client->descriptor, "unknown command");
}

static void *capture_worker(void *argument)
{
    service_state *service = argument;

    for (;;) {
        capture_job *job;
        pthread_mutex_lock(&service->mutex);
        while (!service->stopping && service->queue_head == NULL)
            pthread_cond_wait(&service->queued, &service->mutex);
        if (service->stopping && service->queue_head == NULL) {
            pthread_mutex_unlock(&service->mutex);
            break;
        }
        job = service->queue_head;
        service->queue_head = job->next;
        if (service->queue_head == NULL)
            service->queue_tail = NULL;
        service->queue_count--;
        pthread_mutex_unlock(&service->mutex);

        if (job->kind == JOB_AREA)
            (void)ksd_capture_area(job->backend, job->output_fd,
                                   job->x, job->y, job->width, job->height);
        else
            (void)ksd_capture_window(job->backend, job->output_fd,
                                     job->handle, job->include_decoration);

        pthread_mutex_lock(&service->mutex);
        job->done = true;
        pthread_cond_signal(&job->completed);
        pthread_mutex_unlock(&service->mutex);
    }
    return NULL;
}

static void *client_worker(void *argument)
{
    client_context *client = argument;
    char line[1024];
    struct timeval no_receive_timeout = { 0 };

    if (perform_handshake(client)
        && setsockopt(client->descriptor, SOL_SOCKET, SO_RCVTIMEO,
                      &no_receive_timeout, sizeof(no_receive_timeout)) == 0)
        while (read_line(client->descriptor, line, sizeof(line)) > 0)
            if (!handle_command(client, line))
                break;

    pthread_mutex_lock(&client->service->mutex);
    client_context **link = &client->service->client_head;
    while (*link != NULL && *link != client)
        link = &(*link)->next;
    if (*link == client)
        *link = client->next;
    client->service->clients--;
    if (client->service->clients == 0u && client->service->queue_count == 0u)
        client->service->idle_since = time(NULL);
    pthread_mutex_unlock(&client->service->mutex);
    close(client->descriptor);
    free(client);
    return NULL;
}

static void refresh_revocations(service_state *service)
{
    uint64_t generation;
    bool readable = ksd_revoke_generation_read(getuid(), &generation) == 0;
    bool invalidate = false;

    pthread_mutex_lock(&service->mutex);
    if (!readable) {
        invalidate = service->revoke_generation_valid;
        service->revoke_generation_valid = false;
    } else if (!service->revoke_generation_valid) {
        service->revoke_generation = generation;
        service->revoke_generation_valid = true;
    } else if (service->revoke_generation != generation) {
        service->revoke_generation = generation;
        invalidate = true;
    }
    if (invalidate)
        for (client_context *client = service->client_head; client != NULL; client = client->next)
            if (client->granted_capabilities != 0u) {
                client->granted_capabilities = 0u;
                (void)shutdown(client->descriptor, SHUT_RDWR);
            }
    pthread_mutex_unlock(&service->mutex);
}

static bool is_idle(service_state *service, unsigned timeout)
{
    bool idle;
    pthread_mutex_lock(&service->mutex);
    idle = service->clients == 0u && service->queue_count == 0u
        && timeout != 0u && time(NULL) - service->idle_since >= (time_t)timeout;
    pthread_mutex_unlock(&service->mutex);
    return idle;
}

int ksd_serve_main(int argc, char **argv)
{
    char socket_path[4096] = "";
    service_state service = {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .queued = PTHREAD_COND_INITIALIZER,
        .idle_since = 0,
    };
    pthread_t capture_thread;
    int listener;
    bool own_socket = false;
    unsigned idle_timeout;

    (void)argv;

    if (argc != 1) {
        fprintf(stderr, "usage: keysharp-desktop serve\n");
        return 2;
    }
    if (!running_unprivileged()) {
        fputs("keysharp-desktop serve: refusing to run with elevated credentials\n", stderr);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    idle_timeout = idle_timeout_seconds();
    service.idle_since = time(NULL);
    service.revoke_generation_valid =
        ksd_revoke_generation_read(getuid(), &service.revoke_generation) == 0;
    listener = systemd_listener();
    if (listener < 0) {
        listener = create_listener(socket_path, sizeof(socket_path));
        own_socket = true;
    }
    if (listener < 0) {
        perror("keysharp-desktop serve: listen");
        return 1;
    }
    if (pthread_create(&capture_thread, NULL, capture_worker, &service) != 0) {
        close(listener);
        return 1;
    }

    for (;;) {
        struct pollfd poll_fd = { .fd = listener, .events = POLLIN };
        int ready = poll(&poll_fd, 1u, KSD_REVOCATION_POLL_MILLISECONDS);
        if (ready < 0 && errno == EINTR)
            continue;
        refresh_revocations(&service);
        if (ready < 0 || is_idle(&service, idle_timeout))
            break;
        if (ready == 0 || (poll_fd.revents & POLLIN) == 0)
            continue;

        int descriptor = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
        if (descriptor < 0)
            continue;
        struct timeval receive_timeout = {
            .tv_sec = (time_t)KSD_HANDSHAKE_TIMEOUT_SECONDS,
            .tv_usec = 0,
        };
        struct timeval send_timeout = { .tv_sec = 35, .tv_usec = 0 };
        (void)setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout));
        (void)setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));

        client_context *client = calloc(1u, sizeof(*client));
        socklen_t credentials_length = sizeof(client->credentials);
        if (client == NULL
            || getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &client->credentials, &credentials_length) != 0) {
            free(client);
            close(descriptor);
            continue;
        }
        client->start_time = ksd_process_start_time(client->credentials.pid);
        client->descriptor = descriptor;
        client->service = &service;

        pthread_mutex_lock(&service.mutex);
        if (service.clients >= KSD_MAX_CLIENTS) {
            pthread_mutex_unlock(&service.mutex);
            (void)write_handshake_error(descriptor, "busy", "too many clients");
            close(descriptor);
            free(client);
            continue;
        }
        service.clients++;
        client->next = service.client_head;
        service.client_head = client;
        pthread_mutex_unlock(&service.mutex);

        pthread_t thread;
        if (pthread_create(&thread, NULL, client_worker, client) != 0) {
            pthread_mutex_lock(&service.mutex);
            client_context **link = &service.client_head;
            while (*link != NULL && *link != client)
                link = &(*link)->next;
            if (*link == client)
                *link = client->next;
            service.clients--;
            pthread_mutex_unlock(&service.mutex);
            close(descriptor);
            free(client);
            continue;
        }
        pthread_detach(thread);
    }

    close(listener);
    if (own_socket && socket_path[0] != '\0')
        (void)unlink(socket_path);
    pthread_mutex_lock(&service.mutex);
    service.stopping = true;
    pthread_cond_signal(&service.queued);
    pthread_mutex_unlock(&service.mutex);
    pthread_join(capture_thread, NULL);
    pthread_cond_destroy(&service.queued);
    pthread_mutex_destroy(&service.mutex);
    return 0;
}
