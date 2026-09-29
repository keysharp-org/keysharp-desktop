#include "wl_hypr_windows.h"

#include "protocol.h"
#include "protocol_io.h"
#include "wl_internal.h"
#include "wl_windows.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Hyprland has no minimized state. Parking a window on this special
 * workspace is the convention its own tooling uses for one. */
#define KSD_HYPR_MINIMIZED_WORKSPACE "special:minimized"
#define KSD_HYPR_COMMAND_CAPACITY 256u

bool ksd_wayland_hypr_windows_available(const ksd_wayland *connection)
{
    return connection != NULL && connection->toplevel_list != NULL
        && connection->hypr;
}

static bool minimized(const ksd_wl_toplevel *item)
{
    return strcmp(item->hypr.workspace_name,
                  KSD_HYPR_MINIMIZED_WORKSPACE) == 0;
}

/* On screen now: mapped, not a background group tab, not parked, and on a
 * workspace some monitor is showing. */
static bool shown(const ksd_wl_toplevel *item)
{
    return item->hypr_known && item->hypr.mapped && !item->hypr.hidden
        && item->hypr.visible && !minimized(item);
}

static bool usable(const ksd_wl_toplevel *item)
{
    return item->handle != NULL && !item->closed && item->identifier != NULL
        && item->hypr_known && item->hypr.mapped;
}

static uint32_t state(const ksd_wl_toplevel *item)
{
    uint32_t value = 0u;

    if (minimized(item))
        value |= KSD_WL_TOPLEVEL_STATE_MINIMIZED;
    if (item->hypr.fullscreen != 0)
        value |= KSD_WL_TOPLEVEL_STATE_MAXIMIZED;
    if (item->hypr.focus_history == 0 && shown(item))
        value |= KSD_WL_TOPLEVEL_STATE_ACTIVATED;
    return value;
}

static bool append_flag(ksd_buffer *out, const char *name, bool value)
{
    char text[48];
    int length = snprintf(text, sizeof(text), ",\"%s\":%s", name,
                          value ? "true" : "false");

    return length > 0 && (size_t)length < sizeof(text)
        && ksd_buffer_bytes(out, text, (size_t)length);
}

/* Reads every usable window's opacity in one batched request, once per
 * refresh. A failed read leaves the value unknown rather than failing the
 * listing it is part of. */
static void read_opacities(ksd_wayland *connection)
{
    const char **addresses;
    int32_t *values;
    ksd_wl_toplevel **items;
    size_t count = 0u;
    size_t used = 0u;

    if (connection->hypr_opacity_fresh)
        return;
    connection->hypr_opacity_fresh = true;
    for (ksd_wl_toplevel *item = connection->toplevels; item != NULL;
         item = item->next) {
        item->hypr_opacity = -1;
        if (usable(item))
            count++;
    }
    if (count == 0u)
        return;
    addresses = calloc(count, sizeof(*addresses));
    values = calloc(count, sizeof(*values));
    items = calloc(count, sizeof(*items));
    if (addresses != NULL && values != NULL && items != NULL) {
        for (ksd_wl_toplevel *item = connection->toplevels;
             item != NULL && used < count; item = item->next) {
            if (!usable(item))
                continue;
            items[used] = item;
            addresses[used++] = item->hypr.address;
        }
        if (ksd_wayland_hypr_opacities(connection->session_pid, addresses,
                                       used, values))
            for (size_t index = 0u; index < used; index++)
                items[index]->hypr_opacity = values[index];
    }
    free(addresses);
    free(values);
    free(items);
}

static bool append_window(ksd_buffer *out, ksd_wayland *connection,
                          const ksd_wl_toplevel *item)
{
    char text[384];
    const ksd_hypr_client *client = &item->hypr;
    const char *title = item->title == NULL ? "" : item->title;
    const char *app_id = item->app_id == NULL ? "" : item->app_id;
    uint32_t flags = state(item);
    bool has_pid = client->pid > 0;
    int length;

    read_opacities(connection);
    length = snprintf(text, sizeof(text), "{\"id\":\"%llu\"",
                      (unsigned long long)item->id);
    if (length <= 0 || (size_t)length >= sizeof(text)
        || !ksd_buffer_bytes(out, text, (size_t)length)
        || !ksd_buffer_bytes(out, ",\"compositorId\":", 16u)
        || !ksd_buffer_json_string(out, item->identifier,
                                   strlen(item->identifier), false)
        || !ksd_buffer_bytes(out, ",\"captureId\":", 13u)
        || !ksd_buffer_json_string(out, item->identifier,
                                   strlen(item->identifier), false)
        || !ksd_buffer_bytes(out, ",\"title\":", 9u)
        || !ksd_buffer_json_string(out, title, strlen(title), false)
        || !ksd_buffer_bytes(out, ",\"appId\":", 9u)
        || !ksd_buffer_json_string(out, app_id, strlen(app_id), false))
        return false;
    /* Hyprland draws the border outside this box and the client paints all
     * of it, so the frame and the client area are the same rectangle. */
    length = snprintf(text, sizeof(text),
        ",\"frame\":{\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d}"
        ",\"client\":{\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d}",
        client->x, client->y, client->width, client->height,
        client->x, client->y, client->width, client->height);
    if (length <= 0 || (size_t)length >= sizeof(text)
        || !ksd_buffer_bytes(out, text, (size_t)length))
        return false;
    if (has_pid) {
        length = snprintf(text, sizeof(text), ",\"pid\":%lld",
                          (long long)client->pid);
        if (length <= 0 || (size_t)length >= sizeof(text)
            || !ksd_buffer_bytes(out, text, (size_t)length))
            return false;
    }
    if (item->hypr_opacity >= 0) {
        length = snprintf(text, sizeof(text), ",\"transparency\":%d",
                          item->hypr_opacity);
        if (length <= 0 || (size_t)length >= sizeof(text)
            || !ksd_buffer_bytes(out, text, (size_t)length))
            return false;
    }
    /* A parked window counts as being on the current workspace, the way a
     * minimized window on any other desktop stays in its place. */
    return append_flag(out, "active",
                       (flags & KSD_WL_TOPLEVEL_STATE_ACTIVATED) != 0u)
        && append_flag(out, "minimized",
                       (flags & KSD_WL_TOPLEVEL_STATE_MINIMIZED) != 0u)
        && append_flag(out, "maximized",
                       (flags & KSD_WL_TOPLEVEL_STATE_MAXIMIZED) != 0u)
        && append_flag(out, "visible", !client->hidden)
        && append_flag(out, "alwaysOnTop", client->floating)
        && append_flag(out, "onCurrentWorkspace",
                       client->visible || minimized(item))
        && ksd_buffer_bytes(out,
            ",\"validFields\":[\"id\",\"compositorId\",\"captureId\",\"title\","
            "\"appId\",\"frame\",\"client\",\"active\",\"minimized\","
            "\"maximized\",\"visible\",\"alwaysOnTop\",\"onCurrentWorkspace\"",
            sizeof(",\"validFields\":[\"id\",\"compositorId\",\"captureId\",\"title\","
                   "\"appId\",\"frame\",\"client\",\"active\",\"minimized\","
                   "\"maximized\",\"visible\",\"alwaysOnTop\","
                   "\"onCurrentWorkspace\"") - 1u)
        && (!has_pid || ksd_buffer_bytes(out, ",\"pid\"", 6u))
        && (item->hypr_opacity < 0
            || ksd_buffer_bytes(out, ",\"transparency\"",
                                sizeof(",\"transparency\"") - 1u))
        && ksd_buffer_bytes(out, "]}", 2u);
}

const ksd_wayland_window_view *ksd_wayland_hypr_window_view(void)
{
    static const ksd_wayland_window_view view = {
        .usable = usable,
        .state = state,
        .append_window = append_window,
    };

    return &view;
}

/* Hyprland reports no stacking order. Its rule is that an open special
 * workspace sits above the regular one, floating and full screen windows sit
 * above tiled ones, and among equals the most recently focused is on top,
 * which focus history captures. An explicit raise or lower is not visible in
 * focus history, so its stamp orders the window until focus moves again.
 *
 * The list runs bottom to top, the order X11 stacking lists use, so a
 * negative result puts a below b. */
static int stack_compare(const ksd_wl_toplevel *a, const ksd_wl_toplevel *b)
{
    bool a_shown;
    bool b_shown;
    bool a_special;
    bool b_special;
    bool a_raised;
    bool b_raised;

    if (a->hypr_known != b->hypr_known)
        return a->hypr_known ? 1 : -1;
    if (!a->hypr_known)
        return 0;
    a_shown = shown(a);
    b_shown = shown(b);
    if (a_shown != b_shown)
        return a_shown ? 1 : -1;
    a_special = strncmp(a->hypr.workspace_name, "special:", 8u) == 0;
    b_special = strncmp(b->hypr.workspace_name, "special:", 8u) == 0;
    if (a_special != b_special)
        return a_special ? 1 : -1;
    a_raised = a->hypr.floating || a->hypr.fullscreen != 0;
    b_raised = b->hypr.floating || b->hypr.fullscreen != 0;
    if (a_raised != b_raised)
        return a_raised ? 1 : -1;
    if (a->hypr_stack_stamp != b->hypr_stack_stamp)
        return a->hypr_stack_stamp > b->hypr_stack_stamp ? 1 : -1;
    return (a->hypr.focus_history < b->hypr.focus_history)
        - (a->hypr.focus_history > b->hypr.focus_history);
}

static void sort_by_stack(ksd_wayland *connection)
{
    ksd_wl_toplevel *sorted = NULL;
    ksd_wl_toplevel *item = connection->toplevels;

    /* Stable insertion: equal ranks keep the order the compositor sent. */
    while (item != NULL) {
        ksd_wl_toplevel *next = item->next;
        ksd_wl_toplevel **slot = &sorted;

        while (*slot != NULL && stack_compare(*slot, item) <= 0)
            slot = &(*slot)->next;
        item->next = *slot;
        *slot = item;
        item = next;
    }
    connection->toplevels = sorted;
}

/* Focusing a window raises it above every earlier stamp. Stamps exist only
 * once something has been restacked explicitly; until then focus history
 * alone orders the list. Every window focused since the last refresh was
 * raised in turn, so each is stamped, oldest first. */
static void note_focus_change(ksd_wayland *connection)
{
    ksd_wl_toplevel *active = NULL;
    int32_t since = 1;

    for (ksd_wl_toplevel *item = connection->toplevels; item != NULL;
         item = item->next) {
        if (!item->hypr_known)
            continue;
        if (active == NULL && item->hypr.focus_history == 0 && shown(item))
            active = item;
        if (strcmp(item->hypr.stable_id, connection->hypr_last_active) == 0
            && item->hypr.focus_history > 0)
            since = item->hypr.focus_history;
    }
    if (active == NULL
        || strcmp(active->hypr.stable_id, connection->hypr_last_active) == 0)
        return;
    memcpy(connection->hypr_last_active, active->hypr.stable_id,
           sizeof(connection->hypr_last_active));
    if (connection->hypr_stack_sequence == 0)
        return;
    for (int32_t history = since - 1; history >= 0; history--)
        for (ksd_wl_toplevel *item = connection->toplevels; item != NULL;
             item = item->next)
            if (item->hypr_known && item->hypr.focus_history == history)
                item->hypr_stack_stamp = ++connection->hypr_stack_sequence;
}

bool ksd_wayland_hypr_windows_refresh(ksd_wayland *connection,
                                      ksd_operation_result *result)
{
    ksd_hypr_client *clients = NULL;
    size_t count = 0u;

    if (!ksd_wayland_hypr_clients(connection->session_pid, &clients,
                                  &count)) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                         "Hyprland did not answer the window query");
        return false;
    }
    for (ksd_wl_toplevel *item = connection->toplevels; item != NULL;
         item = item->next) {
        item->hypr_known = false;
        if (item->handle == NULL || item->identifier == NULL)
            continue;
        for (size_t index = 0u; index < count; index++) {
            if (strcmp(clients[index].stable_id, item->identifier) == 0) {
                item->hypr = clients[index];
                item->hypr_known = true;
                break;
            }
        }
        /* Restored by some other means, so the parked origin is stale. */
        if (item->hypr_known && !minimized(item))
            item->hypr_has_restore = false;
    }
    connection->hypr_opacity_fresh = false;
    free(clients);
    note_focus_change(connection);
    sort_by_stack(connection);
    return true;
}

static bool format_command(char out[KSD_HYPR_COMMAND_CAPACITY],
                           const char *format, ...)
    __attribute__((format(printf, 2, 3)));

static bool format_command(char out[KSD_HYPR_COMMAND_CAPACITY],
                           const char *format, ...)
{
    va_list arguments;
    int length;

    va_start(arguments, format);
    length = vsnprintf(out, KSD_HYPR_COMMAND_CAPACITY, format, arguments);
    va_end(arguments);
    return length > 0 && (size_t)length < KSD_HYPR_COMMAND_CAPACITY;
}

static bool dispatch(ksd_wayland *connection, const char *lua,
                     const char *legacy)
{
    return ksd_wayland_hypr_dispatch(connection->session_pid, lua, legacy);
}

static bool focus(ksd_wayland *connection, const ksd_wl_toplevel *window)
{
    char lua[KSD_HYPR_COMMAND_CAPACITY];
    char legacy[KSD_HYPR_COMMAND_CAPACITY];

    return format_command(lua, "hl.dsp.focus({ window = 'address:%s' })",
                          window->hypr.address)
        && format_command(legacy, "focuswindow address:%s",
                          window->hypr.address)
        && dispatch(connection, lua, legacy);
}

/* The classic fullscreenstate dispatcher acts on the focused window only, so
 * that spelling focuses the target first. */
static bool set_fullscreen(ksd_wayland *connection,
                           const ksd_wl_toplevel *window, int mode)
{
    char lua[KSD_HYPR_COMMAND_CAPACITY];
    char legacy[KSD_HYPR_COMMAND_CAPACITY];

    if (!format_command(lua,
            "hl.dsp.window.fullscreen_state({ internal = %d, client = %d,"
            " window = 'address:%s' })", mode, mode, window->hypr.address)
        || !format_command(legacy, "fullscreenstate %d %d", mode, mode))
        return false;
    if (dispatch(connection, lua, NULL))
        return true;
    return focus(connection, window) && dispatch(connection, NULL, legacy);
}

static bool workspace_name_valid(const char *name)
{
    if (name[0] == 0 || strncmp(name, "special:", 8u) == 0)
        return false;
    for (const char *at = name; *at != 0; at++)
        if ((unsigned char)*at < 0x20u || *at == '\'' || *at == '"'
            || *at == '\\' || *at == ',')
            return false;
    return true;
}

static bool unminimize(ksd_wayland *connection, ksd_wl_toplevel *window)
{
    char lua[KSD_HYPR_COMMAND_CAPACITY];
    char legacy[KSD_HYPR_COMMAND_CAPACITY];
    int64_t workspace;
    bool formatted;

    if (!minimized(window))
        return true;
    if (window->hypr_has_restore && window->hypr_restore_workspace > 0)
        workspace = window->hypr_restore_workspace;
    else if (window->hypr_has_restore
             && workspace_name_valid(window->hypr_restore_name))
        workspace = 0;
    else if (!ksd_wayland_hypr_active_workspace(connection->session_pid,
                                                &workspace)
             || workspace <= 0)
        return false;
    if (workspace > 0)
        formatted = format_command(lua,
                "hl.dsp.window.move({ workspace = %lld, follow = false,"
                " window = 'address:%s' })",
                (long long)workspace, window->hypr.address)
            && format_command(legacy, "movetoworkspacesilent %lld,address:%s",
                              (long long)workspace, window->hypr.address);
    else
        formatted = format_command(lua,
                "hl.dsp.window.move({ workspace = 'name:%s', follow = false,"
                " window = 'address:%s' })",
                window->hypr_restore_name, window->hypr.address)
            && format_command(legacy,
                              "movetoworkspacesilent name:%s,address:%s",
                              window->hypr_restore_name, window->hypr.address);
    if (!formatted || !dispatch(connection, lua, legacy))
        return false;
    window->hypr_has_restore = false;
    return true;
}

static bool minimize(ksd_wayland *connection, ksd_wl_toplevel *window)
{
    char lua[KSD_HYPR_COMMAND_CAPACITY];
    char legacy[KSD_HYPR_COMMAND_CAPACITY];

    if (minimized(window))
        return true;
    if (!format_command(lua,
            "hl.dsp.window.move({ workspace = '" KSD_HYPR_MINIMIZED_WORKSPACE
            "', follow = false, window = 'address:%s' })",
            window->hypr.address)
        || !format_command(legacy,
            "movetoworkspacesilent " KSD_HYPR_MINIMIZED_WORKSPACE
            ",address:%s", window->hypr.address))
        return false;
    window->hypr_has_restore =
        strncmp(window->hypr.workspace_name, "special:", 8u) != 0;
    window->hypr_restore_workspace = window->hypr.workspace_id;
    memcpy(window->hypr_restore_name, window->hypr.workspace_name,
           sizeof(window->hypr_restore_name));
    if (dispatch(connection, lua, legacy))
        return true;
    window->hypr_has_restore = false;
    return false;
}

static bool set_state(ksd_wayland *connection, ksd_wl_toplevel *window,
                      uint32_t value)
{
    switch (value) {
        case 1u:
            return minimize(connection, window);
        case 2u:
            return unminimize(connection, window)
                && (window->hypr.fullscreen != 0
                    || set_fullscreen(connection, window, 1));
        case 3u:
            return unminimize(connection, window);
        default:
            return unminimize(connection, window)
                && (window->hypr.fullscreen == 0
                    || set_fullscreen(connection, window, 0));
    }
}

static bool restack(ksd_wayland *connection, ksd_wl_toplevel *window,
                    bool top)
{
    char lua[KSD_HYPR_COMMAND_CAPACITY];
    char legacy[KSD_HYPR_COMMAND_CAPACITY];
    const char *mode = top ? "top" : "bottom";
    int64_t stamp;

    if (!format_command(lua,
            "hl.dsp.window.alter_zorder({ mode = '%s', window = 'address:%s' })",
            mode, window->hypr.address)
        || !format_command(legacy, "alterzorder %s,address:%s", mode,
                           window->hypr.address)
        || !dispatch(connection, lua, legacy))
        return false;
    stamp = ++connection->hypr_stack_sequence;
    window->hypr_stack_stamp = top ? stamp : -stamp;
    return true;
}

/* Hyprland has no keep-above, but floating windows always stack above tiled
 * ones, so the floating layer is what above means here and leaving it means
 * rejoining the layout. Both float actions are idempotent, so a cached state
 * that is out of date cannot turn one into a toggle. */
static bool set_above(ksd_wayland *connection, ksd_wl_toplevel *window,
                      bool above)
{
    char lua[KSD_HYPR_COMMAND_CAPACITY];
    char legacy[KSD_HYPR_COMMAND_CAPACITY];

    if (!format_command(lua,
            "hl.dsp.window.float({ action = '%s', window = 'address:%s' })",
            above ? "enable" : "disable", window->hypr.address)
        || !format_command(legacy, "%s address:%s",
                           above ? "setfloating" : "settiled",
                           window->hypr.address)
        || !dispatch(connection, lua, legacy))
        return false;
    window->hypr.floating = above;
    return !above || restack(connection, window, true);
}

static bool close_window(ksd_wayland *connection,
                         const ksd_wl_toplevel *window)
{
    char lua[KSD_HYPR_COMMAND_CAPACITY];
    char legacy[KSD_HYPR_COMMAND_CAPACITY];

    return format_command(lua,
            "hl.dsp.window.close({ window = 'address:%s' })",
            window->hypr.address)
        && format_command(legacy, "closewindow address:%s",
                          window->hypr.address)
        && dispatch(connection, lua, legacy);
}

/* Hyprland keeps separate active, inactive and full screen opacities, each
 * multiplied by the configured one unless overridden. A transparency level
 * sets all three absolutely; opaque clears the overrides so the configured
 * look returns. */
static bool set_opacity(ksd_wayland *connection, ksd_wl_toplevel *window,
                        uint32_t opacity)
{
    static const char *const props[][2] = {
        { "opacity", "opacity_override" },
        { "opacity_inactive", "opacity_inactive_override" },
        { "opacity_fullscreen", "opacity_fullscreen_override" },
    };
    char value[32];
    char lua[KSD_HYPR_COMMAND_CAPACITY];
    char legacy[KSD_HYPR_COMMAND_CAPACITY];
    const char *address = window->hypr.address;
    const char *override = opacity < 255u ? "1" : "0";
    int32_t applied = -1;

    if (!ksd_wayland_hypr_opacity_text(opacity, value, sizeof(value)))
        return false;
    for (size_t index = 0u; index < sizeof(props) / sizeof(props[0]);
         index++) {
        if (!format_command(lua,
                "hl.dsp.window.set_prop({ prop = '%s', value = '%s',"
                " window = 'address:%s' })", props[index][0], value, address)
            || !format_command(legacy, "setprop address:%s %s %s", address,
                               props[index][0], value)
            || !dispatch(connection, lua, legacy)
            || !format_command(lua,
                "hl.dsp.window.set_prop({ prop = '%s', value = '%s',"
                " window = 'address:%s' })", props[index][1], override,
                address)
            || !format_command(legacy, "setprop address:%s %s %s", address,
                               props[index][1], override)
            || !dispatch(connection, lua, legacy))
            return false;
    }
    /* A set_prop reply does not prove the property exists, so the value is
     * read back. */
    if (!ksd_wayland_hypr_opacities(connection->session_pid, &address, 1u,
                                    &applied)
        || applied != (int32_t)opacity)
        return false;
    window->hypr_opacity = applied;
    return true;
}

typedef struct kill_target {
    const ksd_wayland *connection;
    const char *stable_id;
} kill_target;

static bool still_owner(void *context, pid_t pid)
{
    const kill_target *target = context;
    ksd_hypr_client *clients = NULL;
    size_t count = 0u;
    bool owner = false;

    if (!ksd_wayland_hypr_clients(target->connection->session_pid, &clients,
                                  &count))
        return false;
    for (size_t index = 0u; index < count && !owner; index++)
        owner = clients[index].pid == pid && clients[index].mapped
            && strcmp(clients[index].stable_id, target->stable_id) == 0;
    free(clients);
    return owner;
}

/* Hyprland's own kill dispatcher sends SIGKILL to whatever pid the window
 * reports, which is -1 once its surface is gone and would take every process
 * of this user with it. The process is signalled here instead, after checking
 * that it still owns the window. */
static uint32_t kill_owner(const ksd_wayland *connection,
                           const ksd_wl_toplevel *window)
{
    kill_target target = {
        .connection = connection,
        .stable_id = window->hypr.stable_id,
    };

    if (window->hypr.pid <= 1 || window->hypr.pid > INT_MAX)
        return KSD_STATUS_UNAVAILABLE;
    return ksd_wayland_hypr_signal_owner((pid_t)window->hypr.pid, still_owner,
                                         &target);
}

void ksd_wayland_hypr_window_action(ksd_wayland *connection, uint16_t opcode,
                                    uint64_t handle, uint32_t value,
                                    ksd_operation_result *result)
{
    ksd_wl_toplevel *window = ksd_wayland_window_for_action(
        connection, handle, ksd_wayland_hypr_window_view(), result);
    bool done;

    if (window == NULL)
        return;
    switch (opcode) {
        case KSD_OP_WINDOW_FOCUS:
            /* Activating a minimized window restores it first. */
            done = unminimize(connection, window) && focus(connection, window);
            break;
        case KSD_OP_WINDOW_CLOSE:
            done = close_window(connection, window);
            break;
        case KSD_OP_WINDOW_SET_STATE:
            done = set_state(connection, window, value);
            break;
        case KSD_OP_WINDOW_RAISE:
            done = restack(connection, window, true);
            break;
        case KSD_OP_WINDOW_LOWER:
            done = restack(connection, window, false);
            break;
        case KSD_OP_WINDOW_SET_OPACITY:
            done = set_opacity(connection, window, value);
            break;
        case KSD_OP_WINDOW_SET_ABOVE:
            done = set_above(connection, window, value != 0u);
            break;
        case KSD_OP_WINDOW_KILL: {
            uint32_t status = kill_owner(connection, window);

            if (status == KSD_STATUS_OK)
                (void)ksd_result_copy(result, NULL, 0u);
            else
                ksd_result_error(result, status, 0u,
                                 status == KSD_STATUS_NOT_FOUND
                                 ? "the window no longer exists"
                                 : "Hyprland reports no process for this"
                                   " window");
            return;
        }
        default:
            ksd_result_error(result, KSD_STATUS_INVALID_REQUEST, 0u,
                             "invalid Wayland window operation");
            return;
    }
    if (!done) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                         "Hyprland rejected the window request");
        return;
    }
    (void)ksd_result_copy(result, NULL, 0u);
}

void ksd_wayland_hypr_window_move_resize(ksd_wayland *connection,
                                         uint64_t handle, int32_t x,
                                         int32_t y, uint32_t width,
                                         uint32_t height,
                                         ksd_operation_result *result)
{
    char lua[KSD_HYPR_COMMAND_CAPACITY];
    char legacy[KSD_HYPR_COMMAND_CAPACITY];
    ksd_wl_toplevel *window = ksd_wayland_window_for_action(
        connection, handle, ksd_wayland_hypr_window_view(), result);
    ksd_hypr_client current;
    int32_t target_x;
    int32_t target_y;
    uint32_t target_width;
    uint32_t target_height;
    bool exact;
    bool floated = false;
    bool resize;
    bool done = true;

    if (window == NULL)
        return;
    current = window->hypr;
    target_x = x == INT32_MIN ? current.x : x;
    target_y = y == INT32_MIN ? current.y : y;
    target_width = width == 0u ? (uint32_t)current.width : width;
    target_height = height == 0u ? (uint32_t)current.height : height;
    /* Nothing is sent that would change nothing, which the geometry read at
     * the start of the request can tell, except of a full screen window: that
     * describes it only until it is restored, so its request goes as it is. */
    exact = current.fullscreen == 0;
    if (!exact)
        done = set_fullscreen(connection, window, 0);
    /* A tiled window's box belongs to the layout, so placing one takes it out
     * of the layout first, at the box it already had. */
    if (done && !current.floating) {
        done = format_command(lua,
                "hl.dsp.window.float({ action = 'enable',"
                " window = 'address:%s' })", current.address)
            && format_command(legacy, "setfloating address:%s",
                              current.address)
            && dispatch(connection, lua, legacy);
        floated = done;
    }
    resize = floated
        || (exact ? target_width != (uint32_t)current.width
                        || target_height != (uint32_t)current.height
                  : width != 0u || height != 0u);
    if (done && resize)
        done = format_command(lua,
                "hl.dsp.window.resize({ x = %u, y = %u,"
                " window = 'address:%s' })",
                target_width, target_height, current.address)
            && format_command(legacy, "resizewindowpixel exact %u %u,address:%s",
                              target_width, target_height, current.address)
            && dispatch(connection, lua, legacy);
    /* Hyprland resizes a floating window about its centre, so a resize
     * restates the position: an unchanged corner stays where it was. */
    if (done && (resize
                 || (exact ? target_x != current.x || target_y != current.y
                           : x != INT32_MIN || y != INT32_MIN)))
        done = format_command(lua,
                "hl.dsp.window.move({ x = %d, y = %d,"
                " window = 'address:%s' })",
                target_x, target_y, current.address)
            && format_command(legacy, "movewindowpixel exact %d %d,address:%s",
                              target_x, target_y, current.address)
            && dispatch(connection, lua, legacy);
    if (!done) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                         "Hyprland rejected the window geometry");
        return;
    }
    (void)ksd_result_copy(result, NULL, 0u);
}

void ksd_wayland_hypr_window_at_point(ksd_wayland *connection, int32_t x,
                                      int32_t y, ksd_operation_result *result)
{
    const ksd_wayland_window_view *view = ksd_wayland_hypr_window_view();
    ksd_wl_toplevel *found = NULL;
    ksd_buffer out;
    bool ok;

    if (!ksd_wayland_windows_refresh(connection, view, result))
        return;
    /* The list runs bottom to top, so the last hit is the one on top. */
    for (ksd_wl_toplevel *item = connection->toplevels; item != NULL;
         item = item->next) {
        const ksd_hypr_client *client = &item->hypr;

        if (usable(item) && shown(item)
            && (int64_t)x >= client->x && (int64_t)y >= client->y
            && (int64_t)x < (int64_t)client->x + client->width
            && (int64_t)y < (int64_t)client->y + client->height)
            found = item;
    }
    ksd_buffer_init(&out, KSD_MAX_TEXT_BYTES);
    ok = ksd_buffer_bytes(&out, "{\"ok\":true,\"window\":", 20u)
        && (found == NULL ? ksd_buffer_bytes(&out, "null", 4u)
            : append_window(&out, connection, found))
        && ksd_buffer_bytes(&out, "}", 1u);
    if (!ok)
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "the window result is too large");
    else
        (void)ksd_result_take_framed_text(&out, result,
            KSD_STATUS_RESOURCE_EXHAUSTED, "the window result is too large");
    ksd_buffer_clear(&out);
}
