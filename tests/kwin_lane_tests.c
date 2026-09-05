#include "kwin_wire.h"
#include "operation_scope.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

/* The lane split only means anything while the fast lane is bounded. Swept
 * over the whole opcode space rather than checked on a list, so a verb added
 * to the fast lane later cannot quietly bring an enumeration with it. */
static void check_fast_lane_is_bounded(void)
{
    unsigned fast = 0u;
    unsigned slow = 0u;

    for (uint32_t value = 0u; value <= UINT16_MAX; value++) {
        uint16_t opcode = (uint16_t)value;
        ksd_kwin_lane lane = ksd_kwin_lane_for(opcode);
        ksd_kwin_cost cost = ksd_kwin_script_cost(opcode);

        switch (lane) {
            case KSD_KWIN_LANE_FAST:
                fast++;
                if (cost != KSD_KWIN_COST_BOUNDED) {
                    fprintf(stderr,
                            "opcode 0x%04x sits in the fast lane with "
                            "unbounded cost. The fast lane exists so a "
                            "trivial query is never behind an enumeration; "
                            "admitting one to it removes the only guarantee "
                            "the split provides.\n", (unsigned)opcode);
                    abort();
                }
                break;
            case KSD_KWIN_LANE_SLOW:
                slow++;
                if (cost != KSD_KWIN_COST_UNBOUNDED) {
                    fprintf(stderr,
                            "opcode 0x%04x sits in the slow lane with bounded "
                            "cost. A bounded verb in the slow lane waits "
                            "behind enumerations for no reason.\n",
                            (unsigned)opcode);
                    abort();
                }
                break;
            case KSD_KWIN_LANE_NONE:
                if (cost != KSD_KWIN_COST_NONE) {
                    fprintf(stderr,
                            "opcode 0x%04x takes no lane but claims a script "
                            "cost. An opcode that never reaches the script "
                            "has no cost there.\n", (unsigned)opcode);
                    abort();
                }
                break;
        }
    }

    /* WINDOW_LIST alone is unbounded, and the fast lane is not empty. Both
     * counts are asserted so a change that empties either shows up as a
     * failure rather than a vacuous sweep. */
    /* Two enumerations now: the window list and the handle list. Both walk
     * every window, which is what the slow lane is for -- handles is far
     * cheaper per window but its cost still grows with the session. */
    assert(slow == 2u);
    assert(ksd_kwin_lane_for(KSD_OP_WINDOW_LIST) == KSD_KWIN_LANE_SLOW);
    assert(ksd_kwin_lane_for(KSD_OP_WINDOW_HANDLES) == KSD_KWIN_LANE_SLOW);
    assert(ksd_kwin_lane_for(KSD_OP_WINDOW_QUERY) == KSD_KWIN_LANE_FAST);
    assert(fast >= 10u);
}

/* Everything the script serves must be an operation the service classifies,
 * or a job could be dispatched for an opcode the authority would refuse. */
static void check_lanes_are_classified(void)
{
    for (uint32_t value = 0u; value <= UINT16_MAX; value++) {
        uint16_t opcode = (uint16_t)value;

        if (ksd_kwin_lane_for(opcode) == KSD_KWIN_LANE_NONE)
            continue;
        if (ksd_operation_bit(opcode) == 0u) {
            fprintf(stderr,
                    "opcode 0x%04x takes a KWin lane but carries no operation "
                    "bit, so the authority would never admit it.\n",
                    (unsigned)opcode);
            abort();
        }
    }
}

/* Captures deliberately never reach the script: they run in the forked
 * worker, which is what keeps a long capture off the compositor thread. */
static void check_capture_never_takes_a_lane(void)
{
    assert(ksd_kwin_lane_for(KSD_OP_CAPTURE_AREA) == KSD_KWIN_LANE_NONE);
    assert(ksd_kwin_lane_for(KSD_OP_CAPTURE_WINDOW) == KSD_KWIN_LANE_NONE);
    assert(ksd_kwin_script_cost(KSD_OP_CAPTURE_AREA) == KSD_KWIN_COST_NONE);
    assert(ksd_kwin_script_cost(KSD_OP_CAPTURE_WINDOW) == KSD_KWIN_COST_NONE);
}

int main(void)
{
    check_fast_lane_is_bounded();
    check_lanes_are_classified();
    check_capture_never_takes_a_lane();
    return 0;
}
