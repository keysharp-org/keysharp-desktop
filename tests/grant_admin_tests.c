#include "common.h"
#include "grants.h"
#include "keysharp_desktop/protocol.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "check failed at %s:%d: %s\n",                    \
                    __FILE__, __LINE__, #condition);                            \
            return 1;                                                           \
        }                                                                       \
    } while (0)

static const char first_hash[] =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char second_hash[] =
    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
static const uint32_t shared_capabilities[] = {
    KSP_SCOPE_SCREEN_CAPTURE,
    KSP_SCOPE_WINDOW_MONITORING,
    KSP_SCOPE_WINDOW_CONTROL,
    KSP_SCOPE_AUDIO_CAPTURE,
    KSP_SCOPE_CAMERA_CAPTURE,
    KSP_SCOPE_CLIPBOARD_MONITORING,
};

static int marker_path(char *path, size_t capacity, const char *hash,
                       uint32_t shared_capability)
{
    int length = snprintf(path, capacity, KSD_GRANT_DIRECTORY
                          "/grant-%lu-%s-%08x.grant", (unsigned long)getuid(),
                          hash, shared_capability);
    return length > 0 && (size_t)length < capacity ? 0 : -1;
}

static void remove_marker(const char *hash, uint32_t shared_capability)
{
    char path[KSD_PATH_MAX + 128u];
    if (marker_path(path, sizeof(path), hash, shared_capability) == 0)
        (void)unlink(path);
}

static int write_marker_text(const char *path, const char *contents)
{
    FILE *file = fopen(path, "w");
    if (file == NULL)
        return -1;
    int result = fputs(contents, file) == EOF ? -1 : 0;
    if (fclose(file) != 0)
        result = -1;
    return result;
}

int main(void)
{
    char generation_lock[KSD_PATH_MAX];
    char generation_path[KSD_PATH_MAX];
    ksd_process_identity first = { .uid = getuid() };
    ksd_process_identity second = { .uid = getuid() };
    ksd_stored_grant *grants = NULL;
    size_t count = 0u;
    uint64_t generation = UINT64_MAX;
    uint32_t capabilities = 0u;

    (void)snprintf(first.hash, sizeof(first.hash), "%s", first_hash);
    (void)snprintf(first.executable, sizeof(first.executable), "/opt/apps/first");
    (void)snprintf(second.hash, sizeof(second.hash), "%s", second_hash);
    (void)snprintf(second.executable, sizeof(second.executable), "/opt/apps/second");
    CHECK(ksd_revoke_generation_path(getuid(), generation_path,
                                     sizeof(generation_path)) == 0);
    CHECK(snprintf(generation_lock, sizeof(generation_lock),
                   KSD_RUNTIME_DIRECTORY "/.revoke-%lu.lock",
                   (unsigned long)getuid()) > 0);

    for (const char *hash = first_hash; hash != NULL;
         hash = hash == first_hash ? second_hash : NULL)
        for (size_t index = 0u;
             index < sizeof(shared_capabilities) / sizeof(shared_capabilities[0]);
             index++)
            remove_marker(hash, shared_capabilities[index]);
    (void)unlink(KSD_GRANT_DIRECTORY "/.lock");
    (void)rmdir(KSD_GRANT_DIRECTORY);
    (void)unlink(generation_path);
    (void)unlink(generation_lock);
    (void)rmdir(KSD_RUNTIME_DIRECTORY);

    CHECK(ksd_grants_add(&first, KSD_CAP_ALL) == 0);
    CHECK(ksd_grants_add(&second, KSD_CAP_WINDOW_CONTROL) == 0);

    char malformed_path[KSD_PATH_MAX + 128u];
    char marker_text[KSD_PATH_MAX + 256u];
    CHECK(marker_path(malformed_path, sizeof(malformed_path), first_hash,
                      KSP_SCOPE_SCREEN_CAPTURE) == 0);
    CHECK(snprintf(marker_text, sizeof(marker_text),
        "keysharp-permission-v1\n%lu\t%s\t%08x\t1\n",
        (unsigned long)getuid(), first_hash, KSP_SCOPE_SCREEN_CAPTURE)
        < (int)sizeof(marker_text));
    CHECK(write_marker_text(malformed_path, marker_text) == 0);
    CHECK(ksd_grants_check(&first, &capabilities) != 0);
    CHECK(snprintf(marker_text, sizeof(marker_text),
        "keysharp-permission-v1\n%lu\t%s\t%08x\t1\t\n",
        (unsigned long)getuid(), first_hash, KSP_SCOPE_SCREEN_CAPTURE)
        < (int)sizeof(marker_text));
    CHECK(write_marker_text(malformed_path, marker_text) == 0);
    CHECK(ksd_grants_check(&first, &capabilities) != 0);
    CHECK(snprintf(marker_text, sizeof(marker_text),
        "keysharp-permission-v1\n%lu\t%s\t%08x\t1\t/opt/apps/first\nextra\n",
        (unsigned long)getuid(), first_hash, KSP_SCOPE_SCREEN_CAPTURE)
        < (int)sizeof(marker_text));
    CHECK(write_marker_text(malformed_path, marker_text) == 0);
    CHECK(ksd_grants_check(&first, &capabilities) != 0);
    CHECK(snprintf(marker_text, sizeof(marker_text),
        "keysharp-permission-v1\n%lu\t%s\t%08x\t1\t/opt/apps/first",
        (unsigned long)getuid(), first_hash, KSP_SCOPE_SCREEN_CAPTURE)
        < (int)sizeof(marker_text));
    CHECK(write_marker_text(malformed_path, marker_text) == 0);
    CHECK(ksd_grants_check(&first, &capabilities) != 0);
    CHECK(snprintf(marker_text, sizeof(marker_text),
        "keysharp-permission-v1\n%lu\t%s\t%08x\t1\t/opt/apps/\rfirst\n",
        (unsigned long)getuid(), first_hash, KSP_SCOPE_SCREEN_CAPTURE)
        < (int)sizeof(marker_text));
    CHECK(write_marker_text(malformed_path, marker_text) == 0);
    CHECK(ksd_grants_check(&first, &capabilities) != 0);
    CHECK(snprintf(marker_text, sizeof(marker_text),
        "keysharp-permission-v1\n%lu\t%s\t%08x\t1\t/opt/apps/\001first\n",
        (unsigned long)getuid(), first_hash, KSP_SCOPE_SCREEN_CAPTURE)
        < (int)sizeof(marker_text));
    CHECK(write_marker_text(malformed_path, marker_text) == 0);
    CHECK(ksd_grants_check(&first, &capabilities) != 0);
    CHECK(snprintf(marker_text, sizeof(marker_text),
        "keysharp-permission-v1\n%lu\t%s\t%08x\t1\t/opt/apps/first\n",
        (unsigned long)getuid(), first_hash, KSP_SCOPE_SCREEN_CAPTURE)
        < (int)sizeof(marker_text));
    CHECK(write_marker_text(malformed_path, marker_text) == 0);

    CHECK(ksd_grants_list_uid(getuid(), &grants, &count) == 0);
    CHECK(count == 2u);
    CHECK(strcmp(grants[0].hash, first_hash) == 0);
    CHECK(grants[0].capabilities == KSD_CAP_ALL);
    CHECK(strcmp(grants[0].executable, "/opt/apps/first") == 0);
    CHECK(strcmp(grants[1].hash, second_hash) == 0);
    CHECK(grants[1].capabilities == KSD_CAP_WINDOW_CONTROL);
    free(grants);
    grants = NULL;

    CHECK(ksd_revoke_generation_read(getuid(), &generation) == 0);
    CHECK(generation == 0u);
    CHECK(ksd_grants_revoke(getuid(), first_hash,
                            KSD_CAP_SCREEN_CAPTURE) == 0);
    CHECK(ksd_revoke_generation_read(getuid(), &generation) == 0);
    CHECK(generation == 2u);
    CHECK(ksd_grants_check(&first, &capabilities) == 0);
    CHECK(capabilities == (KSD_CAP_ALL & ~KSD_CAP_SCREEN_CAPTURE));
    CHECK(ksd_grants_check(&second, &capabilities) == 0);
    CHECK(capabilities == KSD_CAP_WINDOW_CONTROL);
    CHECK(ksd_grants_add_if_generation(&first, KSD_CAP_SCREEN_CAPTURE, 0u) == 1);

    CHECK(ksd_grants_revoke(getuid(), first_hash, KSD_CAP_ALL) == 0);
    CHECK(ksd_revoke_generation_read(getuid(), &generation) == 0);
    CHECK(generation == 4u);
    CHECK(ksd_grants_check(&first, &capabilities) == 0);
    CHECK(capabilities == 0u);
    CHECK(ksd_grants_list_uid(getuid(), &grants, &count) == 0);
    CHECK(count == 1u && strcmp(grants[0].hash, second_hash) == 0);
    free(grants);
    grants = NULL;

    CHECK(ksd_grants_revoke(getuid(), "INVALID", KSD_CAP_ALL) < 0);
    CHECK(ksd_grants_revoke(getuid(), second_hash, 0u) < 0);
    CHECK(ksd_grants_revoke_uid(getuid()) == 0);
    CHECK(ksd_revoke_generation_read(getuid(), &generation) == 0);
    CHECK(generation == 6u);
    CHECK(ksd_grants_list_uid(getuid(), &grants, &count) == 0);
    CHECK(count == 0u && grants == NULL);

    for (const char *hash = first_hash; hash != NULL;
         hash = hash == first_hash ? second_hash : NULL)
        for (size_t index = 0u;
             index < sizeof(shared_capabilities) / sizeof(shared_capabilities[0]);
             index++)
            remove_marker(hash, shared_capabilities[index]);
    (void)unlink(generation_path);
    (void)unlink(generation_lock);
    (void)rmdir(KSD_RUNTIME_DIRECTORY);
    (void)unlink(KSD_GRANT_DIRECTORY "/.lock");
    (void)rmdir(KSD_GRANT_DIRECTORY);
    return 0;
}
