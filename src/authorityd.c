#include "common.h"
#include "authority_admin.h"
#include "grants.h"
#include "keysharp_desktop/protocol.h"
#include "polkit_result.h"
#include "roles.h"

#include <errno.h>
#include <fcntl.h>
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
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define KSD_POLKIT_ACTION "org.keysharp.desktop.grant"
#define KSD_MAX_AUTHORITY_WORKERS 32u
#ifndef KSD_PKCHECK_PATH
#define KSD_PKCHECK_PATH "/usr/bin/pkcheck"
#endif

typedef struct authority_state {
    pthread_mutex_t mutex;
    size_t workers;
} authority_state;

typedef struct authority_client {
    authority_state *state;
    int descriptor;
} authority_client;

static int inherited_socket(void)
{
    const char *listen_pid = getenv("LISTEN_PID");
    const char *listen_fds = getenv("LISTEN_FDS");

    if (listen_pid != NULL && listen_fds != NULL
        && (pid_t)strtol(listen_pid, NULL, 10) == getpid()
        && strtol(listen_fds, NULL, 10) >= 1)
        return 3;
    return -1;
}

static int create_socket(void)
{
    struct sockaddr_un address;
    int descriptor;

    if (ksd_make_parent_directories(KSD_AUTHORITY_SOCKET, 0755) != 0)
        return -1;
    descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (descriptor < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    (void)snprintf(address.sun_path, sizeof(address.sun_path), "%s", KSD_AUTHORITY_SOCKET);
    (void)unlink(KSD_AUTHORITY_SOCKET);
    if (bind(descriptor, (const struct sockaddr *)&address, sizeof(address)) != 0
        || chmod(KSD_AUTHORITY_SOCKET, 0666) != 0 || listen(descriptor, 32) != 0) {
        close(descriptor);
        return -1;
    }
    return descriptor;
}

static int run_pkcheck(const ksd_process_identity *identity, uint32_t requested)
{
    char process[128];
    char capabilities[32];
    char capability_names[160];
    char prompt[KSD_PATH_MAX + 256u];
    char executable[KSD_PATH_MAX];
    pid_t child;
    int status = 0;

    (void)snprintf(process, sizeof(process), "%ld,%llu,%lu",
                   (long)identity->pid,
                   (unsigned long long)identity->start_time,
                   (unsigned long)identity->uid);
    (void)snprintf(capabilities, sizeof(capabilities), "0x%08x", requested);
    if (ksd_format_capability_names(requested, capability_names,
                                    sizeof(capability_names)) != 0)
        return -1;
    ksd_sanitize_display_text(identity->executable, executable,
                              sizeof(executable));
    int prompt_length = snprintf(prompt, sizeof(prompt),
        "Authentication is required to permanently grant %s to %s",
        capability_names, executable);
    if (prompt_length <= 0 || (size_t)prompt_length >= sizeof(prompt))
        return -1;
    child = fork();
    if (child < 0)
        return -1;
    if (child == 0) {
        if (clearenv() != 0
            || setenv("PATH", "/usr/bin:/bin", 1) != 0
            || setenv("LANG", "C.UTF-8", 1) != 0)
            _exit(127);
        char *const arguments[] = {
            "pkcheck",
            "--action-id", KSD_POLKIT_ACTION,
            "--process", process,
            "--allow-user-interaction",
            "--detail", "app.path", executable,
            "--detail", "desktop.capabilities", capabilities,
            "--detail", "desktop.capability-names", capability_names,
            "--detail", "polkit.message", prompt,
            NULL,
        };
        execv(KSD_PKCHECK_PATH, arguments);
        _exit(127);
    }

    for (unsigned elapsed = 0u; elapsed < KSD_POLKIT_TIMEOUT_SECONDS * 10u; elapsed++) {
        pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child)
            return WIFEXITED(status)
                ? (int)ksd_polkit_result_from_exit(WEXITSTATUS(status))
                : (int)KSD_POLKIT_UNAVAILABLE;
        if (waited < 0 && errno != EINTR)
            return -1;
        struct timespec delay = { .tv_sec = 0, .tv_nsec = 100000000L };
        (void)nanosleep(&delay, NULL);
    }

    (void)kill(child, SIGKILL);
    (void)waitpid(child, &status, 0);
    return -1;
}

static uint16_t check_grant(const ksd_authority_request *request, int client_fd,
                            uint32_t *granted)
{
    struct ucred credentials;
    socklen_t credentials_length = sizeof(credentials);
    ksd_process_identity identity;
    ksd_process_identity verified;
    uint64_t start_time;
    uint64_t grant_generation = 0u;
    uint32_t existing = 0u;
    uint32_t requested = request->capabilities & KSD_CAP_ALL;
    uint32_t missing;
    uint16_t status = KSD_AUTH_STATUS_ERROR;
    int prompt_lock = -1;

    *granted = 0u;
    if (client_fd < 0 || requested != request->capabilities
        || getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &credentials, &credentials_length) != 0)
        return KSD_AUTH_STATUS_ERROR;

    start_time = ksd_process_start_time(credentials.pid);
    if (start_time == 0u
        || ksd_identify_process(credentials.pid, credentials.uid, start_time, &identity) != 0
        || ksd_grants_check(&identity, &existing) != 0)
        return KSD_AUTH_STATUS_ERROR;
    *granted = existing & KSD_CAP_ALL;
    if ((existing & requested) == requested)
        return KSD_AUTH_STATUS_GRANTED;
    if ((request->flags & KSD_AUTH_FLAG_INTERACTIVE) == 0u)
        return KSD_AUTH_STATUS_DENIED;

    prompt_lock = ksd_grants_prompt_lock_acquire(identity.uid, identity.hash);
    if (prompt_lock < 0)
        return KSD_AUTH_STATUS_ERROR;
    if (ksd_process_start_time(identity.pid) != identity.start_time
        || ksd_identify_process(identity.pid, identity.uid, identity.start_time, &verified) != 0
        || strcmp(verified.hash, identity.hash) != 0) {
        status = KSD_AUTH_STATUS_DENIED;
        goto done;
    }
    identity = verified;
    if (ksd_grants_check_at_generation(&identity, &existing,
                                       &grant_generation) != 0)
        goto done;
    *granted = existing & KSD_CAP_ALL;
    if ((existing & requested) == requested) {
        status = KSD_AUTH_STATUS_GRANTED;
        goto done;
    }
    missing = requested & ~existing;

    int authorization = run_pkcheck(&identity, missing);
    if (authorization > 0) {
        status = KSD_AUTH_STATUS_DENIED;
        goto done;
    }
    if (authorization < 0)
        goto done;

    if (ksd_process_start_time(identity.pid) != identity.start_time
        || ksd_identify_process(identity.pid, identity.uid, identity.start_time, &verified) != 0
        || strcmp(verified.hash, identity.hash) != 0) {
        status = KSD_AUTH_STATUS_DENIED;
        goto done;
    }
    int add_result = ksd_grants_add_if_generation(&verified, missing,
                                                   grant_generation);
    if (add_result != 0) {
        status = add_result > 0 ? KSD_AUTH_STATUS_DENIED : KSD_AUTH_STATUS_ERROR;
        goto done;
    }
    *granted = existing | missing;
    status = KSD_AUTH_STATUS_GRANTED;

done:
    ksd_grants_prompt_lock_release(prompt_lock);
    return status;
}

static void handle_connection(int descriptor)
{
    ksd_authority_request request;
    ksd_authority_response response = {
        .magic = KSD_AUTH_MAGIC,
        .major = KSD_PROTOCOL_MAJOR,
        .minor = KSD_PROTOCOL_MINOR,
        .status = KSD_AUTH_STATUS_ERROR,
    };
    ksd_stored_grant *listing = NULL;
    size_t listing_count = 0u;
    bool send_listing = false;
    int passed_fd = -1;

    if (ksd_receive_optional_fd(descriptor, &request, sizeof(request), &passed_fd) != 0
        || request.magic != KSD_AUTH_MAGIC || request.major != KSD_PROTOCOL_MAJOR
        || request.minor != KSD_PROTOCOL_MINOR)
        goto respond;

    if (request.operation == KSD_AUTH_OP_CHECK) {
        response.status = check_grant(&request, passed_fd, &response.granted_capabilities);
    } else if (request.operation == KSD_AUTH_OP_REVOKE_UID) {
        struct ucred credentials;
        socklen_t length = sizeof(credentials);
        if (passed_fd < 0 && request.flags == 0u && request.capabilities == 0u
            && getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED,
                          &credentials, &length) == 0
            && ksd_grants_revoke_uid(credentials.uid) == 0)
            response.status = KSD_AUTH_STATUS_GRANTED;
    } else if (request.operation == KSD_AUTH_OP_LIST_UID) {
        struct ucred credentials;
        socklen_t length = sizeof(credentials);
        if (passed_fd < 0 && request.flags == 0u && request.capabilities == 0u
            && getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED,
                          &credentials, &length) == 0
            && ksd_grants_list_uid(credentials.uid, &listing, &listing_count) == 0
            && listing_count <= KSD_AUTHORITY_MAX_LIST_RECORDS) {
            response.status = KSD_AUTH_STATUS_GRANTED;
            response.granted_capabilities = (uint32_t)listing_count;
            send_listing = true;
        }
    } else if (request.operation == KSD_AUTH_OP_REVOKE) {
        char hash[KSD_HASH_HEX_LENGTH + 1u];
        struct ucred credentials;
        socklen_t length = sizeof(credentials);
        if (passed_fd < 0 && request.flags == 0u
            && request.capabilities != 0u
            && (request.capabilities & ~KSD_CAP_ALL) == 0u
            && ksd_read_all(descriptor, hash, sizeof(hash))
            && hash[KSD_HASH_HEX_LENGTH] == '\0'
            && getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED,
                          &credentials, &length) == 0
            && ksd_grants_revoke(credentials.uid, hash,
                                 request.capabilities) == 0)
            response.status = KSD_AUTH_STATUS_GRANTED;
    } else if (request.operation == KSD_AUTH_OP_INFO) {
        response.status = KSD_AUTH_STATUS_GRANTED;
    }

respond:
    if (passed_fd >= 0)
        close(passed_fd);
    if (!ksd_write_all(descriptor, &response, sizeof(response)))
        goto done;
    if (send_listing) {
        for (size_t index = 0u; index < listing_count; index++) {
            char executable[KSD_PATH_MAX];
            ksd_authority_grant_record_header header = { 0 };

            ksd_sanitize_display_text(listing[index].executable,
                                      executable, sizeof(executable));
            size_t executable_length = strlen(executable);
            (void)snprintf(header.hash, sizeof(header.hash),
                           "%s", listing[index].hash);
            header.capabilities = listing[index].capabilities;
            header.executable_length = (uint32_t)executable_length;
            if (!ksd_write_all(descriptor, &header, sizeof(header))
                || !ksd_write_all(descriptor, executable, executable_length))
                break;
        }
    }

done:
    free(listing);
}

static void *connection_worker(void *argument)
{
    authority_client *client = argument;

    handle_connection(client->descriptor);
    close(client->descriptor);
    pthread_mutex_lock(&client->state->mutex);
    client->state->workers--;
    pthread_mutex_unlock(&client->state->mutex);
    free(client);
    return NULL;
}

int ksd_authority_main(int argc, char **argv)
{
    authority_state state = { .mutex = PTHREAD_MUTEX_INITIALIZER };
    int listener;

    (void)argv;

    if (argc != 1) {
        fprintf(stderr, "usage: keysharp-desktop authority\n");
        return 2;
    }
    if (getuid() != 0 || geteuid() != 0 || getgid() != 0 || getegid() != 0) {
        fputs("keysharp-desktop authority must run as root\n", stderr);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    listener = inherited_socket();
    if (listener < 0)
        listener = create_socket();
    if (listener < 0) {
        perror("keysharp-desktop authority: listen");
        return 1;
    }

    for (;;) {
        int connection = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
        if (connection < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
                struct timespec delay = { .tv_sec = 0, .tv_nsec = 100000000L };
                (void)nanosleep(&delay, NULL);
                continue;
            }
            perror("keysharp-desktop authority: accept");
            return 1;
        }
        struct timeval receive_timeout = { .tv_sec = 5, .tv_usec = 0 };
        struct timeval send_timeout = { .tv_sec = 5, .tv_usec = 0 };
        (void)setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO,
                         &receive_timeout, sizeof(receive_timeout));
        (void)setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO,
                         &send_timeout, sizeof(send_timeout));

        authority_client *client = calloc(1u, sizeof(*client));
        pthread_mutex_lock(&state.mutex);
        if (client == NULL || state.workers >= KSD_MAX_AUTHORITY_WORKERS) {
            pthread_mutex_unlock(&state.mutex);
            free(client);
            close(connection);
            continue;
        }
        state.workers++;
        pthread_mutex_unlock(&state.mutex);
        client->state = &state;
        client->descriptor = connection;

        pthread_t worker;
        if (pthread_create(&worker, NULL, connection_worker, client) != 0) {
            pthread_mutex_lock(&state.mutex);
            state.workers--;
            pthread_mutex_unlock(&state.mutex);
            close(connection);
            free(client);
            continue;
        }
        pthread_detach(worker);
    }
}
