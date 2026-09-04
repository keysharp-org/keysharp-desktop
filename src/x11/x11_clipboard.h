#ifndef KEYSHARP_DESKTOP_X11_CLIPBOARD_H
#define KEYSHARP_DESKTOP_X11_CLIPBOARD_H

#include "operation_result.h"
#include "x11_connect.h"

#include <stdbool.h>
#include <stdint.h>

/* Reads only. Owning a selection means staying alive to answer requests for
 * it, and this worker exits when its one operation is done, so writing the
 * clipboard is not something this path can do without lying about it: the
 * bytes would vanish the moment the worker exited. Setting the clipboard on
 * bare X11 needs a process that outlives the request, which is its own stage.
 *
 * Each fills result with exactly the payload the compositor providers produce
 * for the same opcode. */
void ksd_x11_clipboard_mimetypes(ksd_x11 *connection,
                                 ksd_operation_result *result);
void ksd_x11_clipboard_text(ksd_x11 *connection,
                            ksd_operation_result *result);
void ksd_x11_clipboard_content(ksd_x11 *connection, const uint8_t *mimetype,
                               uint32_t mimetype_length,
                               ksd_operation_result *result);

#endif
