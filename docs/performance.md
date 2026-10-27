# Window operation performance

## Native X11 measurements

The isolated Xvfb benchmark uses one persistent native X connection and a private
1279×1024 display. It measures the native backend directly, including X server
requests. It excludes the authority RPC, executable/grant checks, managed caller,
real compositor and GPU. Timings are observations, not test thresholds.

Measured on Linux x86-64 in the development VM, 2026-09-05:

| Operation | Mean time per call | Result bytes |
| --- | ---: | ---: |
| List 40 windows | 1.33 ms | varies with window metadata |
| Keyboard state with full map | 0.648 ms | 68,207 |
| Keyboard state with matching map revision | 0.022 ms | 244 |
| Query one window | 0.367 ms | 451 |
| Capture a 200×100 window | 0.667 ms | 80,020 |

The window list uses 10 measured calls; other rows use 1,000. Capture bytes travel
in a sealed memory descriptor. A matching keyboard map revision keeps the map in
the client cache and returns current state; an invalidated revision returns the
new map. The measured unchanged response is about 280 times smaller.

Reproduce after building `x11-bench-tests`:

```sh
sh tests/x11_server_test.sh build/x11-bench-tests
```

The wrapper starts and removes its own Xvfb server. These numbers do not establish
end-to-end desktop latency or input-hook performance. Live compositor and physical
input-device measurements remain necessary for those claims.

Window subscriptions use property and structure notifications on a dedicated
worker connection. Idle subscriptions make no X queries. Isolated tests cover
create, close, active, title, minimize, restore and geometry notifications, plus
worker transition and disconnect behavior.

## Completion and scheduling

Window mutations do not add a fixed settling sleep. When a caller subscribes to
window state, the authority waits for the requested effect to appear in that
state before returning its observation sequence. Fully observed operations
return immediately. If a move or resize changes a requested component but leaves
another unchanged, constraints cannot be distinguished from a pending update.
The authority waits up to two seconds, then returns success with sequence zero
if that geometry progress remains visible. This claims no complete observation
fence. An entirely unchanged or unrelated result still times out. KWin does not
provide window subscriptions, so its script reply carries no observation fence.

GNOME and Cinnamon report geometry and property changes directly, including
opacity, keep-above and maximize changes that need not resize a window. Unchanged
geometry and already-satisfied state requests avoid repeating native setters.
Native decoration changes unsupported by a shell return `UNSUPPORTED` immediately
instead of reporting success and waiting for a change that cannot occur.

Hyprland does not report every geometry change. A successful mutation can wake
the existing window watcher immediately rather than wait for its 250 ms idle
sample. While the effect remains unobserved, retries share a 30 ms minimum
interval per authorization lease, including concurrent requests and unrelated
state notifications. These window samples do not query keyboard or display state
or postpone their ordinary updates. The worker advertises whether its windows
need sampling; wlroots and COSMIC retain their native event-driven watches.

COSMIC advertises window subscriptions when its selected window list has stable
identifiers. Clients can maintain their window mirror from native geometry,
title, focus and state notifications; the watcher takes no periodic snapshots
while idle. Initial snapshots wait for COSMIC's first property batch to complete;
a Wayland sync reply can arrive before those properties. The wait shares the
existing refresh deadline and finishes immediately once the batch is ready.

wlroots and COSMIC first synchronize to obtain current window state. An
already-satisfied state request then skips both the mutation and its final
synchronization. Mutations retain their acknowledgments; COSMIC also waits for
the requested state event because a sync reply can precede the configure event.
Maximize unminimizes the window first. COSMIC reports maximization independently
of fullscreen, so restoring its normal state can be observed without ending fullscreen.
Focus requests continue to reach the compositor even when a window was active.

X11 control operations reuse required tree or property replies to validate a
window, avoiding a separate attributes query. Window enumeration continues to
issue its property requests in batches rather than wait for each property.

Hiding a managed X11 client sends the ICCCM withdrawal notification and waits
for the manager to acknowledge `WM_STATE` and release its reparenting frame.
Map and reparent notifications reassert withdrawal within the original
two-second deadline. A final withdrawal follows frame release to cover queued
remaps. Normal toplevels with a pending map still send the notification before
`WM_STATE` appears; child and override-redirect windows use direct unmaps.

X11 restacking uses the window manager's advertised `_NET_RESTACK_WINDOW`
request for managed clients, with the native fallback on older managers and
bare displays. X11 and generic Wayland restacks return sequence zero: compositor
layers can keep docks or keep-above windows above a raised client, and desktop
windows below a lowered one. X11 enumeration also includes unmapped support
windows outside the managed stack. These calls confirm delivery without claiming
a global stacking edge; subscriptions continue to report the resulting order.

The shared KWin/display relay uses monotonic condition deadlines instead of
periodic response polling. Waiting for another writer and writing to the peer
both respect each request's deadline. Work that has not been sent can return
`BUSY`; an attempted transfer with an unknown outcome returns `TIMEOUT` and is
not automatically retried. KWin's parked script polls wake when work arrives;
their idle keepalive timers add no delay to queued window operations.
