#ifndef KEYSHARP_DESKTOP_KWIN_BUS_H
#define KEYSHARP_DESKTOP_KWIN_BUS_H

#include "kwin_host.h"

#include <stdbool.h>

/* The D-Bus surface the KWin script calls, and the only part of this channel
 * that touches GLib.
 *
 * Everything that decides anything lives in ksd_kwin_host, which is pure and
 * tested without a bus. What is left here is transport: own the name, register
 * the object, hand each envelope to the host, and hold a parked invocation
 * until there is something to answer it with. That division is deliberate --
 * this file is the part that cannot be tested on a build machine, so it is
 * kept as close to empty of judgement as it can be.
 *
 * Started only for the KWin backend. Every other backend keeps the daemon's
 * plain poll loop untouched, because none of them needs a bus name and a main
 * loop they do not use is a main loop that can go wrong. */
typedef struct ksd_kwin_bus ksd_kwin_bus;

/* Owns io.github.keysharp.KWinProvider1 on the session bus and registers
 * /io/github/keysharp/KWinProvider. Returns NULL when the name cannot be
 * taken, which means another daemon already holds it. */
ksd_kwin_bus *ksd_kwin_bus_start(ksd_kwin_host *host);

/* Runs until the authority closes descriptor, then returns. The authority
 * socket is watched as a source on the same loop rather than from another
 * thread: everything here already runs on one, and a second would need a lock
 * around the host for no gain. */
int ksd_kwin_bus_run(ksd_kwin_bus *bus, int descriptor);

void ksd_kwin_bus_stop(ksd_kwin_bus *bus);

#endif
