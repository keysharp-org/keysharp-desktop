#ifndef KEYSHARP_DESKTOP_WL_HYPR_H
#define KEYSHARP_DESKTOP_WL_HYPR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define KSD_HYPR_ID_CAPACITY 32u
#define KSD_HYPR_WORKSPACE_CAPACITY 64u

/* The facts Hyprland's j/clients reply gives about one window. stable_id is
 * the string the compositor also sends as the ext-foreign-toplevel
 * identifier, which is what joins a toplevel handle to these facts. */
typedef struct ksd_hypr_client {
    char stable_id[KSD_HYPR_ID_CAPACITY];
    char address[KSD_HYPR_ID_CAPACITY];
    char workspace_name[KSD_HYPR_WORKSPACE_CAPACITY];
    int64_t workspace_id;
    int64_t pid;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    int32_t fullscreen;
    int32_t focus_history;
    bool mapped;
    bool hidden;
    bool visible;
    bool floating;
} ksd_hypr_client;

/* The pid of the Hyprland instance the session's environment names, or -1
 * when there is none this user can reach. */
pid_t ksd_wayland_hypr_pid(pid_t session_pid);
bool ksd_wayland_hypr_cursor(pid_t session_pid, int32_t *x, int32_t *y);
bool ksd_wayland_hypr_move(pid_t session_pid, int32_t x, int32_t y);

/* Runs one dispatcher. Under a Lua configuration the IPC dispatch command
 * evaluates Lua, under a hyprlang one it takes the classic dispatcher syntax,
 * and nothing says in advance which is live. Both spellings are supplied and
 * the one that last succeeded is tried first. */
bool ksd_wayland_hypr_dispatch(pid_t session_pid, const char *lua,
                               const char *legacy);

/* Every client the compositor manages. The caller frees *clients. */
bool ksd_wayland_hypr_clients(pid_t session_pid, ksd_hypr_client **clients,
                              size_t *count);
bool ksd_wayland_hypr_active_workspace(pid_t session_pid, int64_t *id);

/* Parses a j/clients reply; exposed for tests. */
bool ksd_wayland_hypr_parse_clients(const char *json, size_t length,
                                    ksd_hypr_client **clients, size_t *count);

/* Window opacity as 0..255, the scale the window JSON and the set request
 * use. The text form is what a set_prop value takes and getprop returns. */
bool ksd_wayland_hypr_opacity_text(uint32_t opacity, char *out,
                                   size_t capacity);
bool ksd_wayland_hypr_parse_opacity(const char *text, size_t length,
                                    uint32_t *opacity);
/* Splits a batched getprop reply into count values; an entry that is not an
 * opacity becomes -1. */
bool ksd_wayland_hypr_parse_opacity_batch(const char *reply, size_t length,
                                          int32_t *values, size_t count);
bool ksd_wayland_hypr_opacities(pid_t session_pid,
                                const char *const *addresses, size_t count,
                                int32_t *values);

/* SIGKILLs pid through a pidfd. still_owner runs with the pidfd already held,
 * so a pid it confirms cannot be recycled before the signal lands. Returns a
 * KSD_STATUS value. */
uint32_t ksd_wayland_hypr_signal_owner(pid_t pid,
                                       bool (*still_owner)(void *context,
                                                           pid_t pid),
                                       void *context);

#define KSD_HYPR_LAYOUT_NAME_CAPACITY 256u

typedef struct ksd_hypr_layout {
    uint32_t index;
    char name[KSD_HYPR_LAYOUT_NAME_CAPACITY];
} ksd_hypr_layout;

/* The active layout of the keyboard Hyprland marks main, the seat keyboard
 * whose keymap and group clients receive. */
bool ksd_wayland_hypr_active_layout(pid_t session_pid,
                                    ksd_hypr_layout *layout);
bool ksd_wayland_hypr_parse_active_layout(const char *json, size_t length,
                                          ksd_hypr_layout *layout);

#define KSD_HYPR_MONITOR_NAME_CAPACITY 64u

/* reserved is left, top, right, bottom in logical pixels. */
typedef struct ksd_hypr_monitor {
    char name[KSD_HYPR_MONITOR_NAME_CAPACITY];
    int32_t x;
    int32_t y;
    int32_t reserved[4];
    bool focused;
} ksd_hypr_monitor;

bool ksd_wayland_hypr_monitors(pid_t session_pid, ksd_hypr_monitor **monitors,
                               size_t *count);
bool ksd_wayland_hypr_parse_monitors(const char *json, size_t length,
                                     ksd_hypr_monitor **monitors,
                                     size_t *count);

/* The event socket. Opening it before a snapshot means no change between the
 * two goes unseen. Returns a nonblocking descriptor or -1. */
int ksd_wayland_hypr_events_open(pid_t session_pid);

#define KSD_HYPR_EVENT_LINE_CAPACITY 256u

/* Splits the event stream into lines across reads for their event names. A
 * line longer than the buffer is dropped up to its newline and reported as an
 * unknown event, so a reader can still treat it as a reason to look again. */
typedef struct ksd_hypr_event_reader {
    char line[KSD_HYPR_EVENT_LINE_CAPACITY];
    size_t used;
    bool discarding;
} ksd_hypr_event_reader;

void ksd_wayland_hypr_event_reader_init(ksd_hypr_event_reader *reader);
/* Calls on_line(context, name) per complete line; name is empty for a line
 * that was dropped or has no ">>" separator. */
void ksd_wayland_hypr_event_feed(ksd_hypr_event_reader *reader,
                                 const char *bytes, size_t length,
                                 void (*on_line)(void *context,
                                                 const char *name),
                                 void *context);
/* Whether an event can change what the window list reports. */
bool ksd_wayland_hypr_event_affects_windows(const char *name);

#endif
