#ifndef KEYSHARP_DESKTOP_WL_OUTPUTS_H
#define KEYSHARP_DESKTOP_WL_OUTPUTS_H

#include "wl_connect.h"

#include <stdbool.h>
#include <stdint.h>

struct ksd_wl_output;
struct wl_registry;

void ksd_wayland_output_add(ksd_wayland *connection,
                            struct wl_registry *registry, uint32_t name,
                            uint32_t version);
void ksd_wayland_output_remove(ksd_wayland *connection, uint32_t name);
void ksd_wayland_outputs_bind_xdg(ksd_wayland *connection);
void ksd_wayland_outputs_clear(ksd_wayland *connection);
bool ksd_wayland_output_bounds(const struct ksd_wl_output *output,
                               int32_t *x, int32_t *y,
                               int32_t *width, int32_t *height);

#endif
