#ifndef KSD_INSTALL_MODE_H
#define KSD_INSTALL_MODE_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* Where this installation keeps its socket, and who must own the things it
 * trusts.
 *
 * A system installation runs the authority as root and serves every user on
 * the machine. A user installation runs it as one unprivileged user and serves
 * only that user, which is what makes an install without administrator rights
 * possible at all.
 *
 * The two differ in exactly one thing that matters: who must own an artefact
 * before it is trusted. Rather than relaxing each ownership test on its own --
 * a dozen independent chances to relax the wrong one -- every test asks
 * ksd_install_owner(), and that function cannot return anything but root while
 * the process is root. A root daemon therefore cannot be talked into accepting
 * a user-owned binary or socket by any configuration, environment or argument,
 * because there is nothing to talk to: the answer is derived from the
 * process's own credentials and from nothing else.
 */

/* The uid every trusted artefact must be owned by: root when this process is
 * root, and this process's own uid otherwise. */
uid_t ksd_install_owner(void);

/* The gid that goes with the owner, for artefacts that carry both. Derived
 * the same way and for the same reason: while this process is root the answer
 * is root's group, so nothing built on it is loosened by the user mode. */
gid_t ksd_install_group(void);

/* Whether this is the system-wide installation. Equivalent to the owner being
 * root, and spelled separately only where that reads better. */
bool ksd_install_is_system(void);

/* True when a file or directory owned by `owner` may be trusted here. */
bool ksd_install_owner_trusted(uid_t owner);

/* The authority socket for this installation, written into `buffer`.
 *
 * The system path is fixed. The user path lives under XDG_RUNTIME_DIR, which
 * is per-user, already mode 0700 and cleaned up at logout -- the properties
 * the system path gets from being root-owned. A user installation with no
 * runtime directory has nowhere safe to put a socket and is refused rather
 * than falling back to a world-writable directory.
 */
bool ksd_install_socket_path(char *buffer, size_t capacity);

/* The mode the authority socket is created with. A system socket is reachable
 * by every user on the machine and is filtered by peer credentials; a user
 * socket has no business being reachable by anyone else, and its directory
 * already says so. */
unsigned ksd_install_socket_mode(void);

/* Where the permission store keeps grants that outlive a session, and those
 * that do not. NULL leaves the library's own system-wide default in place,
 * which is the right answer for a system installation. */
const char *ksd_install_persistent_directory(void);
const char *ksd_install_runtime_directory(void);

#endif
