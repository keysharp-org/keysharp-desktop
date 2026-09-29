#include "protocol.h"
#include "wl_displays.h"
#include "wl_hypr.h"
#include "wl_keyboard.h"
#include "wl_watch.h"

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>
#include <stdlib.h>
#include <string.h>

static const char clients_reply[] =
    "[{\n"
    "    \"address\": \"0x5bf064700450\",\n"
    "    \"mapped\": true,\n"
    "    \"hidden\": false,\n"
    "    \"visible\": true,\n"
    "    \"at\": [990, 22],\n"
    "    \"size\": [205, 697],\n"
    "    \"workspace\": {\"id\": 1, \"name\": \"1\"},\n"
    "    \"floating\": false,\n"
    "    \"class\": \"foot\",\n"
    "    \"title\": \"a \\\"quoted\\\" \\u00e9 title\",\n"
    "    \"pid\": 49781,\n"
    "    \"pinned\": false,\n"
    "    \"fullscreen\": 0,\n"
    "    \"grouped\": [],\n"
    "    \"tags\": [\"x\", {\"nested\": [1, 2.5, null]}],\n"
    "    \"focusHistoryID\": 1,\n"
    "    \"stableId\": \"18000002\"\n"
    "},{\n"
    "    \"address\": \"0x5bf0654d25e0\",\n"
    "    \"at\": [-20, 30],\n"
    "    \"size\": [954, 697],\n"
    "    \"workspace\": {\"id\": -98, \"name\": \"special:minimized\"},\n"
    "    \"floating\": true,\n"
    "    \"pid\": 49922,\n"
    "    \"fullscreen\": 2,\n"
    "    \"focusHistoryID\": 0,\n"
    "    \"stableId\": \"18000003\"\n"
    "},{\n"
    "    \"address\": \"0x1\",\n"
    "    \"title\": \"no stable id, skipped\"\n"
    "},{\n"
    "    \"address\": \"not-hex\",\n"
    "    \"stableId\": \"18000004\"\n"
    "}]\n";

static void test_parses_clients(void)
{
    ksd_hypr_client *clients = NULL;
    size_t count = 0u;

    assert(ksd_wayland_hypr_parse_clients(clients_reply,
                                          sizeof(clients_reply) - 1u,
                                          &clients, &count));
    assert(count == 2u);
    assert(strcmp(clients[0].stable_id, "18000002") == 0);
    assert(strcmp(clients[0].address, "0x5bf064700450") == 0);
    assert(clients[0].x == 990 && clients[0].y == 22);
    assert(clients[0].width == 205 && clients[0].height == 697);
    assert(clients[0].workspace_id == 1);
    assert(strcmp(clients[0].workspace_name, "1") == 0);
    assert(clients[0].pid == 49781);
    assert(clients[0].focus_history == 1);
    assert(clients[0].mapped && clients[0].visible);
    assert(!clients[0].floating && clients[0].fullscreen == 0);

    assert(strcmp(clients[1].stable_id, "18000003") == 0);
    assert(clients[1].x == -20 && clients[1].y == 30);
    assert(clients[1].workspace_id == -98);
    assert(strcmp(clients[1].workspace_name, "special:minimized") == 0);
    assert(clients[1].floating && clients[1].fullscreen == 1);
    assert(clients[1].focus_history == 0);
    free(clients);
}

static void test_rejects_malformed_documents(void)
{
    static const char *const bad[] = {
        "",
        "{}",
        "[{\"stableId\": \"1\", \"address\": \"0x1\"",
        "[{\"stableId\": \"1\", \"address\": \"0x1\", \"at\": [1]}]",
        "[1, 2",
    };
    ksd_hypr_client *clients = NULL;
    size_t count = 0u;

    for (size_t index = 0u; index < sizeof(bad) / sizeof(bad[0]); index++) {
        bool parsed = ksd_wayland_hypr_parse_clients(bad[index],
            strlen(bad[index]), &clients, &count);

        /* An object whose own fields are malformed is skipped, but only when
         * the document around it still parses. */
        assert(!parsed || count == 0u);
        free(clients);
        clients = NULL;
    }
    assert(ksd_wayland_hypr_parse_clients("[]", 2u, &clients, &count));
    assert(count == 0u && clients == NULL);
}

static void test_rejects_truncated_identifiers(void)
{
    char reply[256];
    char identifier[KSD_HYPR_ID_CAPACITY + 8u];
    ksd_hypr_client *clients = NULL;
    size_t count = 0u;

    memset(identifier, '7', sizeof(identifier) - 1u);
    identifier[sizeof(identifier) - 1u] = 0;
    strcpy(reply, "[{\"address\": \"0x10\", \"stableId\": \"");
    strcat(reply, identifier);
    strcat(reply, "\"}]");
    assert(ksd_wayland_hypr_parse_clients(reply, strlen(reply), &clients,
                                          &count));
    assert(count == 0u);
    free(clients);
}


static void test_opacity_text_round_trips(void)
{
    char text[32];
    uint32_t parsed;

    assert(ksd_wayland_hypr_opacity_text(255u, text, sizeof(text)));
    assert(strcmp(text, "1.000000") == 0);
    assert(ksd_wayland_hypr_opacity_text(0u, text, sizeof(text)));
    assert(strcmp(text, "0.000000") == 0);
    assert(!ksd_wayland_hypr_opacity_text(256u, text, sizeof(text)));
    for (uint32_t value = 0u; value <= 255u; value++) {
        assert(ksd_wayland_hypr_opacity_text(value, text, sizeof(text)));
        assert(ksd_wayland_hypr_parse_opacity(text, strlen(text), &parsed));
        assert(parsed == value);
    }
}

static void test_parses_opacity_replies(void)
{
    static const char batch[] = "0.25\n\n\n1\n\n\nwindow not found";
    static const char *const rejected[] = {
        "", "window not found", "prop not found", "nan", "0.5x", "-0.1",
    };
    uint32_t parsed;
    int32_t values[3];

    assert(ksd_wayland_hypr_parse_opacity(" 0.5\n", 5u, &parsed)
           && parsed == 128u);
    assert(ksd_wayland_hypr_parse_opacity("1.2", 3u, &parsed)
           && parsed == 255u);
    for (size_t index = 0u; index < sizeof(rejected) / sizeof(rejected[0]);
         index++)
        assert(!ksd_wayland_hypr_parse_opacity(rejected[index],
                                               strlen(rejected[index]),
                                               &parsed));
    assert(ksd_wayland_hypr_parse_opacity_batch(batch, sizeof(batch) - 1u,
                                                values, 3u));
    assert(values[0] == 64 && values[1] == 255 && values[2] == -1);
    assert(!ksd_wayland_hypr_parse_opacity_batch(batch, sizeof(batch) - 1u,
                                                 values, 2u));
    assert(!ksd_wayland_hypr_parse_opacity_batch(batch, sizeof(batch) - 1u,
                                                 values, 4u));
}

static bool confirm(void *context, pid_t pid)
{
    (void)pid;
    return *(bool *)context;
}

static void test_signals_only_a_confirmed_owner(void)
{
    bool yes = true;
    bool no = false;
    int status;
    pid_t child = fork();

    assert(child >= 0);
    if (child == 0) {
        /* Bounded, so a failed assertion cannot leave it holding
         * ctest's pipes. */
        alarm(30u);
        pause();
        _exit(0);
    }
    assert(ksd_wayland_hypr_signal_owner(child, confirm, &no)
           == KSD_STATUS_NOT_FOUND);
    assert(waitpid(child, &status, WNOHANG) == 0);
    assert(ksd_wayland_hypr_signal_owner(child, confirm, &yes)
           == KSD_STATUS_OK);
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    /* Reaped, so the pid now names nothing. */
    assert(ksd_wayland_hypr_signal_owner(child, confirm, &yes)
           == KSD_STATUS_NOT_FOUND);
    assert(ksd_wayland_hypr_signal_owner(-1, confirm, &yes)
           == KSD_STATUS_UNAVAILABLE);
    assert(ksd_wayland_hypr_signal_owner(0, confirm, &yes)
           == KSD_STATUS_UNAVAILABLE);
    assert(ksd_wayland_hypr_signal_owner(1, confirm, &yes)
           == KSD_STATUS_UNAVAILABLE);
    assert(ksd_wayland_hypr_signal_owner(getpid(), confirm, &yes)
           == KSD_STATUS_UNAVAILABLE);
}

static const char devices_reply[] =
    "{\n\"mice\": [{\"address\": \"0x1\", \"scrollFactor\": -1.00}],\n"
    "\"keyboards\": [{\"address\": \"0x2\", \"name\": \"power-button\","
    " \"active_layout_index\": 0, \"active_keymap\": \"English (US)\","
    " \"main\": false},{\"main\": true, \"name\": \"kbd\","
    " \"active_keymap\": \"German\", \"active_layout_index\": 1},"
    "{\"name\": \"keysharp-virtual-input\", \"active_layout_index\": \"x\","
    " \"active_keymap\": \"English (US)\", \"main\": false}],\n"
    "\"tablets\": [\n\n],\n\"touch\": [\n\n],\n\"switches\": [\n\n]\n}\n";

static void test_parses_main_keyboard_layout(void)
{
    static const char *const rejected[] = {
        "", "[]", "{", "{\"keyboards\":{}}", "{\"keyboards\":[1]}",
        "{\"keyboards\":[{\"main\":true,\"active_keymap\":\"a\"}]}",
        "{\"keyboards\":[{\"main\":true,\"active_layout_index\":-1,"
        "\"active_keymap\":\"a\"}]}",
        "{\"keyboards\":[{\"main\":true,\"active_layout_index\":4294967296,"
        "\"active_keymap\":\"a\"}]}",
        "{\"keyboards\":[{\"main\":false,\"active_layout_index\":0,"
        "\"active_keymap\":\"a\"}]}",
        "{\"keyboards\":[{\"main\":true,\"active_layout_index\":0,"
        "\"active_keymap\":\"a\"},{\"main\":true,\"active_layout_index\":1,"
        "\"active_keymap\":\"b\"}]}",
        "{\"keyboards\":[{\"main\":true,\"active_layout_index\":0,"
        "\"active_keymap\":\"a\"}",
    };
    ksd_hypr_layout layout;

    assert(ksd_wayland_hypr_parse_active_layout(devices_reply,
                                                sizeof(devices_reply) - 1u,
                                                &layout));
    assert(layout.index == 1u && strcmp(layout.name, "German") == 0);
    for (size_t index = 0u; index < sizeof(rejected) / sizeof(rejected[0]);
         index++)
        assert(!ksd_wayland_hypr_parse_active_layout(
            rejected[index], strlen(rejected[index]), &layout));
}

static void test_keyboard_group_requires_matching_layout(void)
{
    static const char map[] =
        "xkb_keymap { xkb_keycodes { minimum=8; maximum=255; <AD01>=24; };"
        "xkb_types { type \"ONE_LEVEL\" { modifiers=None; map[None]=Level1; }; };"
        "xkb_compatibility {}; xkb_symbols { name[Group1]=\"ksd one\";"
        " name[Group2]=\"ksd two\";"
        " key <AD01> { type=\"ONE_LEVEL\", symbols[Group1]=[q],"
        " symbols[Group2]=[w] }; }; };";
    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_ENVIRONMENT_NAMES
                                                  | XKB_CONTEXT_NO_DEFAULT_INCLUDES);
    struct xkb_keymap *keymap;
    ksd_hypr_layout layout = { .index = 1u };
    uint32_t group = 99u;

    assert(context != NULL);
    keymap = xkb_keymap_new_from_string(context, map, XKB_KEYMAP_FORMAT_TEXT_V1,
                                        XKB_KEYMAP_COMPILE_NO_FLAGS);
    assert(keymap != NULL && xkb_keymap_num_layouts(keymap) == 2u);
    strcpy(layout.name, "ksd two");
    assert(ksd_wayland_keyboard_group(keymap, &layout, &group) && group == 1u);
    strcpy(layout.name, "ksd one");
    assert(!ksd_wayland_keyboard_group(keymap, &layout, &group));
    layout.index = 2u;
    strcpy(layout.name, "ksd two");
    assert(!ksd_wayland_keyboard_group(keymap, &layout, &group));
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
}

static const char monitors_reply[] =
    "[{\n    \"id\": 0,\n    \"name\": \"Virtual-1\",\n    \"width\": 2433,\n"
    "    \"height\": 1481,\n    \"refreshRate\": 59.99900,\n    \"x\": 0,\n"
    "    \"y\": 0,\n    \"activeWorkspace\": {\"id\": 1, \"name\": \"1\"},\n"
    "    \"reserved\": [0, 30, 0, 0],\n    \"scale\": 2.00,\n"
    "    \"transform\": 0,\n    \"focused\": true,\n"
    "    \"solitary\": \"0\",\n    \"solitaryBlockedBy\": [\"WINDOWED\"],\n"
    "    \"disabled\": false,\n    \"availableModes\": [\"2433x1481@60.00Hz\"]\n"
    "},{\"name\": \"HDMI-A-1\", \"x\": -1920, \"y\": 0,"
    " \"reserved\": [10, 0, 0, 40], \"focused\": false, \"disabled\": false},"
    "{\"name\": \"DP-9\", \"x\": 0, \"y\": 0, \"reserved\": [0, 0, 0, 0],"
    " \"disabled\": true}]";

static void test_parses_monitors(void)
{
    static const char short_reserved[] =
        "[{\"name\":\"a\",\"x\":0,\"y\":0,\"reserved\":[0,0,0]}]";
    static const char unterminated[] = "[{\"name\":\"a\"";
    ksd_hypr_monitor *monitors;
    size_t count;

    assert(ksd_wayland_hypr_parse_monitors(monitors_reply,
                                           sizeof(monitors_reply) - 1u,
                                           &monitors, &count));
    assert(count == 2u);
    assert(strcmp(monitors[0].name, "Virtual-1") == 0);
    assert(monitors[0].x == 0 && monitors[0].y == 0 && monitors[0].focused);
    assert(monitors[0].reserved[0] == 0 && monitors[0].reserved[1] == 30
           && monitors[0].reserved[2] == 0 && monitors[0].reserved[3] == 0);
    assert(strcmp(monitors[1].name, "HDMI-A-1") == 0 && monitors[1].x == -1920);
    free(monitors);
    assert(!ksd_wayland_hypr_parse_monitors("{}", 2u, &monitors, &count));
    assert(!ksd_wayland_hypr_parse_monitors(short_reserved,
                                            strlen(short_reserved), &monitors,
                                            &count));
    assert(!ksd_wayland_hypr_parse_monitors(unterminated, strlen(unterminated),
                                            &monitors, &count));
}

static void test_reserved_work_area(void)
{
    static const int32_t top_bar[4] = { 0, 30, 0, 0 };
    static const int32_t left_and_bottom[4] = { 10, 0, 0, 40 };
    static const int32_t negative[4] = { -5, -5, 0, 0 };
    static const int32_t everything[4] = { 0, 741, 0, 0 };
    int32_t area[4];

    /* Logical bounds at scale 2, never the 2433x1481 mode. */
    assert(ksd_wayland_reserved_work_area(0, 0, 1217, 741, top_bar, area));
    assert(area[0] == 0 && area[1] == 30 && area[2] == 1217 && area[3] == 711);
    assert(ksd_wayland_reserved_work_area(-1920, 0, 1920, 1080,
                                          left_and_bottom, area));
    assert(area[0] == -1910 && area[1] == 0 && area[2] == 1910
           && area[3] == 1040);
    assert(ksd_wayland_reserved_work_area(0, 0, 100, 100, negative, area));
    assert(area[0] == 0 && area[1] == 0 && area[2] == 100 && area[3] == 100);
    assert(!ksd_wayland_reserved_work_area(0, 0, 1217, 741, everything, area));
}

typedef struct event_log {
    char lines[8][64];
    size_t count;
} event_log;

static void record_line(void *context, const char *name)
{
    event_log *log = context;

    assert(log->count < 8u);
    snprintf(log->lines[log->count++], sizeof(log->lines[0]), "%s", name);
}

static void test_reads_event_lines(void)
{
    static const char first[] = "openwindow>>5bf0,1,foot,a,b\nactive";
    static const char second[] = "windowv2>>\nnoseparator\n";
    ksd_hypr_event_reader reader;
    event_log log = { .count = 0u };
    char oversized[KSD_HYPR_EVENT_LINE_CAPACITY + 16u];

    ksd_wayland_hypr_event_reader_init(&reader);
    ksd_wayland_hypr_event_feed(&reader, first, strlen(first), record_line,
                                &log);
    ksd_wayland_hypr_event_feed(&reader, second, strlen(second), record_line,
                                &log);
    assert(log.count == 3u);
    assert(strcmp(log.lines[0], "openwindow") == 0);
    assert(strcmp(log.lines[1], "activewindowv2") == 0);
    assert(strcmp(log.lines[2], "") == 0);
    memset(oversized, 'x', sizeof(oversized) - 1u);
    oversized[sizeof(oversized) - 1u] = '\n';
    ksd_wayland_hypr_event_feed(&reader, oversized, sizeof(oversized),
                                record_line, &log);
    ksd_wayland_hypr_event_feed(&reader, "bell>>\n", 7u, record_line, &log);
    assert(log.count == 5u);
    assert(strcmp(log.lines[3], "") == 0);
    assert(strcmp(log.lines[4], "bell") == 0);
    assert(ksd_wayland_hypr_event_affects_windows("openwindow"));
    assert(ksd_wayland_hypr_event_affects_windows("movewindowv2"));
    assert(ksd_wayland_hypr_event_affects_windows(""));
    assert(!ksd_wayland_hypr_event_affects_windows("bell"));
    assert(!ksd_wayland_hypr_event_affects_windows("activelayout"));
}

typedef struct diff_log {
    uint16_t kinds[16];
    uint64_t ids[16];
    size_t count;
} diff_log;

static void record_event(void *context, uint16_t kind, uint64_t id)
{
    diff_log *log = context;

    assert(log->count < 16u);
    log->kinds[log->count] = kind;
    log->ids[log->count++] = id;
}

static void test_diffs_window_snapshots(void)
{
    ksd_watch_window previous[] = {
        { .id = 1u, .title = "one", .x = 0, .y = 0, .width = 10, .height = 10 },
        { .id = 2u, .title = "two", .x = 0, .y = 0, .width = 10, .height = 10 },
        { .id = 3u, .title = "three", .x = 5, .y = 5, .width = 10,
          .height = 10 },
    };
    ksd_watch_window next[] = {
        { .id = 1u, .title = "one!", .minimized = true, .x = 1, .y = 0,
          .width = 10, .height = 10 },
        { .id = 3u, .title = "three", .x = 5, .y = 5, .width = 10,
          .height = 10 },
        { .id = 4u, .title = "four" },
    };
    static const uint16_t expected[] = {
        KSD_WINDOW_EVENT_TITLE, KSD_WINDOW_EVENT_MINIMIZE,
        KSD_WINDOW_EVENT_MOVE, KSD_WINDOW_EVENT_CREATE,
        KSD_WINDOW_EVENT_CLOSE, KSD_WINDOW_EVENT_ACTIVE_STATE,
        KSD_WINDOW_EVENT_ACTIVE,
    };
    static const uint64_t expected_ids[] = { 1u, 1u, 1u, 4u, 2u, 1u, 3u };
    diff_log log = { .count = 0u };

    ksd_wayland_watch_diff(previous, 3u, 1u, next, 3u, 3u, record_event, &log);
    assert(log.count == sizeof(expected) / sizeof(expected[0]));
    for (size_t index = 0u; index < log.count; index++)
        assert(log.kinds[index] == expected[index]
               && log.ids[index] == expected_ids[index]);

    /* Focus leaving for no window deactivates only; a vanished active window
     * is covered by its close. */
    log.count = 0u;
    ksd_wayland_watch_diff(next, 3u, 3u, next, 3u, 0u, record_event, &log);
    assert(log.count == 1u && log.kinds[0] == KSD_WINDOW_EVENT_ACTIVE_STATE
           && log.ids[0] == 3u);
    log.count = 0u;
    ksd_wayland_watch_diff(next, 3u, 4u, next, 2u, 0u, record_event, &log);
    assert(log.count == 1u && log.kinds[0] == KSD_WINDOW_EVENT_CLOSE
           && log.ids[0] == 4u);
    log.count = 0u;
    ksd_wayland_watch_diff(next, 3u, 3u, next, 3u, 3u, record_event, &log);
    assert(log.count == 0u);
}

int main(void)
{
    test_parses_clients();
    test_rejects_malformed_documents();
    test_rejects_truncated_identifiers();
    test_opacity_text_round_trips();
    test_parses_opacity_replies();
    test_signals_only_a_confirmed_owner();
    test_parses_main_keyboard_layout();
    test_keyboard_group_requires_matching_layout();
    test_parses_monitors();
    test_reserved_work_area();
    test_reads_event_lines();
    test_diffs_window_snapshots();
    return 0;
}
