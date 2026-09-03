#ifndef KEYSHARP_DESKTOP_BACKEND_H
#define KEYSHARP_DESKTOP_BACKEND_H

#include "keysharp_desktop/client.h"

#include <sys/types.h>

ksd_backend ksd_backend_resolve(void);
ksd_backend ksd_backend_resolve_process(pid_t pid);
bool ksd_backend_session_unsupported(void);
uint64_t ksd_backend_operations(ksd_backend backend);

#endif
