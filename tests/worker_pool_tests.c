#include "worker_pool.h"

#include <assert.h>

#define GENERAL (KSD_MAX_AUTHORITY_WORKERS - KSD_AUTHORITY_REGISTRATION_RESERVE)

static bool admits(size_t workers, size_t uid_workers, bool *from_reserve)
{
    return ksd_authority_admit_worker(workers, uid_workers, from_reserve);
}

int main(void)
{
    bool reserved = true;

    /* An idle service admits on a general slot. */
    assert(admits(0u, 0u, &reserved));
    assert(!reserved);

    /* Still general right up to the reserve boundary. */
    assert(admits(GENERAL - 1u, 0u, &reserved));
    assert(!reserved);

    /* K3-a. Once the general pool is full a connection may still come in, but
     * only on a reserved slot, which an ordinary connection will not keep.
     * Without this a uid's applications could fill the pool and the session
     * daemon of that desktop could never register, taking the backend down for
     * every consumer including the ones that were not flooding. */
    assert(admits(GENERAL, 0u, &reserved));
    assert(reserved);
    assert(admits(KSD_MAX_AUTHORITY_WORKERS - 1u, 0u, &reserved));
    assert(reserved);

    /* The pool is still bounded. */
    assert(!admits(KSD_MAX_AUTHORITY_WORKERS, 0u, &reserved));
    assert(!admits(KSD_MAX_AUTHORITY_WORKERS + 1u, 0u, &reserved));

    /* K3-b. A uid at its per-uid cap reaches the reserve rather than being
     * refused outright. Every consumer of one desktop shares a uid, so a uid
     * at its limit is the ordinary busy case, and refusing its daemon a
     * registration would punish the desktop for being in use. */
    assert(admits(0u, KSD_MAX_AUTHORITY_WORKERS_PER_UID, &reserved));
    assert(reserved);
    assert(admits(0u, KSD_MAX_AUTHORITY_WORKERS_PER_UID - 1u, &reserved));
    assert(!reserved);

    /* K3-c. A reserved slot is kept only by a registration. An ordinary
     * connection holding one is exactly the starvation the reserve exists to
     * prevent. */
    assert(ksd_authority_worker_keeps_slot(false, false));
    assert(ksd_authority_worker_keeps_slot(false, true));
    assert(ksd_authority_worker_keeps_slot(true, true));
    assert(!ksd_authority_worker_keeps_slot(true, false));

    /* F1. A consumer may have four KWin requests in flight and no more.
     * Everything a KWin script does runs on the compositor main thread, so one
     * consumer flooding it delays every other consumer of that desktop, and a
     * per-uid cap cannot tell them apart because they share a uid. */
    assert(ksd_authority_admit_kwin(0u));
    assert(ksd_authority_admit_kwin(KSD_MAX_KWIN_INFLIGHT_PER_PID - 1u));
    assert(!ksd_authority_admit_kwin(KSD_MAX_KWIN_INFLIGHT_PER_PID));
    assert(!ksd_authority_admit_kwin(KSD_MAX_KWIN_INFLIGHT_PER_PID + 1u));

    /* Reachable on purpose: one request in flight per connection means a
     * consumer holding five connections hits it. A cap nobody can reach is a
     * cap no test can prove works. */
    assert(KSD_MAX_KWIN_INFLIGHT_PER_PID
           < KSD_MAX_AUTHORITY_WORKERS_PER_UID);

    /* The reserve must be smaller than the pool, or holding it back would
     * leave nothing for ordinary work. */
    assert(KSD_AUTHORITY_REGISTRATION_RESERVE < KSD_MAX_AUTHORITY_WORKERS);
    assert(GENERAL > KSD_MAX_AUTHORITY_WORKERS_PER_UID);
    return 0;
}
