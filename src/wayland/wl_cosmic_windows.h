#ifndef KEYSHARP_DESKTOP_WL_COSMIC_WINDOWS_H
#define KEYSHARP_DESKTOP_WL_COSMIC_WINDOWS_H

#include "operation_result.h"
#include "wl_connect.h"

#include <stdbool.h>
#include <stdint.h>

struct ext_foreign_toplevel_handle_v1;
struct wl_output;
struct wl_registry;
struct ksd_wl_toplevel;

void ksd_wayland_cosmic_bind_info(ksd_wayland *connection,
                                  struct wl_registry *registry,
                                  uint32_t name, uint32_t version);
void ksd_wayland_cosmic_bind_manager(ksd_wayland *connection,
                                     struct wl_registry *registry,
                                     uint32_t name, uint32_t version);
void ksd_wayland_cosmic_attach_toplevel(
    ksd_wayland *connection, struct ksd_wl_toplevel *toplevel);
void ksd_wayland_cosmic_detach_toplevel(
    struct ksd_wl_toplevel *toplevel);
void ksd_wayland_cosmic_output_removed(ksd_wayland *connection,
                                       struct wl_output *output);
void ksd_wayland_cosmic_clear(ksd_wayland *connection);

bool ksd_wayland_cosmic_can_list(const ksd_wayland *connection);
bool ksd_wayland_cosmic_can_focus(const ksd_wayland *connection);
bool ksd_wayland_cosmic_can_close(const ksd_wayland *connection);
bool ksd_wayland_cosmic_can_set_state(const ksd_wayland *connection);

void ksd_wayland_cosmic_window_handles(ksd_wayland *connection,
                                       ksd_operation_result *result);
void ksd_wayland_cosmic_window_list(ksd_wayland *connection,
                                    bool include_hidden,
                                    ksd_operation_result *result);
void ksd_wayland_cosmic_active_window(ksd_wayland *connection,
                                      ksd_operation_result *result);
void ksd_wayland_cosmic_window_action(ksd_wayland *connection,
                                      uint16_t opcode, uint64_t handle,
                                      uint32_t value,
                                      ksd_operation_result *result);

void ksd_wayland_cosmic_window_query(ksd_wayland *connection,
    uint64_t handle, ksd_operation_result *result);

#endif
