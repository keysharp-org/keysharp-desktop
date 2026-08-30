#ifndef KEYSHARP_DESKTOP_AUTHORITY_ADMIN_H
#define KEYSHARP_DESKTOP_AUTHORITY_ADMIN_H

#include "common.h"

#include <stdint.h>

#define KSD_AUTHORITY_MAX_LIST_RECORDS 4096u

typedef struct ksd_authority_grant_record_header {
    char hash[KSD_HASH_HEX_LENGTH + 1u];
    uint8_t reserved[3];
    uint32_t capabilities;
    uint32_t executable_length;
} ksd_authority_grant_record_header;

_Static_assert(sizeof(ksd_authority_grant_record_header) == 76u,
               "authority grant record header layout changed");

#endif
