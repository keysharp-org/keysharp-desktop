#ifndef KEYSHARP_DESKTOP_WL_INTERNAL_H
#define KEYSHARP_DESKTOP_WL_INTERNAL_H

#include "ext-data-control-v1-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "cosmic-toplevel-info-v2-client-protocol.h"
#include "cosmic-toplevel-management-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "wl_connect.h"
#include "wl_hypr.h"

#include <wayland-client.h>

/* Shared between the Wayland sources only. Nothing outside src/wayland sees a
 * wl_ type, so no other target needs the Wayland headers. */
/* One toplevel the compositor has told us about. The protocol delivers a
 * handle and then its properties as separate events, so these accumulate as
 * they arrive and are read once the compositor says it is done. */
typedef struct ksd_cosmic_geometry {
    struct wl_output *output;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    struct ksd_cosmic_geometry *next;
} ksd_cosmic_geometry;

typedef struct ksd_wl_toplevel {
    struct ext_foreign_toplevel_handle_v1 *handle;
    struct zcosmic_toplevel_handle_v1 *cosmic_handle;
    struct zwlr_foreign_toplevel_handle_v1 *wlr_handle;
    struct ksd_wayland *connection;
    uint64_t id;
    char *title;
    char *app_id;
    char *identifier;
    uint32_t state;
    bool ready;
    bool closed;
    ksd_cosmic_geometry *cosmic_geometries;
    /* Joined from Hyprland IPC on each refresh, by identifier. The restore
     * workspace survives refreshes so a minimized window returns where it
     * was rather than to whichever workspace is active. */
    ksd_hypr_client hypr;
    bool hypr_known;
    bool hypr_has_restore;
    int64_t hypr_restore_workspace;
    char hypr_restore_name[KSD_HYPR_WORKSPACE_CAPACITY];
    /* Where explicit raises and lowers left this window; zero when none has. */
    int64_t hypr_stack_stamp;
    /* 0..255, or -1 when Hyprland did not report it. */
    int32_t hypr_opacity;
    struct ksd_wl_toplevel *next;
} ksd_wl_toplevel;

#define KSD_WL_TOPLEVEL_STATE_MAXIMIZED (UINT32_C(1) << 0)
#define KSD_WL_TOPLEVEL_STATE_MINIMIZED (UINT32_C(1) << 1)
#define KSD_WL_TOPLEVEL_STATE_ACTIVATED (UINT32_C(1) << 2)

typedef struct ksd_wl_output {
    struct wl_output *output;
    struct zxdg_output_v1 *xdg_output;
    uint32_t registry_name;
    int32_t x;
    int32_t y;
    int32_t mode_width;
    int32_t mode_height;
    int32_t scale;
    int32_t transform;
    int32_t logical_x;
    int32_t logical_y;
    int32_t logical_width;
    int32_t logical_height;
    int32_t physical_width_mm;
    int32_t physical_height_mm;
    int32_t refresh_mhz;
    /* wl_output v4 names take precedence over xdg-output ones, which only
     * matter to compositors that predate them. */
    char name[64];
    bool name_from_output;
    bool current_mode;
    bool logical_position;
    bool logical_size;
    bool done;
    struct ksd_wl_output *next;
} ksd_wl_output;

struct ksd_wayland {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    struct xkb_keymap *keymap;
    char *keymap_text;
    char *keymap_revision;
    struct ext_data_control_manager_v1 *data_control;
    struct ext_foreign_toplevel_list_v1 *toplevel_list;
    struct zcosmic_toplevel_info_v1 *cosmic_toplevel_info;
    struct zcosmic_toplevel_manager_v1 *cosmic_toplevel_manager;
    struct zwlr_foreign_toplevel_manager_v1 *toplevel_manager;
    struct zwlr_screencopy_manager_v1 *screencopy_manager;
    struct ext_output_image_capture_source_manager_v1 *output_source_manager;
    struct ext_foreign_toplevel_image_capture_source_manager_v1 *toplevel_source_manager;
    struct ext_image_copy_capture_manager_v1 *image_copy_manager;
    struct zxdg_output_manager_v1 *xdg_output_manager;
    struct wl_shm *shm;
    struct zwlr_virtual_pointer_manager_v1 *pointer_manager;
    struct zwlr_virtual_pointer_v1 *virtual_pointer;
    uint32_t screencopy_version;
    /* The name and version the registry advertised for the seat, kept so a
     * seat arriving after the first round trip can still be bound. */
    uint32_t seat_name;
    /* Every toplevel seen on this connection, oldest first. The list is
     * attached at connect rather than per call, because the compositor sends
     * one toplevel event per existing window as soon as the global is bound
     * and a listener added later would miss them all. */
    ksd_wl_toplevel *toplevels;
    uint32_t cosmic_capabilities;
    ksd_wl_output *outputs;
    pid_t session_pid;
    /* The Wayland compositor's pid, and whether the Hyprland instance the
     * session names is that compositor. Hyprland IPC is used only then. */
    pid_t compositor_pid;
    bool hypr;
    /* Derives window handles from compositor identifiers, so every connection
     * to one compositor instance names a window the same way. Unset when the
     * instance could not be identified; handles are then random. */
    uint8_t handle_salt[32];
    bool handle_salt_ready;
    /* Orders explicit restacking on Hyprland, whose IPC reports no stack. */
    int64_t hypr_stack_sequence;
    char hypr_last_active[KSD_HYPR_ID_CAPACITY];
    /* Whether the toplevels' opacity was read since the last refresh; read
     * lazily because only window listings need it. */
    bool hypr_opacity_fresh;
};

/* A handle remains opaque across worker and compositor restarts; one derived
 * from an identifier is the same in every worker of one compositor instance. */
uint64_t ksd_wayland_new_handle(const ksd_wayland *connection);
/* The handle for a compositor identifier, or 0 when none can be derived or
 * it would collide with a live one. */
uint64_t ksd_wayland_handle_for_identifier(const ksd_wayland *connection,
                                           const char *identifier);
void ksd_wayland_replace_string(char **slot, const char *value);

/* Attaches the toplevel listener. Called once, immediately after the global is
 * bound, for the reason above. */
void ksd_wayland_toplevels_attach(ksd_wayland *connection);
void ksd_wayland_toplevels_clear(ksd_wayland *connection);
void ksd_wayland_wlr_toplevels_attach(ksd_wayland *connection);

/* One round trip, bounded. wl_display_roundtrip blocks with no deadline of its
 * own, and a compositor that stops answering would park the worker for as long
 * as it liked -- the same hazard the X11 clipboard has, answered the same way. */
bool ksd_wayland_roundtrip(ksd_wayland *connection, int timeout_ms);
bool ksd_wayland_dispatch_until(ksd_wayland *connection,
                                bool (*complete)(void *), void *data,
                                int timeout_ms);
/* Reads and dispatches what the display socket already holds, without
 * waiting. For a caller that polls the socket itself. */
bool ksd_wayland_dispatch_ready(ksd_wayland *connection);

/* Reads everything a peer writes into a pipe, to a ceiling, with a deadline.
 * Both the clipboard read path and anything else that takes a descriptor from
 * the compositor need exactly this. */
bool ksd_wayland_drain(int descriptor, int timeout_ms, uint8_t **data,
                       size_t *length);

#endif
