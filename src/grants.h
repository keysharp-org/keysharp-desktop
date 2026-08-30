#ifndef KEYSHARP_DESKTOP_GRANTS_H
#define KEYSHARP_DESKTOP_GRANTS_H

#include "common.h"

#include <stdint.h>
#include <sys/types.h>

#ifndef KSD_GRANT_DIRECTORY
#define KSD_GRANT_DIRECTORY "/var/lib/keysharp-permissions/v1"
#endif

typedef struct ksd_stored_grant {
    char hash[KSD_HASH_HEX_LENGTH + 1u];
    uint32_t capabilities;
    char executable[KSD_PATH_MAX];
} ksd_stored_grant;

int ksd_grants_check(const ksd_process_identity *identity, uint32_t *capabilities);
int ksd_grants_check_at_generation(const ksd_process_identity *identity,
                                   uint32_t *capabilities, uint64_t *generation);
int ksd_grants_add(const ksd_process_identity *identity, uint32_t capabilities);
/* Returns 1 without writing when a revoke changed the shared generation since
 * the caller began authorization, -1 on an I/O error, and 0 on success. */
int ksd_grants_add_if_generation(const ksd_process_identity *identity,
                                 uint32_t capabilities, uint64_t expected_generation);
int ksd_grants_prompt_lock_acquire(uid_t uid, const char *hash);
void ksd_grants_prompt_lock_release(int descriptor);
int ksd_grants_list_uid(uid_t uid, ksd_stored_grant **grants, size_t *count);
int ksd_grants_revoke(uid_t uid, const char *hash, uint32_t capabilities);
int ksd_grants_revoke_uid(uid_t uid);
int ksd_grants_bump_revoke_generation(uid_t uid);

#endif
