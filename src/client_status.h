#ifndef KEYSHARP_DESKTOP_CLIENT_STATUS_H
#define KEYSHARP_DESKTOP_CLIENT_STATUS_H

#include "keysharp_desktop/client.h"

#include <errno.h>

static inline ksd_status ksd_status_for_system_error(int system_error)
{
    return system_error == ETIMEDOUT || system_error == EAGAIN
            || system_error == EWOULDBLOCK
        ? KSD_STATUS_TIMEOUT : KSD_STATUS_UNAVAILABLE;
}

#endif
