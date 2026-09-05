#ifndef KEYSHARP_DESKTOP_PORTAL_CAPTURE_H
#define KEYSHARP_DESKTOP_PORTAL_CAPTURE_H

#include "operation_result.h"

#include <stdbool.h>

bool ksd_portal_capture_available(void);
void ksd_portal_capture_desktop(ksd_operation_result *result);

#endif
