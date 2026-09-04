#ifndef KEYSHARP_DESKTOP_WL_WINDOWS_H
#define KEYSHARP_DESKTOP_WL_WINDOWS_H

#include "operation_result.h"
#include "wl_connect.h"

/* Enumeration only, and it reports less than the other backends do.
 * ext-foreign-toplevel-list carries a title, an app id and an opaque
 * identifier, and nothing else: no geometry, no pid, no minimized flag, and no
 * activated flag -- which is why there is no active-window verb here. Those
 * fields are left out of the reply rather than filled with zeros, because a
 * zero would be read as a fact.
 *
 * Changing a window needs a different protocol than listing them, and the one
 * that can (wlr-foreign-toplevel-management) is not part of wayland-protocols.
 * So this backend can say what exists and not act on it. */
void ksd_wayland_window_list(ksd_wayland *connection,
                             ksd_operation_result *result);

#endif
