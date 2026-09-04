#include "worker_pool.h"

bool ksd_authority_admit_worker(size_t workers, size_t uid_workers,
                                bool *from_reserve)
{
    size_t general = KSD_MAX_AUTHORITY_WORKERS
        - KSD_AUTHORITY_REGISTRATION_RESERVE;

    if (from_reserve == NULL)
        return false;
    *from_reserve = false;
    if (workers >= KSD_MAX_AUTHORITY_WORKERS)
        return false;
    /* The ordinary path: a general slot, and this uid is under its cap. */
    if (workers < general && uid_workers < KSD_MAX_AUTHORITY_WORKERS_PER_UID)
        return true;
    /* Otherwise the connection may still come in, but only on a reserved slot,
     * which it keeps only if it turns out to be a registration. A uid at its
     * cap reaches here too: a busy desktop must still be able to register. */
    *from_reserve = true;
    return true;
}

bool ksd_authority_worker_keeps_slot(bool from_reserve, bool is_registration)
{
    return !from_reserve || is_registration;
}
