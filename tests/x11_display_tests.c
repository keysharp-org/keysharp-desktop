#include "protocol.h"
#include "x11_display.h"
#include "x11_worker.h"

#include <assert.h>
#include <string.h>

static bool accepts(const char *value, const char *expected)
{
    char canonical[KSD_X11_DISPLAY_CAPACITY];
    if (!ksd_x11_display_parse(value, canonical, sizeof(canonical)))
        return false;
    return strcmp(canonical, expected) == 0;
}

static bool rejects(const char *value)
{
    char canonical[KSD_X11_DISPLAY_CAPACITY];
    return !ksd_x11_display_parse(value, canonical, sizeof(canonical));
}

static ksd_frame verb(uint16_t opcode, const uint8_t *payload,
                      uint32_t length)
{
    ksd_frame frame = { 0 };
    frame.opcode = opcode;
    frame.request_id = 1u;
    frame.payload = (uint8_t *)payload;
    frame.payload_length = length;
    return frame;
}

/* The worker refuses a request it does not serve rather than opening a
 * display for it, so the shape is checked before anything reaches the X
 * library. */
static void check_request_validity(void)
{
    static const uint8_t list_ok[8] = { 1, 0, 0, 0, 0, 0, 0, 0 };
    static const uint8_t list_reserved[8] = { 0, 0, 0, 0, 1, 0, 0, 0 };
    static const uint8_t list_bad_flag[8] = { 2, 0, 0, 0, 0, 0, 0, 0 };
    static const uint8_t trailing[12] = { 0 };

    ksd_frame list = verb(KSD_OP_WINDOW_LIST, list_ok, 8u);
    assert(ksd_x11_request_valid(&list));

    list = verb(KSD_OP_WINDOW_LIST, list_reserved, 8u);
    assert(!ksd_x11_request_valid(&list));
    list = verb(KSD_OP_WINDOW_LIST, list_bad_flag, 8u);
    assert(!ksd_x11_request_valid(&list));
    list = verb(KSD_OP_WINDOW_LIST, trailing, 12u);
    assert(!ksd_x11_request_valid(&list));
    list = verb(KSD_OP_WINDOW_LIST, NULL, 0u);
    assert(!ksd_x11_request_valid(&list));

    ksd_frame empty = verb(KSD_OP_WORK_AREA, NULL, 0u);
    assert(ksd_x11_request_valid(&empty));
    empty = verb(KSD_OP_CURSOR_POSITION, NULL, 0u);
    assert(ksd_x11_request_valid(&empty));
    empty = verb(KSD_OP_WINDOW_ACTIVE, NULL, 0u);
    assert(ksd_x11_request_valid(&empty));

    /* A payload on a verb that takes none. */
    empty = verb(KSD_OP_WORK_AREA, list_ok, 8u);
    assert(!ksd_x11_request_valid(&empty));

    /* The two clipboard reads that take no argument. */
    empty = verb(KSD_OP_CLIPBOARD_TEXT, NULL, 0u);
    assert(ksd_x11_request_valid(&empty));
    empty = verb(KSD_OP_CLIPBOARD_MIMETYPES, NULL, 0u);
    assert(ksd_x11_request_valid(&empty));

    /* A capture carries a rectangle or a handle, so the bare opcode is not a
     * request: the shape is checked, not just the opcode. */
    ksd_frame other = verb(KSD_OP_CAPTURE_AREA, NULL, 0u);
    assert(!ksd_x11_request_valid(&other));
    other = verb(KSD_OP_CAPTURE_WINDOW, NULL, 0u);
    assert(!ksd_x11_request_valid(&other));

    /* Reading one format needs the name of the format. */
    static const uint8_t mimetype[] = {
        7u, 0u, 0u, 0u, 't', 'e', 'x', 't', '/', 'c', 's'
    };
    other = verb(KSD_OP_CLIPBOARD_CONTENT, mimetype, sizeof(mimetype));
    assert(ksd_x11_request_valid(&other));
    other = verb(KSD_OP_CLIPBOARD_CONTENT, NULL, 0u);
    assert(!ksd_x11_request_valid(&other));
    /* A length that does not match the bytes that follow it. */
    static const uint8_t truncated[] = { 9u, 0u, 0u, 0u, 't', 'e', 'x', 't' };
    other = verb(KSD_OP_CLIPBOARD_CONTENT, truncated, sizeof(truncated));
    assert(!ksd_x11_request_valid(&other));

    /* Writing the clipboard is refused: this worker cannot hold a selection,
     * and accepting the request would drop the content when it exited. */
    other = verb(KSD_OP_CLIPBOARD_SET_CONTENT, NULL, 0u);
    assert(!ksd_x11_request_valid(&other));

    /* Window control needs a window manager, and input is not this project. */
    other = verb(KSD_OP_WINDOW_CLOSE, NULL, 0u);
    assert(!ksd_x11_request_valid(&other));
    other = verb(KSD_OP_MOUSE_BUTTON, NULL, 0u);
    assert(!ksd_x11_request_valid(&other));
    assert(!ksd_x11_request_valid(NULL));
}

int main(void)
{
    char long_colons[4096];

    assert(accepts(":0", ":0"));
    assert(accepts(":0.0", ":0.0"));
    assert(accepts(":10", ":10"));
    assert(accepts(":255.255", ":255.255"));

    /* A host part names a remote server and a transport prefix chooses a
     * transport. The broker will do neither on a caller's behalf, so both are
     * refused rather than passed through to the X library to interpret. */
    assert(rejects("host:0"));
    assert(rejects("tcp/host:0"));
    assert(rejects("unix/:0"));
    assert(rejects("localhost:0"));

    /* Shapes with nothing where a number must be. */
    assert(rejects(""));
    assert(rejects(":"));
    assert(rejects(":0."));
    assert(rejects("0"));
    assert(rejects(".0"));

    /* Anything trailing. This is how a shell metacharacter or a second screen
     * field would arrive. */
    assert(rejects(":0 ;rm"));
    assert(rejects(":0.0.0"));
    assert(rejects(":0x"));
    assert(rejects(":0 "));

    /* Out of range, and too many digits to be worth reading. */
    assert(rejects(":99999"));
    assert(rejects(":256"));
    assert(rejects(":0.256"));
    assert(rejects(":000000"));

    memset(long_colons, 58, sizeof(long_colons) - 1u);
    long_colons[sizeof(long_colons) - 1u] = 0;
    assert(rejects(long_colons));

    /* A buffer that cannot hold the longest accepted value is refused rather
     * than filled with a truncated one. */
    char narrow[4];
    assert(!ksd_x11_display_parse(":0", narrow, sizeof(narrow)));
    check_request_validity();
    return 0;
}
