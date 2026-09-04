#include "kwin_wire.h"

ksd_kwin_lane ksd_kwin_lane_for(uint16_t opcode)
{
    switch (opcode) {
        /* O(1) inside the script: one window looked up in its own map, and a
         * bounded number of property touches. */
        case KSD_OP_WINDOW_ACTIVE:
        case KSD_OP_WINDOW_FOCUS:
        case KSD_OP_WINDOW_RAISE:
        case KSD_OP_WINDOW_LOWER:
        case KSD_OP_WINDOW_CLOSE:
        case KSD_OP_WINDOW_MOVE_RESIZE:
        case KSD_OP_WINDOW_MOVE_RESIZE_XID:
        case KSD_OP_WINDOW_SET_STATE:
        case KSD_OP_WINDOW_SET_OPACITY:
        case KSD_OP_WINDOW_SET_ABOVE:
        case KSD_OP_WINDOW_SET_DECORATED:
        case KSD_OP_CURSOR_POSITION:
        case KSD_OP_WORK_AREA:
            return KSD_KWIN_LANE_FAST;

        /* The only verb whose cost grows with the session. */
        case KSD_OP_WINDOW_LIST:
            return KSD_KWIN_LANE_SLOW;

        default:
            /* Reservations come from a daemon table, WINDOW_KILL is a kill(2)
             * in the daemon, and captures run in the forked worker. None of
             * them reaches the script, so none of them takes a lane. */
            return KSD_KWIN_LANE_NONE;
    }
}

ksd_kwin_cost ksd_kwin_script_cost(uint16_t opcode)
{
    switch (opcode) {
        case KSD_OP_WINDOW_ACTIVE:
        case KSD_OP_WINDOW_FOCUS:
        case KSD_OP_WINDOW_RAISE:
        case KSD_OP_WINDOW_LOWER:
        case KSD_OP_WINDOW_CLOSE:
        case KSD_OP_WINDOW_MOVE_RESIZE:
        case KSD_OP_WINDOW_MOVE_RESIZE_XID:
        case KSD_OP_WINDOW_SET_STATE:
        case KSD_OP_WINDOW_SET_OPACITY:
        case KSD_OP_WINDOW_SET_ABOVE:
        case KSD_OP_WINDOW_SET_DECORATED:
        case KSD_OP_CURSOR_POSITION:
        case KSD_OP_WORK_AREA:
            return KSD_KWIN_COST_BOUNDED;

        case KSD_OP_WINDOW_LIST:
            return KSD_KWIN_COST_UNBOUNDED;

        default:
            return KSD_KWIN_COST_NONE;
    }
}
