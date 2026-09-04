#ifndef KEYSHARP_DESKTOP_WL_CLIPBOARD_H
#define KEYSHARP_DESKTOP_WL_CLIPBOARD_H

#include "operation_result.h"
#include "wl_connect.h"

#include <stdint.h>

/* Reads only, and for the same reason as on X11 rather than a different one:
 * a data source must stay alive to answer the send events the compositor
 * delivers when someone pastes, and a worker that exits with its one operation
 * cannot. The content would vanish behind the caller.
 *
 * ext-data-control is deliberately not a portal. It asks for no consent dialog
 * and shows none, because a compositor that offers this protocol has already
 * decided that a client which can bind it may read the selection. That makes
 * this the one clipboard route on a generic Wayland session that behaves the
 * way the other backends do. */
void ksd_wayland_clipboard_mimetypes(ksd_wayland *connection,
                                     ksd_operation_result *result);
void ksd_wayland_clipboard_content(ksd_wayland *connection,
                                   const uint8_t *mimetype,
                                   uint32_t mimetype_length,
                                   ksd_operation_result *result);
void ksd_wayland_clipboard_text(ksd_wayland *connection,
                                ksd_operation_result *result);

#endif
