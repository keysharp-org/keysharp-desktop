#include "backend.h"
#include "kwin_wire.h"
#include "operation_scope.h"
#include "permission_domain.h"
#include "protocol.h"

#include <assert.h>
#include <keysharp_permissions/permissions.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * One row per opcode that execute_operation() is willing to run. A row states
 * the whole contract for that opcode: the backend operation bit it drives and
 * the permission scope its grant check demands. A zero scope means the opcode
 * runs with no grant at all, so only the cursor and work-area reads may carry
 * one, and they must also be named by ksd_operation_scope_free().
 *
 * ksd_operation_scope() is written as ranges, so an opcode renumbered into a
 * neighbouring range silently inherits that range's scope. The rows below pin
 * the forward direction, and the sweep in main() pins the reverse: every
 * opcode outside this table must be unclassified. Adding an opcode to
 * src/operation_scope.c without adding a row here fails this test.
 *
 * chunkable says whether the opcode's request payload may arrive as a
 * KSD_FLAG_MORE sequence. The clipboard write is the only chunkable opcode,
 * so an opcode that gains a chunked payload has to say so on its row, and a
 * chunked opcode must carry a permission scope.
 */
typedef struct expected_operation {
    uint16_t opcode;
    uint32_t scope;
    uint64_t bit;
    bool chunkable;
} expected_operation;

static const expected_operation expected_operations[] = {
    { KSD_OP_CAPTURE_AREA, KSP_SCOPE_SCREEN_CAPTURE,
      KSD_OPERATION_CAPTURE_AREA, false },
    { KSD_OP_CAPTURE_WINDOW, KSP_SCOPE_SCREEN_CAPTURE,
      KSD_OPERATION_CAPTURE_WINDOW, false },
    { KSD_OP_WINDOW_LIST, KSP_SCOPE_WINDOW_MONITORING,
      KSD_OPERATION_WINDOW_LIST, false },
    { KSD_OP_WINDOW_ACTIVE, KSP_SCOPE_WINDOW_MONITORING,
      KSD_OPERATION_WINDOW_ACTIVE, false },
    /* Scope 0: enumeration is ungated. A handle says a window exists and
     * nothing else, so there is nothing to consent to; every verb that reads
     * something ABOUT a window sits above with WINDOW_MONITORING. */
    { KSD_OP_WINDOW_HANDLES, 0u,
      KSD_OPERATION_WINDOW_HANDLES, false },
    { KSD_OP_WINDOW_WATCH, KSP_SCOPE_WINDOW_MONITORING,
      KSD_OPERATION_WINDOW_WATCH, false },
    { KSD_OP_WINDOW_FOCUS, KSP_SCOPE_WINDOW_CONTROL,
      KSD_OPERATION_WINDOW_FOCUS, false },
    { KSD_OP_WINDOW_RAISE, KSP_SCOPE_WINDOW_CONTROL,
      KSD_OPERATION_WINDOW_RAISE, false },
    { KSD_OP_WINDOW_LOWER, KSP_SCOPE_WINDOW_CONTROL,
      KSD_OPERATION_WINDOW_LOWER, false },
    { KSD_OP_WINDOW_CLOSE, KSP_SCOPE_WINDOW_CONTROL,
      KSD_OPERATION_WINDOW_CLOSE, false },
    { KSD_OP_WINDOW_KILL, KSP_SCOPE_WINDOW_CONTROL,
      KSD_OPERATION_WINDOW_KILL, false },
    { KSD_OP_WINDOW_MOVE_RESIZE, KSP_SCOPE_WINDOW_CONTROL,
      KSD_OPERATION_WINDOW_MOVE_RESIZE, false },
    { KSD_OP_WINDOW_MOVE_RESIZE_XID, KSP_SCOPE_WINDOW_CONTROL,
      KSD_OPERATION_WINDOW_MOVE_RESIZE_XID, false },
    { KSD_OP_WINDOW_SET_STATE, KSP_SCOPE_WINDOW_CONTROL,
      KSD_OPERATION_WINDOW_SET_STATE, false },
    { KSD_OP_WINDOW_SET_OPACITY, KSP_SCOPE_WINDOW_CONTROL,
      KSD_OPERATION_WINDOW_SET_OPACITY, false },
    { KSD_OP_WINDOW_SET_ABOVE, KSP_SCOPE_WINDOW_CONTROL,
      KSD_OPERATION_WINDOW_SET_ABOVE, false },
    { KSD_OP_WINDOW_SET_DECORATED, KSP_SCOPE_WINDOW_CONTROL,
      KSD_OPERATION_WINDOW_SET_DECORATED, false },
    { KSD_OP_WINDOW_RESERVE, KSP_SCOPE_WINDOW_CONTROL,
      KSD_OPERATION_WINDOW_RESERVE, false },
    { KSD_OP_WINDOW_GET_RESERVED, KSP_SCOPE_WINDOW_CONTROL,
      KSD_OPERATION_WINDOW_GET_RESERVED, false },
    { KSD_OP_CLIPBOARD_MIMETYPES, KSP_SCOPE_CLIPBOARD_MONITORING,
      KSD_OPERATION_CLIPBOARD_MIMETYPES, false },
    { KSD_OP_CLIPBOARD_CONTENT, KSP_SCOPE_CLIPBOARD_MONITORING,
      KSD_OPERATION_CLIPBOARD_CONTENT, false },
    { KSD_OP_CLIPBOARD_TEXT, KSP_SCOPE_CLIPBOARD_MONITORING,
      KSD_OPERATION_CLIPBOARD_TEXT, false },
    { KSD_OP_CLIPBOARD_WATCH, KSP_SCOPE_CLIPBOARD_MONITORING,
      KSD_OPERATION_CLIPBOARD_WATCH, false },
    { KSD_OP_CLIPBOARD_SET_CONTENT, 0u,
      KSD_OPERATION_CLIPBOARD_SET_CONTENT, true },
    { KSD_OP_MOUSE_MOVE_ABSOLUTE, KSP_SCOPE_INPUT_CONTROL,
      KSD_OPERATION_MOUSE_MOVE_ABSOLUTE, false },
    { KSD_OP_MOUSE_MOVE_RELATIVE, KSP_SCOPE_INPUT_CONTROL,
      KSD_OPERATION_MOUSE_MOVE_RELATIVE, false },
    { KSD_OP_MOUSE_BUTTON, KSP_SCOPE_INPUT_CONTROL,
      KSD_OPERATION_MOUSE_BUTTON, false },
    { KSD_OP_MOUSE_SCROLL, KSP_SCOPE_INPUT_CONTROL,
      KSD_OPERATION_MOUSE_SCROLL, false },
    { KSD_OP_CURSOR_POSITION, 0u, KSD_OPERATION_CURSOR_POSITION, false },
    { KSD_OP_WORK_AREA, 0u, KSD_OPERATION_WORK_AREA, false },
};

#define EXPECTED_OPERATION_COUNT 30u
#define EXPECTED_OPERATION_ROWS \
    (sizeof(expected_operations) / sizeof(expected_operations[0]))

static const expected_operation *expected_for(uint16_t opcode)
{
    for (size_t index = 0u; index < EXPECTED_OPERATION_ROWS; index++)
        if (expected_operations[index].opcode == opcode)
            return &expected_operations[index];
    return NULL;
}

/* The split the whole operation exists for: enumerating windows carries no
 * grant, reading anything about one does. If these two ever answered the same
 * way, WINDOW_HANDLES would be pointless and WINDOW_LIST would be handing out
 * titles for free. */
static void check_enumeration_is_ungated(void)
{
    assert(ksd_operation_scope_free(KSD_OP_WINDOW_HANDLES));
    assert(ksd_operation_scope(KSD_OP_WINDOW_HANDLES) == 0u);
    assert(!ksd_operation_scope_free(KSD_OP_WINDOW_LIST));
    assert(ksd_operation_scope(KSD_OP_WINDOW_LIST) != 0u);
    assert(!ksd_operation_scope_free(KSD_OP_WINDOW_ACTIVE));
    /* And it is a real operation with its own bit, not an alias. */
    assert(ksd_operation_bit(KSD_OP_WINDOW_HANDLES)
           == KSD_OPERATION_WINDOW_HANDLES);
    assert(ksd_operation_bit(KSD_OP_WINDOW_HANDLES)
           != ksd_operation_bit(KSD_OP_WINDOW_LIST));
}

int main(void)
{
    check_enumeration_is_ungated();
    assert(sizeof(expected_operations) / sizeof(expected_operations[0])
           == EXPECTED_OPERATION_COUNT);

    uint64_t classified = 0u;
    for (size_t index = 0u; index < EXPECTED_OPERATION_ROWS; index++) {
        const expected_operation *entry = &expected_operations[index];
        assert(entry->bit != 0u);
        assert((classified & entry->bit) == 0u);
        classified |= entry->bit;
        assert(expected_for(entry->opcode) == entry);
        assert(ksd_operation_bit(entry->opcode) == entry->bit);
        assert(ksd_operation_scope(entry->opcode) == entry->scope);
        assert(ksd_operation_chunkable(entry->opcode) == entry->chunkable);
        if (entry->scope != 0u) {
            assert((entry->scope & KSD_DESKTOP_MANAGED_SCOPES)
                   == entry->scope);
            assert(!ksd_operation_scope_free(entry->opcode));
        } else {
            /* The complete list of operations that carry no grant, named one
             * by one so adding another is a deliberate act. Each is here for
             * its own reason: the pointer and the work area are readable by
             * any client of the display anyway, a clipboard write gives
             * something away rather than taking it, and a handle says a window
             * exists without saying anything about it. */
            assert(entry->opcode == KSD_OP_CURSOR_POSITION
                   || entry->opcode == KSD_OP_WORK_AREA
                   || entry->opcode == KSD_OP_CLIPBOARD_SET_CONTENT
                   || entry->opcode == KSD_OP_WINDOW_HANDLES);
            assert(ksd_operation_scope_free(entry->opcode));
        }
    }

    for (uint32_t value = 0u; value <= UINT16_MAX; value++) {
        uint16_t opcode = (uint16_t)value;
        if (expected_for(opcode) != NULL)
            continue;
        if (ksd_operation_bit(opcode) != 0u
            || ksd_operation_scope(opcode) != 0u
            || ksd_operation_scope_free(opcode)
            || ksd_operation_chunkable(opcode)) {
            fprintf(stderr,
                    "opcode 0x%04x is classified in src/operation_scope.c but "
                    "has no row in expected_operations[]. Add a row naming "
                    "its permission scope and whether its request may be "
                    "chunked, or the operation runs ungated.\n",
                    (unsigned)opcode);
            return 1;
        }
    }

    uint64_t reachable = ksd_backend_operations(KSD_BACKEND_KWIN)
        | ksd_backend_operations(KSD_BACKEND_GNOME)
        | ksd_backend_operations(KSD_BACKEND_CINNAMON);
    assert(reachable == classified);
    assert(ksd_backend_operations(KSD_BACKEND_NONE) == 0u);
    uint64_t in_memory_capture = KSD_OPERATION_CAPTURE_AREA
        | KSD_OPERATION_CAPTURE_WINDOW;
    assert((ksd_backend_operations(KSD_BACKEND_GNOME) & in_memory_capture)
           == in_memory_capture);
    assert((ksd_backend_operations(KSD_BACKEND_CINNAMON)
            & KSD_OPERATION_CAPTURE_WINDOW) != 0u);
    assert((ksd_backend_operations(KSD_BACKEND_CINNAMON)
            & KSD_OPERATION_CAPTURE_AREA) == 0u);
    /* KWin serves the two captures from the forked worker and everything else
     * through the script, over the socket its daemon hands over. Pinned as an
     * exact mask so a verb cannot be advertised before the script implements
     * it, which is the defect this project has now made twice. */
    assert(ksd_backend_operations(KSD_BACKEND_KWIN)
           == (KSD_OPERATION_CAPTURE_AREA | KSD_OPERATION_CAPTURE_WINDOW
               | KSD_OPERATION_WINDOW_LIST | KSD_OPERATION_WINDOW_ACTIVE
               | KSD_OPERATION_WINDOW_HANDLES
               | KSD_OPERATION_WINDOW_FOCUS | KSD_OPERATION_WINDOW_RAISE
               | KSD_OPERATION_WINDOW_CLOSE
               | KSD_OPERATION_WINDOW_MOVE_RESIZE
               | KSD_OPERATION_WINDOW_SET_STATE
               | KSD_OPERATION_WINDOW_SET_OPACITY
               | KSD_OPERATION_WINDOW_SET_ABOVE
               | KSD_OPERATION_WINDOW_SET_DECORATED
               | KSD_OPERATION_CURSOR_POSITION | KSD_OPERATION_WORK_AREA));
    /* LOWER stays out on purpose: KWin exposes raise and no lower, and the
     * script answers UNSUPPORTED rather than approximating it by sending the
     * window behind everything. Advertising it would promise a verb that is
     * always refused. */
    assert((ksd_backend_operations(KSD_BACKEND_KWIN)
            & KSD_OPERATION_WINDOW_LOWER) == 0u);
    /* And every KWin verb outside the two captures takes a lane, or it would
     * be advertised with no way to reach the script. */
    for (uint32_t opcode = 0u; opcode <= UINT16_MAX; opcode++) {
        uint64_t bit = ksd_operation_bit((uint16_t)opcode);

        if (bit == 0u
            || (ksd_backend_operations(KSD_BACKEND_KWIN) & bit) == 0u)
            continue;
        if (opcode == KSD_OP_CAPTURE_AREA || opcode == KSD_OP_CAPTURE_WINDOW)
            continue;
        assert(ksd_kwin_lane_for((uint16_t)opcode) != KSD_KWIN_LANE_NONE);
    }

    /* X11 serves the coordinate group and both captures, and nothing that
     * needs a compositor: no clipboard, no window control, no input. Pinned as
     * an exact mask rather than a list of present bits, so a verb added to the
     * backend without an implementation behind it fails here. */
    /* The generic Wayland ceiling: what the shared protocols make possible
     * for a client on the OUTSIDE of a compositor. Any given compositor
     * implements some subset, and the daemon narrows this at registration. */
    assert(ksd_backend_operations(KSD_BACKEND_GENERIC)
           == (KSD_OPERATION_WINDOW_LIST | KSD_OPERATION_WINDOW_HANDLES
               | KSD_OPERATION_CLIPBOARD_MIMETYPES
               | KSD_OPERATION_CLIPBOARD_CONTENT
               | KSD_OPERATION_CLIPBOARD_TEXT));
    /* The nine that are IMPOSSIBLE here, not merely unwritten, each because no
     * Wayland protocol provides it: no client may restack another client's
     * window, set its geometry, set its opacity, keep it above, change its
     * decoration, learn a pid to signal, or correlate a toplevel to the
     * process that will create it. Pinned so that adding one later has to be a
     * deliberate act with a protocol behind it rather than an oversight. */
    assert((ksd_backend_operations(KSD_BACKEND_GENERIC)
            & (KSD_OPERATION_WINDOW_LOWER | KSD_OPERATION_WINDOW_KILL
               | KSD_OPERATION_WINDOW_MOVE_RESIZE
               | KSD_OPERATION_WINDOW_MOVE_RESIZE_XID
               | KSD_OPERATION_WINDOW_SET_OPACITY
               | KSD_OPERATION_WINDOW_SET_ABOVE
               | KSD_OPERATION_WINDOW_SET_DECORATED
               | KSD_OPERATION_WINDOW_RESERVE
               | KSD_OPERATION_WINDOW_GET_RESERVED)) == 0u);
    /* And active-window, which this protocol cannot answer: nothing in
     * ext-foreign-toplevel-list says which window has focus. */
    assert((ksd_backend_operations(KSD_BACKEND_GENERIC)
            & KSD_OPERATION_WINDOW_ACTIVE) == 0u);
    assert(ksd_backend_operations(KSD_BACKEND_X11)
           == ksd_backend_x11_route(KSD_BACKEND_X11, true));
    assert(ksd_backend_operations(KSD_BACKEND_X11)
           == (KSD_OPERATION_WINDOW_LIST | KSD_OPERATION_WINDOW_ACTIVE
               | KSD_OPERATION_WINDOW_HANDLES
               | KSD_OPERATION_CURSOR_POSITION | KSD_OPERATION_WORK_AREA
               | KSD_OPERATION_CAPTURE_AREA
               | KSD_OPERATION_CAPTURE_WINDOW
               | KSD_OPERATION_CLIPBOARD_MIMETYPES
               | KSD_OPERATION_CLIPBOARD_CONTENT
               | KSD_OPERATION_CLIPBOARD_TEXT
               | KSD_OPERATION_WINDOW_FOCUS | KSD_OPERATION_WINDOW_RAISE
               | KSD_OPERATION_WINDOW_LOWER | KSD_OPERATION_WINDOW_CLOSE
               | KSD_OPERATION_WINDOW_KILL
               | KSD_OPERATION_WINDOW_MOVE_RESIZE
               | KSD_OPERATION_WINDOW_SET_STATE
               | KSD_OPERATION_WINDOW_SET_OPACITY
               | KSD_OPERATION_WINDOW_SET_ABOVE
               | KSD_OPERATION_WINDOW_SET_DECORATED));
    /* Still absent, and each for its own reason rather than merely being
     * unwritten. WATCH needs an event stream this worker has no way to hold
     * open; the reservations are a compositor-side table; MOVE_RESIZE_XID is
     * the same verb as MOVE_RESIZE here, since an X11 handle already is an
     * XID; and input belongs to keysharp-input. */
    assert((ksd_backend_operations(KSD_BACKEND_X11)
            & (KSD_OPERATION_WINDOW_WATCH | KSD_OPERATION_WINDOW_RESERVE
               | KSD_OPERATION_WINDOW_GET_RESERVED
               | KSD_OPERATION_WINDOW_MOVE_RESIZE_XID
               | KSD_OPERATION_CLIPBOARD_WATCH
               | KSD_OPERATION_MOUSE_BUTTON)) == 0u);
    /* Reading the clipboard is served; writing it is not, and must not be
     * advertised while no process outlives the request to hold the selection.
     * A client told it can set the clipboard here would watch the content
     * disappear as the worker exited. */
    assert((ksd_backend_operations(KSD_BACKEND_X11)
            & KSD_OPERATION_CLIPBOARD_SET_CONTENT) == 0u);
    assert((ksd_backend_operations(KSD_BACKEND_X11)
            & KSD_OPERATION_CLIPBOARD_WATCH) == 0u);
    for (uint32_t backend = KSD_BACKEND_X11 + 1u; backend <= 4096u; backend++)
        assert(ksd_backend_operations(backend) == 0u);

    /* Routing never exceeds the mask, and is empty off an X11 session. This is
     * what makes "nothing changes on Wayland" checkable rather than asserted. */
    for (uint32_t backend = 0u; backend <= 4096u; backend++) {
        uint64_t routed = ksd_backend_x11_route(backend, true);
        assert((routed & ~ksd_backend_operations(backend)) == 0u);
        assert(ksd_backend_x11_route(backend, false) == 0u);
    }
    assert(ksd_backend_x11_route(KSD_BACKEND_X11, true) != 0u);
    assert(ksd_backend_x11_route(KSD_BACKEND_GNOME, true) == 0u);
    return 0;
}
