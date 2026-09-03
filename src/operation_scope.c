#include "operation_scope.h"

#include "protocol.h"

#include <keysharp_permissions/permissions.h>

uint32_t ksd_operation_scope(uint16_t opcode)
{
    if (opcode == KSD_OP_CAPTURE_AREA || opcode == KSD_OP_CAPTURE_WINDOW)
        return KSP_SCOPE_SCREEN_CAPTURE;
    if (opcode == KSD_OP_WINDOW_LIST || opcode == KSD_OP_WINDOW_ACTIVE
        || opcode == KSD_OP_WINDOW_WATCH)
        return KSP_SCOPE_WINDOW_MONITORING;
    if (opcode >= KSD_OP_WINDOW_FOCUS
        && opcode <= KSD_OP_WINDOW_GET_RESERVED)
        return KSP_SCOPE_WINDOW_CONTROL;
    if (opcode >= KSD_OP_CLIPBOARD_MIMETYPES
        && opcode <= KSD_OP_CLIPBOARD_WATCH)
        return KSP_SCOPE_CLIPBOARD_MONITORING;
    if (opcode >= KSD_OP_MOUSE_MOVE_ABSOLUTE
        && opcode <= KSD_OP_MOUSE_SCROLL)
        return KSP_SCOPE_INPUT_CONTROL;
    return 0u;
}

bool ksd_operation_scope_free(uint16_t opcode)
{
    return opcode == KSD_OP_CURSOR_POSITION || opcode == KSD_OP_WORK_AREA
        || opcode == KSD_OP_CLIPBOARD_SET_CONTENT;
}

bool ksd_operation_chunkable(uint16_t opcode)
{
    return opcode == KSD_OP_CLIPBOARD_SET_CONTENT;
}

bool ksd_request_chunk_admissible(uint16_t opcode, uint16_t flags,
                                 uint64_t request_id)
{
    return request_id != 0u
        && (flags & (KSD_FLAG_RESPONSE | KSD_FLAG_EVENT)) == 0u
        && ksd_operation_chunkable(opcode);
}

uint64_t ksd_operation_bit(uint16_t opcode)
{
    switch (opcode) {
        case KSD_OP_CAPTURE_AREA: return KSD_OPERATION_CAPTURE_AREA;
        case KSD_OP_CAPTURE_WINDOW: return KSD_OPERATION_CAPTURE_WINDOW;
        case KSD_OP_WINDOW_LIST: return KSD_OPERATION_WINDOW_LIST;
        case KSD_OP_WINDOW_ACTIVE: return KSD_OPERATION_WINDOW_ACTIVE;
        case KSD_OP_WINDOW_WATCH: return KSD_OPERATION_WINDOW_WATCH;
        case KSD_OP_WINDOW_FOCUS: return KSD_OPERATION_WINDOW_FOCUS;
        case KSD_OP_WINDOW_RAISE: return KSD_OPERATION_WINDOW_RAISE;
        case KSD_OP_WINDOW_LOWER: return KSD_OPERATION_WINDOW_LOWER;
        case KSD_OP_WINDOW_CLOSE: return KSD_OPERATION_WINDOW_CLOSE;
        case KSD_OP_WINDOW_KILL: return KSD_OPERATION_WINDOW_KILL;
        case KSD_OP_WINDOW_MOVE_RESIZE:
            return KSD_OPERATION_WINDOW_MOVE_RESIZE;
        case KSD_OP_WINDOW_MOVE_RESIZE_XID:
            return KSD_OPERATION_WINDOW_MOVE_RESIZE_XID;
        case KSD_OP_WINDOW_SET_STATE:
            return KSD_OPERATION_WINDOW_SET_STATE;
        case KSD_OP_WINDOW_SET_OPACITY:
            return KSD_OPERATION_WINDOW_SET_OPACITY;
        case KSD_OP_WINDOW_SET_ABOVE:
            return KSD_OPERATION_WINDOW_SET_ABOVE;
        case KSD_OP_WINDOW_SET_DECORATED:
            return KSD_OPERATION_WINDOW_SET_DECORATED;
        case KSD_OP_WINDOW_RESERVE:
            return KSD_OPERATION_WINDOW_RESERVE;
        case KSD_OP_WINDOW_GET_RESERVED:
            return KSD_OPERATION_WINDOW_GET_RESERVED;
        case KSD_OP_CLIPBOARD_MIMETYPES:
            return KSD_OPERATION_CLIPBOARD_MIMETYPES;
        case KSD_OP_CLIPBOARD_CONTENT:
            return KSD_OPERATION_CLIPBOARD_CONTENT;
        case KSD_OP_CLIPBOARD_TEXT: return KSD_OPERATION_CLIPBOARD_TEXT;
        case KSD_OP_CLIPBOARD_WATCH: return KSD_OPERATION_CLIPBOARD_WATCH;
        case KSD_OP_CLIPBOARD_SET_CONTENT:
            return KSD_OPERATION_CLIPBOARD_SET_CONTENT;
        case KSD_OP_MOUSE_MOVE_ABSOLUTE:
            return KSD_OPERATION_MOUSE_MOVE_ABSOLUTE;
        case KSD_OP_MOUSE_MOVE_RELATIVE:
            return KSD_OPERATION_MOUSE_MOVE_RELATIVE;
        case KSD_OP_MOUSE_BUTTON: return KSD_OPERATION_MOUSE_BUTTON;
        case KSD_OP_MOUSE_SCROLL: return KSD_OPERATION_MOUSE_SCROLL;
        case KSD_OP_CURSOR_POSITION: return KSD_OPERATION_CURSOR_POSITION;
        case KSD_OP_WORK_AREA: return KSD_OPERATION_WORK_AREA;
        default: return 0u;
    }
}
