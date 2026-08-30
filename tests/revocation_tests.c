#include "common.h"
#include "grants.h"
#include "keysharp_desktop/protocol.h"

#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void grant_path(char *path, size_t capacity, uid_t uid,
                       const char *hash, uint32_t shared_capability)
{
    assert(snprintf(path, capacity,
                    KSD_GRANT_DIRECTORY "/grant-%lu-%s-%08x.grant",
                    (unsigned long)uid, hash, shared_capability) > 0);
}

int main(void)
{
    uint64_t generation = UINT64_MAX;
    char generation_path[KSD_PATH_MAX];
    char generation_lock[KSD_PATH_MAX];
    char path[KSD_PATH_MAX];
    char sentinel[KSD_PATH_MAX];
    static const char hash[] =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    ksd_process_identity identity = { .uid = 0u };
    uint32_t capabilities = 0u;

    identity.uid = getuid();
    (void)snprintf(identity.hash, sizeof(identity.hash), "%s", hash);
    (void)snprintf(identity.executable, sizeof(identity.executable),
                   "/usr/bin/example-app");

    assert(ksd_revoke_generation_path(getuid(), generation_path,
                                      sizeof(generation_path)) == 0);
    assert(snprintf(generation_lock, sizeof(generation_lock),
                    KSD_RUNTIME_DIRECTORY "/.revoke-%lu.lock",
                    (unsigned long)getuid()) > 0);
    (void)unlink(generation_path);
    (void)unlink(generation_lock);
    (void)rmdir(KSD_RUNTIME_DIRECTORY);
    (void)unlink(KSD_GRANT_DIRECTORY "/.lock");
    (void)rmdir(KSD_GRANT_DIRECTORY);
    assert(ksd_grants_add(&identity, 0x80000000u) < 0);
    assert(ksd_grants_revoke(getuid(), hash, 0x80000000u) < 0);
    assert(ksd_grants_add(&identity,
                          KSD_CAP_SCREEN_CAPTURE
                          | KSD_CAP_WINDOW_MONITORING) == 0);
    assert(ksd_grants_check(&identity, &capabilities) == 0);
    assert(capabilities == (KSD_CAP_SCREEN_CAPTURE
                            | KSD_CAP_WINDOW_MONITORING));

    assert(snprintf(sentinel, sizeof(sentinel),
                    KSD_GRANT_DIRECTORY "/foreign-sentinel") > 0);
    int sentinel_fd = open(sentinel, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    assert(sentinel_fd >= 0);
    close(sentinel_fd);
    assert(ksd_revoke_generation_read(getuid(), &generation) == 0);
    assert(generation == 0u);
    assert(ksd_grants_revoke_uid(getuid()) == 0);
    assert(ksd_revoke_generation_read(getuid(), &generation) == 0);
    assert(generation == 2u);
    capabilities = UINT32_MAX;
    assert(ksd_grants_check(&identity, &capabilities) == 0);
    assert(capabilities == 0u);
    assert(access(sentinel, F_OK) == 0);

    uint64_t before = generation;
    assert(ksd_grants_bump_revoke_generation(getuid()) == 0);
    assert(ksd_grants_add_if_generation(&identity,
                                        KSD_CAP_WINDOW_CONTROL,
                                        before) == 1);
    assert(ksd_grants_revoke_uid(getuid()) == 0);
    assert(ksd_revoke_generation_read(getuid(), &generation) == 0);
    assert(generation == 5u);
    (void)unlink(generation_path);
    (void)unlink(generation_lock);
    (void)rmdir(KSD_RUNTIME_DIRECTORY);
    grant_path(path, sizeof(path), getuid(), hash, KSP_SCOPE_SCREEN_CAPTURE);
    (void)unlink(path);
    grant_path(path, sizeof(path), getuid(), hash, KSP_SCOPE_WINDOW_MONITORING);
    (void)unlink(path);
    (void)unlink(sentinel);
    (void)unlink(KSD_GRANT_DIRECTORY "/.lock");
    (void)rmdir(KSD_GRANT_DIRECTORY);
    return 0;
}
