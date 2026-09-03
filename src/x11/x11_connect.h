#ifndef KEYSHARP_DESKTOP_X11_CONNECT_H
#define KEYSHARP_DESKTOP_X11_CONNECT_H

#include "keysharp_desktop/client.h"

#include <stdbool.h>

typedef struct ksd_x11 ksd_x11;

/* Opens the display named by a value that ksd_x11_display_parse has already
 * accepted and rebuilt. authority may be NULL, in which case the X library
 * uses its own default; when given it is set for this process only, because
 * the worker has already dropped privileges by the time it runs.
 *
 * Refuses an XWayland server. The point of the X11 backend is desktops with
 * no Wayland compositor to broker; on a Wayland session the compositor path
 * is the supported one, and reaching the same windows through XWayland would
 * quietly serve a different and worse view of them. */
ksd_status ksd_x11_open(const char *display, const char *authority,
                        ksd_x11 **connection);
void ksd_x11_close(ksd_x11 *connection);

/* True when the server reports the XWAYLAND extension. Exposed so the refusal
 * can be told apart from a connection failure in a diagnostic. */
bool ksd_x11_server_is_xwayland(ksd_x11 *connection);

#endif
