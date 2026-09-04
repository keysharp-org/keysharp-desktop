#ifndef KEYSHARP_DESKTOP_ENABLE_EXTENSION_H
#define KEYSHARP_DESKTOP_ENABLE_EXTENSION_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* Turns on the bundled shell extension for the user running the command.
 *
 * The install places the extension but has never enabled it, and until it is
 * enabled the daemon finds no provider, registers no backend, and clients are
 * told the selected backend is None or Generic. That is a silent failure with
 * a confusing symptom, so this command exists to make it one step -- and, when
 * it cannot finish the job, to say exactly which of the several things that
 * quietly defeat an enable is in the way.
 *
 * Deliberately per-user. Enablement lives in the user's own dconf, so there is
 * no machine-wide equivalent short of a dconf system database, which would
 * also stop the user ever turning it off. The command therefore refuses to run
 * as root: run under sudo it would write root's dconf and report success.
 */

/* What the command concluded. Every one of these is reported as a distinct
 * status= line, because they are the outcomes a user would otherwise have to
 * tell apart by guessing. */
typedef enum {
    /* The provider already answers on the bus. Nothing was written. */
    KSD_ENABLE_ALREADY_LIVE = 0,
    /* Written, and the provider came up while we watched. */
    KSD_ENABLE_ENABLED,
    /* Written and accepted, but the shell has not loaded it yet. On Wayland
     * that means logging out; there is no in-place shell restart. */
    KSD_ENABLE_NEEDS_RELOGIN,
    /* The uuid was already listed and the provider still is not up, so the
     * list is not what is wrong. */
    KSD_ENABLE_ALREADY_LISTED,
    /* KWin, X11 or generic Wayland: those backends use no extension. */
    KSD_ENABLE_NOT_APPLICABLE,
    /* No GNOME or Cinnamon shell owns a name on this bus. */
    KSD_ENABLE_NO_SHELL,
    /* No session bus reachable. Writing anyway would go to a memory backend
     * or an autolaunched throwaway bus, and report success either way. */
    KSD_ENABLE_NO_BUS,
    /* GNOME's disable-user-extensions is on, which makes every extension
     * inert regardless of the list. Reported rather than overridden: the user
     * may have set it deliberately. */
    KSD_ENABLE_KILL_SWITCH,
    /* A dconf lockdown makes the key unwritable. GSettings would accept the
     * write and drop it. */
    KSD_ENABLE_LOCKED,
    /* The shell's GSettings schema is not installed. */
    KSD_ENABLE_NO_SCHEMA,
    /* We wrote the uuid and the shell took it back out, which is what Cinnamon
     * does to an extension that throws while loading. Without this the user
     * re-runs the command forever. */
    KSD_ENABLE_SHELL_REJECTED,
    /* Running as root, or with mismatched real and effective credentials. */
    KSD_ENABLE_REFUSED,
} ksd_enable_status;

const char *ksd_enable_status_name(ksd_enable_status status);

/* 0 when there is nothing for the user to do, 3 when there is, 1 when nothing
 * was written and something is wrong. Usage errors are 2, as elsewhere in the
 * CLI. The installers treat anything nonzero as advisory and never fail an
 * install over it. */
int ksd_enable_exit_code(ksd_enable_status status);

/* Whole credentials, and not root. Mirrors the daemon's own refusal: a
 * mismatch between real and effective ids is a setuid binary partway through
 * something, and which identity's dconf to write has no good answer. */
bool ksd_enable_credentials_refused(uid_t uid, uid_t euid, gid_t gid,
                                    gid_t egid);

/* The list operations, separated from GSettings so they can be tested without
 * a schema, a bus or a shell. Callers free the results with g_strfreev. */
bool ksd_strv_contains(const char *const *values, const char *needle);
char **ksd_strv_with(const char *const *values, const char *addition);
char **ksd_strv_without(const char *const *values, const char *removal);

/* Whether `directory` appears in a colon-separated search path, comparing
 * whole entries with trailing slashes ignored so that /usr/local/share and
 * /usr/local/share/ are one answer rather than two. */
bool ksd_data_dir_listed(const char *search_path, const char *directory);

int ksd_enable_extension_main(int argc, char **argv);

#endif
