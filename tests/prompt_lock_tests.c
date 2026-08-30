#include "common.h"
#include "grants.h"
#include "keysharp_desktop/protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "check failed at %s:%d: %s\n",                    \
                    __FILE__, __LINE__, #condition);                            \
            return 1;                                                           \
        }                                                                       \
    } while (0)

static const char app_hash[] =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

static int wait_readable(int descriptor, int timeout_milliseconds)
{
    struct pollfd item = { .fd = descriptor, .events = POLLIN };
    int result;

    do {
        result = poll(&item, 1u, timeout_milliseconds);
    } while (result < 0 && errno == EINTR);
    return result;
}

static int wait_child(pid_t child)
{
    int status;
    pid_t result;

    do {
        result = waitpid(child, &status, 0);
    } while (result < 0 && errno == EINTR);
    return result == child && WIFEXITED(status) && WEXITSTATUS(status) == 0
        ? 0 : -1;
}

static int prompt_path(char *path, size_t capacity)
{
    int length = snprintf(path, capacity, KSD_RUNTIME_DIRECTORY
                          "/.prompt-%lu-%s.lock",
                          (unsigned long)getuid(), app_hash);
    return length > 0 && (size_t)length < capacity ? 0 : -1;
}

static int grant_path(char *path, size_t capacity, uint32_t shared_capability)
{
    int length = snprintf(path, capacity, KSD_GRANT_DIRECTORY
                          "/grant-%lu-%s-%08x.grant",
                          (unsigned long)getuid(), app_hash, shared_capability);
    return length > 0 && (size_t)length < capacity ? 0 : -1;
}

int main(void)
{
    char generation_lock[KSD_PATH_MAX];
    char generation_path[KSD_PATH_MAX];
    char lock_path[KSD_PATH_MAX + 128u];
    char marker_path[KSD_PATH_MAX + 128u];
    ksd_process_identity identity = { .uid = getuid() };
    struct stat info;
    uint64_t generation = 0u;
    uint32_t capabilities = 0u;
    int pipe_descriptors[2];
    int prompt_lock;
    pid_t child;
    unsigned char result;

    (void)snprintf(identity.hash, sizeof(identity.hash), "%s", app_hash);
    (void)snprintf(identity.executable, sizeof(identity.executable),
                   "/usr/bin/prompt-lock-test");
    CHECK(prompt_path(lock_path, sizeof(lock_path)) == 0);
    CHECK(ksd_revoke_generation_path(getuid(), generation_path,
                                     sizeof(generation_path)) == 0);
    CHECK(snprintf(generation_lock, sizeof(generation_lock),
                   KSD_RUNTIME_DIRECTORY "/.revoke-%lu.lock",
                   (unsigned long)getuid()) > 0);
    CHECK(grant_path(marker_path, sizeof(marker_path), KSP_SCOPE_SCREEN_CAPTURE) == 0);

    (void)unlink(marker_path);
    (void)unlink(KSD_GRANT_DIRECTORY "/.lock");
    (void)rmdir(KSD_GRANT_DIRECTORY);
    (void)unlink(lock_path);
    (void)unlink(generation_path);
    (void)unlink(generation_lock);
    (void)rmdir(KSD_RUNTIME_DIRECTORY);

    errno = 0;
    CHECK(ksd_grants_prompt_lock_acquire(getuid(), "not-a-hash") < 0);
    CHECK(errno == EINVAL);

    prompt_lock = ksd_grants_prompt_lock_acquire(getuid(), app_hash);
    CHECK(prompt_lock >= 0);
    CHECK(lstat(lock_path, &info) == 0 && S_ISREG(info.st_mode));
    CHECK(info.st_uid == geteuid());
    CHECK((info.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO))
          == (S_IRUSR | S_IWUSR));

    CHECK(pipe2(pipe_descriptors, O_CLOEXEC) == 0);
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        uint64_t child_generation = 0u;
        uint32_t child_capabilities = 0u;
        unsigned char observed = 0u;

        close(pipe_descriptors[0]);
        close(prompt_lock);
        int child_lock = ksd_grants_prompt_lock_acquire(getuid(), app_hash);
        if (child_lock >= 0
            && ksd_grants_check_at_generation(&identity, &child_capabilities,
                                              &child_generation) == 0
            && (child_capabilities & KSD_CAP_SCREEN_CAPTURE) != 0u)
            observed = 1u;
        if (child_lock >= 0)
            ksd_grants_prompt_lock_release(child_lock);
        ssize_t written = write(pipe_descriptors[1], &observed, sizeof(observed));
        close(pipe_descriptors[1]);
        _exit(observed == 1u && written == 1 ? 0 : 1);
    }
    close(pipe_descriptors[1]);
    CHECK(wait_readable(pipe_descriptors[0], 150) == 0);
    CHECK(ksd_grants_check_at_generation(&identity, &capabilities,
                                         &generation) == 0);
    CHECK(capabilities == 0u);
    CHECK(ksd_grants_add_if_generation(&identity, KSD_CAP_SCREEN_CAPTURE,
                                       generation) == 0);
    ksd_grants_prompt_lock_release(prompt_lock);
    CHECK(wait_readable(pipe_descriptors[0], 2000) == 1);
    CHECK(read(pipe_descriptors[0], &result, sizeof(result)) == 1 && result == 1u);
    close(pipe_descriptors[0]);
    CHECK(wait_child(child) == 0);

    prompt_lock = ksd_grants_prompt_lock_acquire(getuid(), app_hash);
    CHECK(prompt_lock >= 0);
    CHECK(ksd_grants_check_at_generation(&identity, &capabilities,
                                         &generation) == 0);
    CHECK((capabilities & KSD_CAP_SCREEN_CAPTURE) != 0u);
    CHECK(pipe2(pipe_descriptors, O_CLOEXEC) == 0);
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        unsigned char revoked;

        close(pipe_descriptors[0]);
        close(prompt_lock);
        revoked = ksd_grants_revoke_uid(getuid()) == 0 ? 1u : 0u;
        ssize_t written = write(pipe_descriptors[1], &revoked, sizeof(revoked));
        close(pipe_descriptors[1]);
        _exit(revoked == 1u && written == 1 ? 0 : 1);
    }
    close(pipe_descriptors[1]);
    CHECK(wait_readable(pipe_descriptors[0], 2000) == 1);
    CHECK(read(pipe_descriptors[0], &result, sizeof(result)) == 1 && result == 1u);
    close(pipe_descriptors[0]);
    CHECK(wait_child(child) == 0);
    CHECK(ksd_grants_add_if_generation(&identity, KSD_CAP_WINDOW_CONTROL,
                                       generation) == 1);
    ksd_grants_prompt_lock_release(prompt_lock);
    capabilities = UINT32_MAX;
    CHECK(ksd_grants_check(&identity, &capabilities) == 0);
    CHECK(capabilities == 0u);

    CHECK(unlink(lock_path) == 0);
    CHECK(symlink("/dev/null", lock_path) == 0);
    CHECK(ksd_grants_prompt_lock_acquire(getuid(), app_hash) < 0);
    CHECK(lstat(lock_path, &info) == 0 && S_ISLNK(info.st_mode));
    CHECK(unlink(lock_path) == 0);

    (void)unlink(generation_path);
    (void)unlink(generation_lock);
    (void)rmdir(KSD_RUNTIME_DIRECTORY);
    (void)unlink(marker_path);
    (void)unlink(KSD_GRANT_DIRECTORY "/.lock");
    (void)rmdir(KSD_GRANT_DIRECTORY);
    return 0;
}
