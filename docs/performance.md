# Native X11 measurements

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
