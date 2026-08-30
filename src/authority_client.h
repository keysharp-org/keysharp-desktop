#ifndef KEYSHARP_DESKTOP_AUTHORITY_CLIENT_H
#define KEYSHARP_DESKTOP_AUTHORITY_CLIENT_H

#include "common.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ksd_authority_grant_info {
    char hash[KSD_HASH_HEX_LENGTH + 1u];
    uint32_t capabilities;
    char executable[KSD_PATH_MAX];
} ksd_authority_grant_info;

int ksd_authority_check(int client_fd, uint32_t capabilities, bool interactive,
                        uint32_t *granted_capabilities);
int ksd_authority_list_current_uid(ksd_authority_grant_info **grants,
                                   size_t *count);
int ksd_authority_revoke_current_uid(const char *hash, uint32_t capabilities,
                                     bool all);

#endif
