// Generated from providers/shared; edit and regenerate those sources.
// GNOME Shell 45+ loader and compositor adapter.
import GnomeGLib from 'gi://GLib';
import GnomeGio from 'gi://Gio';
import GnomeMeta from 'gi://Meta';
import GnomeClutter from 'gi://Clutter';
import GnomeCogl from 'gi://Cogl';
import GnomeGdkPixbuf from 'gi://GdkPixbuf';
import GnomeShell from 'gi://Shell';
import GnomeSt from 'gi://St';
import GnomeCairoGI from 'gi://cairo';
import * as GnomeMain from 'resource:///org/gnome/shell/ui/main.js';
import * as GnomeConfig from 'resource:///org/gnome/shell/misc/config.js';

const SHELL_MAJOR = Number.parseInt(GnomeConfig.PACKAGE_VERSION.split('.')[0], 10);
function workArea() {
    const index = GnomeMain.layoutManager.primaryIndex;
    const area = GnomeMain.layoutManager.getWorkAreaForMonitor(index);
    return [Math.round(area.x), Math.round(area.y),
        Math.round(area.width), Math.round(area.height)];
}

function setDecorated(win, decorated) {
    const isX11 = typeof win.get_client_type === 'function'
        ? win.get_client_type() === GnomeMeta.WindowClientType.X11
        : typeof win.get_xwindow === 'function' && win.get_xwindow() !== 0;
    if (!isX11 || typeof win.get_xwindow !== 'function')
        return true;

    const xid = win.get_xwindow();
    if (!xid)
        return true;

    const value = decorated ? 1 : 0;
    GnomeGLib.spawn_command_line_async(`xprop -id ${xid} -f _MOTIF_WM_HINTS 32c `
        + `-set _MOTIF_WM_HINTS "2, 0, ${value}, 0, 0"`);
    return true;
}

function addClickThroughChrome(actor) {
    if (SHELL_MAJOR >= 50)
        GnomeMain.layoutManager.addTopChrome(actor);
    else
        GnomeMain.layoutManager.addTopChrome(actor, {affectsInputRegion: false});
}

function makeImageContent(content, frame, sameSize) {
    if (content && sameSize) {
        try {
            let updated = false;
            if (SHELL_MAJOR >= 48 && typeof content.get_texture === 'function') {
                const texture = content.get_texture();
                if (texture && typeof texture.set_region === 'function') {
                    updated = texture.set_region(0, 0, 0, 0,
                        frame.width, frame.height, frame.width, frame.height,
                        frame.format, frame.rowStride, frame.pixels);
                    if (updated)
                        content.invalidate();
                }
            } else if (typeof content.set_area === 'function') {
                const area = new GnomeCairoGI.RectangleInt();
                area.x = 0;
                area.y = 0;
                area.width = frame.width;
                area.height = frame.height;
                updated = content.set_area(frame.pixels, frame.format, area,
                    frame.rowStride);
            }
            if (updated)
                return content;
        } catch (_e) {
        }
    }

    if (SHELL_MAJOR >= 48) {
        const image = new GnomeSt.ImageContent({
            preferredWidth: frame.width,
            preferredHeight: frame.height,
        });
        const context = global.stage.context.get_backend().get_cogl_context();
        image.set_bytes(context, frame.pixbuf.read_pixel_bytes(), frame.format,
            frame.width, frame.height, frame.rowStride);
        return image;
    }

    const image = new GnomeClutter.Image();
    image.set_data(frame.pixels, frame.format, frame.width, frame.height,
        frame.rowStride);
    return image;
}

function actorResourceScale(actor) {
    try {
        const scale = typeof actor.get_resource_scale === 'function'
            ? Number(actor.get_resource_scale()) : 1;
        return Number.isFinite(scale) && scale > 0 && scale <= 16 ? scale : 1;
    } catch (_e) {
        return 1;
    }
}

function _captureWindowRect(provider, win, actor, includeDecoration) {
    let buffer = null;
    let frame = null;
    try {
        buffer = win.get_buffer_rect();
        frame = includeDecoration ? buffer : win.get_frame_rect();
    } catch (_e) {
        return null;
    }
    if (!buffer || !frame)
        return null;

    const scale = actorResourceScale(actor);
    const rect = {
        x: Math.round((frame.x - buffer.x) * scale),
        y: Math.round((frame.y - buffer.y) * scale),
        width: Math.floor(frame.width * scale),
        height: Math.floor(frame.height * scale),
    };
    return rect.x >= 0 && rect.y >= 0
        && provider._validCaptureGeometry(rect.width, rect.height) ? rect : null;
}

function _fitCaptureRect(provider, rect, texture) {
    let width = 0;
    let height = 0;
    try {
        width = Number(texture.get_width());
        height = Number(texture.get_height());
    } catch (_e) {
        return null;
    }
    if (!Number.isInteger(width) || !Number.isInteger(height)
        || rect.x >= width || rect.y >= height)
        return null;

    const fitted = {
        x: rect.x,
        y: rect.y,
        width: Math.min(rect.width, width - rect.x),
        height: Math.min(rect.height, height - rect.y),
    };
    return provider._validCaptureGeometry(fitted.width, fitted.height)
        ? fitted : null;
}

function compositeToStream(texture, rect, stream, done) {
    try {
        GnomeShell.Screenshot.composite_to_stream(texture,
            rect.x, rect.y, rect.width, rect.height, 1,
            null, 0, 0, 1, stream, done);
    } catch (_e) {
        GnomeShell.Screenshot.composite_to_stream(texture,
            rect.x, rect.y, rect.width, rect.height, 1,
            null, 0, 0, 1, stream, null, done);
    }
}

function captureArea(provider, params, reply) {
    const [x, y, width, height] = params;
    if (!provider._validCaptureGeometry(width, height)) {
        reply(null);
        return;
    }

    try {
        const screenshot = new GnomeShell.Screenshot();
        const stream = GnomeGio.MemoryOutputStream.new_resizable();
        screenshot.screenshot_area(x, y, width, height, stream, (_obj, result) => {
            try {
                screenshot.screenshot_area_finish(result);
                stream.close(null);
                reply(new Uint8Array(stream.steal_as_bytes().get_data()));
            } catch (e) {
                logError(e, 'Keysharp: CaptureArea screenshot failed');
                reply(null);
            }
        });
    } catch (e) {
        logError(e, 'Keysharp: CaptureArea failed');
        reply(null);
    }
}

function captureWindow(provider, handle, includeDecoration, reply) {
    let watchdog = 0;
    const done = bytes => {
        if (watchdog !== 0) {
            GnomeGLib.source_remove(watchdog);
            watchdog = 0;
        }
        reply(bytes);
    };

    try {
        const win = provider._findWindow(handle);
        const actor = win && typeof win.get_compositor_private === 'function'
            ? win.get_compositor_private() : null;
        if (!actor || typeof actor.paint_to_content !== 'function') {
            done(null);
            return;
        }

        const wanted = _captureWindowRect(provider, win, actor, includeDecoration);
        let content = wanted ? actor.paint_to_content(null) : null;
        const texture = content && typeof content.get_texture === 'function'
            ? content.get_texture() : null;
        const rect = texture ? _fitCaptureRect(provider, wanted, texture) : null;
        if (!rect) {
            done(null);
            return;
        }

        const stream = GnomeGio.MemoryOutputStream.new_resizable();
        watchdog = GnomeGLib.timeout_add(GnomeGLib.PRIORITY_DEFAULT, 15000, () => {
            watchdog = 0;
            content = null;
            reply(null);
            return GnomeGLib.SOURCE_REMOVE;
        });
        compositeToStream(texture, rect, stream, (_obj, result) => {
            content = null;
            try {
                GnomeShell.Screenshot.composite_to_stream_finish(result);
                stream.close(null);
                done(new Uint8Array(stream.steal_as_bytes().get_data()));
            } catch (e) {
                logError(e, 'Keysharp: CaptureWindow composite failed');
                done(null);
            }
        });
    } catch (e) {
        logError(e, 'Keysharp: CaptureWindow failed');
        done(null);
    }
}

function windowExtras(win) {
    let onCurrentWorkspace = true;
    let known = false;
    try {
        const workspace = win.get_workspace();
        onCurrentWorkspace = win.is_on_all_workspaces()
            || (workspace !== null
                && workspace === global.workspace_manager.get_active_workspace());
        known = true;
    } catch (_e) {
    }
    return {
        values: {onCurrentWorkspace: onCurrentWorkspace},
        validFields: known ? ['onCurrentWorkspace'] : [],
    };
}

const env = {
    Clutter: GnomeClutter,
    Cogl: GnomeCogl,
    GdkPixbuf: GnomeGdkPixbuf,
    Gio: GnomeGio,
    GLib: GnomeGLib,
    Main: GnomeMain,
    Meta: GnomeMeta,
    St: GnomeSt,
    serviceName: 'io.github.keysharp.GnomeShell',
    objectPath: '/io/github/keysharp/GnomeShell',
    providerSocketName: 'provider-gnome.sock',
    logError: logError,
    decodeBytes: bytes => new TextDecoder().decode(bytes),
    addClickThroughChrome: addClickThroughChrome,
    makeImageContent: makeImageContent,
    captureArea: captureArea,
    captureWindow: captureWindow,
    setDecorated: setDecorated,
    workArea: workArea,
    maximizeFlags: GnomeMeta.MaximizeFlags.HORIZONTAL | GnomeMeta.MaximizeFlags.VERTICAL,
    windowExtras: windowExtras,
    supportsTooltip: true,
};

// Shared shell-extension implementation. Loader and compositor differences live in env.
const Clutter = env.Clutter;
const Cogl = env.Cogl;
const GdkPixbuf = env.GdkPixbuf;
const Gio = env.Gio;
const GLib = env.GLib;
const Main = env.Main;
const Meta = env.Meta;
const St = env.St;
const DBUS_IFACE_XML = `
<node>
  <interface name="org.keysharp.Desktop.Provider1">

    <!-- Returns JSON: { ok, windows: [ { id, title, appId, pid,
         frame:{x,y,width,height}, client:{x,y,width,height},
         active, minimized, maximized, visible, transparency } ... ] }
         Windows are ordered bottom-to-top (index 0 = lowest z-order). -->
    <method name="GetWindowList">
      <arg type="b" direction="in"  name="includeHidden"/>
      <arg type="s" direction="out" name="json"/>
    </method>

    <!-- Returns JSON: { ok, window: { ... } }. Missing handles return NotFound. -->
    <method name="QueryWindow">
      <arg type="t" direction="in" name="handle"/>
      <arg type="s" direction="out" name="json"/>
    </method>

    <!-- Returns JSON: { ok, window: { ... } | null } -->
    <method name="GetActiveWindow">
      <arg type="s" direction="out" name="json"/>
    </method>

    <!-- Global pointer position in compositor coordinates. -->
    <method name="GetCursorPosition">
      <arg type="i" direction="out" name="x"/>
      <arg type="i" direction="out" name="y"/>
    </method>

    <!-- Work area (primary monitor minus panels/struts), in screen coordinates. -->
    <method name="GetWorkArea">
      <arg type="i" direction="out" name="x"/>
      <arg type="i" direction="out" name="y"/>
      <arg type="i" direction="out" name="width"/>
      <arg type="i" direction="out" name="height"/>
    </method>

    <!-- Clipboard bridge: Wayland has no focus-independent clipboard access for a background app, and
         the shell exposes no data-control protocol, so the shell (which owns the selection) reads/writes it
         via MetaSelection and notifies of changes via the MetaSelection owner-changed signal. Content is
         raw MIME-type <-> bytes so every format (text, image, html, uri-list, ...) round-trips. -->
    <method name="GetClipboardMimetypes">
      <arg type="as" direction="out" name="mimetypes"/>
    </method>
    <!-- Read one MIME type's bytes (async: MetaSelection.transfer_async). "" bytes = absent/empty. -->
    <method name="GetClipboardContent">
      <arg type="s" direction="in" name="mimetype"/>
      <arg type="ay" direction="out" name="bytes"/>
    </method>
    <!-- Replace the whole selection with a single MIME type's bytes. Reachable only on the
         root authenticated provider peer. keysharp-desktop needs no grant for it: reading
         the selection is gated, replacing it is not. -->
    <method name="SetClipboardContent">
      <arg type="s" direction="in" name="mimetype"/>
      <arg type="ay" direction="in" name="bytes"/>
      <arg type="b" direction="out" name="ok"/>
    </method>
    <!-- UTF-8 text convenience (the common A_Clipboard path) is read only here; the broker
         writes text through SetClipboardContent. -->
    <method name="GetClipboardText">
      <arg type="s" direction="out" name="text"/>
    </method>

    <!-- Register this client as an overlay owner. ownerKey = "pid:starttime"; busName = the caller's
         unique D-Bus name. Lets the shell reap the caller's overlays when its process/connection dies. -->
    <method name="RegisterHighlightOwner">
      <arg type="s" direction="in" name="ownerKey"/>
      <arg type="s" direction="in" name="busName"/>
      <arg type="b" direction="out" name="ok"/>
    </method>

    <!-- Window handle is the stable_sequence uint32 of Meta.Window. ok = false when no such window. -->
    <method name="FocusWindow">
      <arg type="t" direction="in" name="handle"/>
      <arg type="b" direction="out" name="ok"/>
    </method>

    <!-- Raise the window to the top of the stack. -->
    <method name="RaiseWindow">
      <arg type="t" direction="in" name="handle"/>
      <arg type="b" direction="out" name="ok"/>
    </method>

    <!-- Lower the window to the bottom of the stack. -->
    <method name="LowerWindow">
      <arg type="t" direction="in" name="handle"/>
      <arg type="b" direction="out" name="ok"/>
    </method>

    <method name="ReserveWindow">
      <arg type="i" direction="in" name="pid"/>
      <arg type="t" direction="in" name="cookie"/>
      <arg type="i" direction="in" name="x"/>
      <arg type="i" direction="in" name="y"/>
      <arg type="i" direction="in" name="ttlMs"/>
      <arg type="b" direction="out" name="ok"/>
    </method>

    <method name="GetReservedWindow">
      <arg type="i" direction="in" name="pid"/>
      <arg type="t" direction="in" name="cookie"/>
      <arg type="s" direction="out" name="id"/>
    </method>

    <!-- Pass x = INT32_MIN (-2147483648) to leave position unchanged;
         width/height <= 0 to leave size unchanged. -->
    <method name="MoveResizeWindow">
      <arg type="t" direction="in" name="handle"/>
      <arg type="i" direction="in" name="x"/>
      <arg type="i" direction="in" name="y"/>
      <arg type="i" direction="in" name="width"/>
      <arg type="i" direction="in" name="height"/>
      <arg type="b" direction="out" name="ok"/>
    </method>

    <!-- Same as MoveResizeWindow but the window is identified by its X11 window id
         (get_xwindow), for X11 sessions where the caller has an XID, not a stable_sequence. -->
    <method name="MoveResizeWindowByXid">
      <arg type="t" direction="in" name="xid"/>
      <arg type="i" direction="in" name="x"/>
      <arg type="i" direction="in" name="y"/>
      <arg type="i" direction="in" name="width"/>
      <arg type="i" direction="in" name="height"/>
      <arg type="b" direction="out" name="ok"/>
    </method>

    <!-- state: 0 = normal, 1 = minimized, 2 = maximized. -->
    <method name="SetWindowState">
      <arg type="t" direction="in" name="handle"/>
      <arg type="i" direction="in" name="state"/>
      <arg type="b" direction="out" name="ok"/>
    </method>

    <!-- above: true = keep above all others, false = clear keep-above. -->
    <method name="SetWindowAbove">
      <arg type="t" direction="in" name="handle"/>
      <arg type="b" direction="in" name="above"/>
      <arg type="b" direction="out" name="ok"/>
    </method>

    <!-- decorated: true = show the titlebar/frame, false = hide it. -->
    <method name="SetWindowDecorated">
      <arg type="t" direction="in" name="handle"/>
      <arg type="b" direction="in" name="decorated"/>
      <arg type="b" direction="out" name="ok"/>
    </method>

    <!-- opacity: 0 (fully transparent) .. 255 (fully opaque). -->
    <method name="SetWindowOpacity">
      <arg type="t" direction="in" name="handle"/>
      <arg type="i" direction="in" name="opacity"/>
      <arg type="b" direction="out" name="ok"/>
    </method>

    <method name="CloseWindow">
      <arg type="t" direction="in" name="handle"/>
      <arg type="b" direction="out" name="ok"/>
    </method>

    <!-- Force-kill the window's client, falling back to a graceful close. -->
    <method name="KillWindow">
      <arg type="t" direction="in" name="handle"/>
      <arg type="b" direction="out" name="ok"/>
    </method>

    <method name="SendMouseMoveAbsolute">
      <arg type="i" direction="in" name="x"/>
      <arg type="i" direction="in" name="y"/>
      <arg type="b" direction="out" name="ok"/>
    </method>
    <method name="SendMouseMoveRelative">
      <arg type="i" direction="in" name="dx"/>
      <arg type="i" direction="in" name="dy"/>
      <arg type="b" direction="out" name="ok"/>
    </method>
    <method name="SendMouseButton">
      <arg type="u" direction="in" name="button"/>
      <arg type="b" direction="in" name="pressed"/>
      <arg type="b" direction="out" name="ok"/>
    </method>
    <method name="SendMouseScroll">
      <arg type="i" direction="in" name="delta"/>
      <arg type="b" direction="in" name="vertical"/>
      <arg type="b" direction="out" name="ok"/>
    </method>

    <!-- Capture a screen region and return raw PNG bytes. No flash, no
         file is written. Runs entirely inside the compositor process.
         Returns an empty array on failure. -->
    <method name="CaptureArea">
      <arg type="i" direction="in"  name="x"/>
      <arg type="i" direction="in"  name="y"/>
      <arg type="i" direction="in"  name="width"/>
      <arg type="i" direction="in"  name="height"/>
      <arg type="ay" direction="out" name="pngData"/>
    </method>

    <!-- Capture one window and return raw PNG bytes without writing a file.
         handle is the stable_sequence uint32 of Meta.Window (the "id" field
         of the window JSON). includeDecoration keeps any compositor buffer
         margin outside the visible frame rect. Returns an empty array on
         failure. -->
    <method name="CaptureWindow">
      <arg type="t" direction="in"  name="handle"/>
      <arg type="b" direction="in"  name="includeDecoration"/>
      <arg type="ay" direction="out" name="pngData"/>
    </method>

    <!-- Draw (or update) a click-through rectangle-outline overlay on the compositor stage. The shell
         has no layer-shell, so the overlay lives inside the shell process. id is a caller-chosen
         token so the same overlay can be moved/resized or hidden later; x/y/width/height are in global
         compositor coordinates; color is an "RRGGBB" hex string; thickness is the outline width in px.
         The overlay is non-reactive and excluded from the shell input region, so it never eats clicks. -->
    <method name="ShowHighlight">
      <arg type="u" direction="in" name="id"/>
      <arg type="s" direction="in" name="ownerKey"/>
      <arg type="s" direction="in" name="busName"/>
      <arg type="i" direction="in" name="x"/>
      <arg type="i" direction="in" name="y"/>
      <arg type="i" direction="in" name="width"/>
      <arg type="i" direction="in" name="height"/>
      <arg type="s" direction="in" name="color"/>
      <arg type="i" direction="in" name="thickness"/>
      <arg type="b" direction="out" name="ok"/>
    </method>

    <!-- Remove the overlay previously created with the given id. Unknown ids are ignored. -->
    <method name="HideHighlight">
      <arg type="u" direction="in" name="id"/>
      <arg type="s" direction="in" name="ownerKey"/>
      <arg type="s" direction="in" name="busName"/>
      <arg type="b" direction="out" name="ok"/>
    </method>

    <method name="ShowImageOverlay">
      <arg type="u" direction="in" name="id"/>
      <arg type="s" direction="in" name="ownerKey"/>
      <arg type="s" direction="in" name="busName"/>
      <arg type="i" direction="in" name="x"/>
      <arg type="i" direction="in" name="y"/>
      <arg type="i" direction="in" name="width"/>
      <arg type="i" direction="in" name="height"/>
      <arg type="ay" direction="in" name="pngData"/>
      <arg type="b" direction="out" name="ok"/>
    </method>

    <method name="MoveImageOverlay">
      <arg type="u" direction="in" name="id"/>
      <arg type="s" direction="in" name="ownerKey"/>
      <arg type="s" direction="in" name="busName"/>
      <arg type="i" direction="in" name="x"/>
      <arg type="i" direction="in" name="y"/>
      <arg type="i" direction="in" name="width"/>
      <arg type="i" direction="in" name="height"/>
      <arg type="b" direction="out" name="ok"/>
    </method>

    <method name="HideImageOverlay">
      <arg type="u" direction="in" name="id"/>
      <arg type="s" direction="in" name="ownerKey"/>
      <arg type="s" direction="in" name="busName"/>
      <arg type="b" direction="out" name="ok"/>
    </method>

    <!-- True when the shell can draw a click-through tooltip on the caller's behalf. -->
    <method name="SupportsTooltip">
      <arg type="b" direction="out" name="ok"/>
    </method>

    <!-- Draw or update a click-through tooltip. slot identifies it per owner; empty text clears it.
         x/y are global compositor coordinates. -->
    <method name="ShowTooltip">
      <arg type="i" direction="in" name="slot"/>
      <arg type="s" direction="in" name="ownerKey"/>
      <arg type="s" direction="in" name="busName"/>
      <arg type="s" direction="in" name="text"/>
      <arg type="i" direction="in" name="x"/>
      <arg type="i" direction="in" name="y"/>
      <arg type="b" direction="out" name="ok"/>
    </method>

    <!-- Remove the tooltip in the given slot for this owner. -->
    <method name="HideTooltip">
      <arg type="i" direction="in" name="slot"/>
      <arg type="s" direction="in" name="ownerKey"/>
      <arg type="s" direction="in" name="busName"/>
      <arg type="b" direction="out" name="ok"/>
    </method>

    <!-- Generic window-event stream consumed by Keysharp's WinEvent.
         type is one of: create, close, active, title, minimize, restore, move.
         json is the affected window's info (the same shape as GetActiveWindow's
         window object); for "close" it carries at least { id }. -->
    <signal name="WindowEvent">
      <arg type="s" name="type"/>
      <arg type="s" name="json"/>
    </signal>

    <!-- Fires on every clipboard change: text is the UTF-8 text (or ""), mimetypes lists the available MIME
         types, so a listener can classify text/other/empty and read any format on demand. -->
    <signal name="ClipboardChanged">
      <arg type="s" name="text"/>
      <arg type="as" name="mimetypes"/>
    </signal>

  </interface>
</node>
`;
const SERVICE_NAME = env.serviceName;
const PROVIDER_OBJECT_PATH = '/org/keysharp/DesktopProvider';
const PROVIDER_SOCKET_NAME = env.providerSocketName;
const OBJECT_PATH = env.objectPath;
const MAX_CLIPBOARD_BYTES = 16 * 1024 * 1024;
const MAX_OVERLAY_PNG_BYTES = 16 * 1024 * 1024;
const MAX_OVERLAY_DIMENSION = 8192;
const MAX_OVERLAY_PIXELS = 16 * 1024 * 1024;
const MAX_OVERLAYS_PER_OWNER = 64;
const MAX_OVERLAYS_TOTAL = 512;
const MAX_OVERLAY_BYTES_PER_OWNER = 64 * 1024 * 1024;
const MAX_OVERLAY_BYTES_TOTAL = 256 * 1024 * 1024;
const MAX_TOOLTIP_CHARS = 65536;
const MAX_CAPTURE_BYTES = 256 * 1024 * 1024;
const MAX_CAPTURE_DIMENSION = 32768;
const MAX_CAPTURE_PIXELS = 64 * 1024 * 1024;

// How many compositor frames to wait for a reserved window to become placeable.
// Retries are timer-driven, so this is a real time budget: 40 x 16ms.
const PLACEMENT_MAX_TRIES = 40;
const PLACEMENT_RETRY_MS = 16;
// How long the actor watchdog stays armed after placement; the observed yank fight lasts ~150ms.
const PLACEMENT_WATCH_MS = 600;

// How long a reservation stays live if the window it was meant for never arrives.
const PLACEMENT_TTL_MS = 2000;
const MAX_PLACEMENTS_TOTAL = 256;
const MAX_PLACEMENTS_PER_PID = 16;
const MAX_PLACED_TOTAL = 256;


const PUBLIC_IFACE_XML = `
<node>
  <interface name="io.github.keysharp.ShellExtension1">
    <method name="GetCursorPosition">
      <arg type="i" direction="out" name="x"/>
      <arg type="i" direction="out" name="y"/>
    </method>
    <method name="GetWorkArea">
      <arg type="i" direction="out" name="x"/>
      <arg type="i" direction="out" name="y"/>
      <arg type="i" direction="out" name="width"/>
      <arg type="i" direction="out" name="height"/>
    </method>
    <method name="RegisterHighlightOwner">
      <arg type="s" direction="in" name="ownerKey"/>
      <arg type="s" direction="in" name="busName"/>
      <arg type="b" direction="out" name="ok"/>
    </method>
    <method name="ShowHighlight">
      <arg type="u" direction="in" name="id"/>
      <arg type="s" direction="in" name="ownerKey"/>
      <arg type="s" direction="in" name="busName"/>
      <arg type="i" direction="in" name="x"/>
      <arg type="i" direction="in" name="y"/>
      <arg type="i" direction="in" name="width"/>
      <arg type="i" direction="in" name="height"/>
      <arg type="s" direction="in" name="color"/>
      <arg type="i" direction="in" name="thickness"/>
      <arg type="b" direction="out" name="ok"/>
    </method>
    <method name="HideHighlight">
      <arg type="u" direction="in" name="id"/>
      <arg type="s" direction="in" name="ownerKey"/>
      <arg type="s" direction="in" name="busName"/>
      <arg type="b" direction="out" name="ok"/>
    </method>
    <method name="ShowImageOverlay">
      <arg type="u" direction="in" name="id"/>
      <arg type="s" direction="in" name="ownerKey"/>
      <arg type="s" direction="in" name="busName"/>
      <arg type="i" direction="in" name="x"/>
      <arg type="i" direction="in" name="y"/>
      <arg type="i" direction="in" name="width"/>
      <arg type="i" direction="in" name="height"/>
      <arg type="ay" direction="in" name="pngData"/>
      <arg type="b" direction="out" name="ok"/>
    </method>
    <method name="MoveImageOverlay">
      <arg type="u" direction="in" name="id"/>
      <arg type="s" direction="in" name="ownerKey"/>
      <arg type="s" direction="in" name="busName"/>
      <arg type="i" direction="in" name="x"/>
      <arg type="i" direction="in" name="y"/>
      <arg type="i" direction="in" name="width"/>
      <arg type="i" direction="in" name="height"/>
      <arg type="b" direction="out" name="ok"/>
    </method>
    <method name="HideImageOverlay">
      <arg type="u" direction="in" name="id"/>
      <arg type="s" direction="in" name="ownerKey"/>
      <arg type="s" direction="in" name="busName"/>
      <arg type="b" direction="out" name="ok"/>
    </method>
    <method name="SupportsTooltip">
      <arg type="b" direction="out" name="ok"/>
    </method>
    <method name="ShowTooltip">
      <arg type="i" direction="in" name="slot"/>
      <arg type="s" direction="in" name="ownerKey"/>
      <arg type="s" direction="in" name="busName"/>
      <arg type="s" direction="in" name="text"/>
      <arg type="i" direction="in" name="x"/>
      <arg type="i" direction="in" name="y"/>
      <arg type="b" direction="out" name="ok"/>
    </method>
    <method name="HideTooltip">
      <arg type="i" direction="in" name="slot"/>
      <arg type="s" direction="in" name="ownerKey"/>
      <arg type="s" direction="in" name="busName"/>
      <arg type="b" direction="out" name="ok"/>
    </method>
  </interface>
</node>`;

// Window state protocol values sent by the C# Wayland bridge.
const STATE_NORMAL    = 0;
const STATE_MINIMIZED = 1;
const STATE_MAXIMIZED = 2;

// Sentinel used by MoveResizeWindow to mean "don't change this axis".
const INT32_MIN = -2147483648;

// Grace period after a client's D-Bus name drops before its overlays are reaped, so a client that
// merely reconnected (new unique name, then re-registers) does not lose its overlays. See
// _handleOverlayConnectionLost.
const OVERLAY_RECONNECT_GRACE_MS = 2000;

class KeysharpExtensionCore {
    constructor() {
        this._dbusImpl     = null;
        this._busNameId    = 0;
        this._providerServer = null;
        this._providerServerSignalId = 0;
        this._providerSocketPath = null;
        this._providerConnections = new Map();
        this._vPointer = null;
        this._focusId      = null;
        this._winCreatedId = null;
        this._mapId = 0;
        this._placements = [];
        this._placed = {};
        this._placementSources = new Set();
        // Overlays are keyed by "<ownerKey>:<id>" (see _overlayKey) so ids never collide across clients.
        // Each entry carries its owner so a dead client's overlays can be reaped:
        //   highlight  entry = { edges:[St.Widget...], ownerKey, ownerPid, ownerStartTime, busName }
        //   image      entry = { actor:Clutter.Actor,  ownerKey, ownerPid, ownerStartTime, busName }
        //   tooltip    entry = { actor:St.Label,       ownerKey, ownerPid, ownerStartTime, busName }
        this._highlights   = new Map();
        this._imageOverlays = new Map();
        this._tooltips     = new Map();
        // Owner-death cleanup: subscription id for NameOwnerChanged, the periodic /proc liveness sweep
        // source id (0 = idle), and per-owner reconnect-grace timers keyed by ownerKey.
        this._overlayNameWatchId = 0;
        this._overlayCleanupId = 0;
        this._overlayReconnectTimers = new Map();
        this._clipboard = null;
        this._clipboardSelectionId = null;
    }

    enable() {
        try {
            const seat = Clutter.get_default_backend().get_default_seat();
            this._vPointer = seat.create_virtual_device(
                Clutter.InputDeviceType.POINTER_DEVICE);
        } catch (e) {
            env.logError(e, 'Keysharp: could not create virtual pointer device');
        }
        this._startProvider();

        // Export the D-Bus object first, then own the well-known name. Guard the whole D-Bus bring-up: if
        // export() or bus_own_name() throws, tear down whatever came up (exported object, name watch, virtual
        // pointer) and bail instead of leaving a half-initialized extension.
        // NOTE: Gio.DBus.session is a Gio.DBusConnection — it has no own_name()
        // method. The correct API is the free function Gio.bus_own_name().
        try {
            this._dbusImpl = Gio.DBusExportedObject.wrapJSObject(PUBLIC_IFACE_XML, this);
            this._dbusImpl.export(Gio.DBus.session, OBJECT_PATH);

            // Watch for client D-Bus connection names dropping off the bus. When one that owns overlays
            // disappears, _handleNameOwnerChanged arms a short reconnect grace timer and then reaps them.
            this._overlayNameWatchId = Gio.DBus.session.signal_subscribe(
                'org.freedesktop.DBus',
                'org.freedesktop.DBus',
                'NameOwnerChanged',
                '/org/freedesktop/DBus',
                null,
                Gio.DBusSignalFlags.NONE,
                (_conn, _sender, _path, _iface, _signal, parameters) => this._handleNameOwnerChanged(parameters));

            this._busNameId = Gio.bus_own_name(
                Gio.BusType.SESSION,
                SERVICE_NAME,
                Gio.BusNameOwnerFlags.NONE,
                null,   // bus_acquired_closure  (not needed; object is already exported)
                null,   // name_acquired_closure
                null);  // name_lost_closure
        } catch (e) {
            env.logError(e, 'Keysharp: could not export D-Bus service / own name');

            if (this._overlayNameWatchId !== 0) {
                try { Gio.DBus.session.signal_unsubscribe(this._overlayNameWatchId); } catch (_e) {}
                this._overlayNameWatchId = 0;
            }
            if (this._busNameId !== 0) {
                try { Gio.bus_unown_name(this._busNameId); } catch (_e) {}
                this._busNameId = 0;
            }
            if (this._dbusImpl !== null) {
                try { this._dbusImpl.unexport(); } catch (_e) {}
                this._dbusImpl = null;
            }
        }

        this._focusId = global.display.connect('notify::focus-window', () => {
            this._emitWindowEventRaw('active-state', this._getActiveWindow());
            const win = global.display.get_focus_window();
            if (win && this._isTrackedWindow(win))
                this._emitWindowEvent('active', win);
        });

        // The moment a reserved window can be placed WITHOUT a visible jump: 'map' fires as the actor is
        // about to be presented, before its first frame reaches the screen, so a move here is never seen.
        // The timer loop in _placeOnMap stays as the safety net for a map that slips past this.
        this._mapId = global.window_manager.connect('map', (_wm, actor) => {
            try {
                const win = actor ? actor.get_meta_window() : null;

                if (win && win.__ksPlaceNow)
                    win.__ksPlaceNow();
            } catch (_e) {
            }
        });

        // Emit WindowEvent('create') for newly mapped windows and hook their per-window
        // signals (title/minimize/move/close).
        this._winCreatedId = global.display.connect('window-created', (_disp, win) => {
            if (!win || !this._isTrackedWindow(win))
                return;
            this._placeOnMap(win);
            this._hookWindow(win);
            this._emitWindowEvent('create', win);
        });

        // Hook windows that already exist at enable time, without re-emitting create.
        for (const actor of global.get_window_actors()) {
            const win = this._liveMetaWindow(actor);
            if (win && this._isTrackedWindow(win))
                this._hookWindow(win);
        }

        // Clipboard change notification: the shell owns the selection, so MetaSelection's owner-changed fires
        // for every clipboard change regardless of which client (or none) is focused — the focus-independent
        // path a background app otherwise can't get on Wayland (Mutter exposes no data-control protocol).
        try {
            this._clipboard = St.Clipboard.get_default();
            this._clipboardSelectionId = global.display.get_selection().connect('owner-changed',
                (_selection, selectionType, _source) => {
                    if (selectionType === Meta.SelectionType.SELECTION_CLIPBOARD)
                        this._emitClipboardChanged();
                });
        } catch (e) {
            env.logError(e, 'Keysharp: could not hook clipboard selection');
        }
    }

    disable() {
        this._stopProvider();
        this._vPointer = null;
        if (this._clipboardSelectionId !== null) {
            try { global.display.get_selection().disconnect(this._clipboardSelectionId); } catch (_e) {}
            this._clipboardSelectionId = null;
        }
        this._clipboard = null;

        // Tear down any overlays still on screen, plus the owner-death cleanup machinery.
        for (const key of [...this._highlights.keys()])
            this._removeHighlightByKey(key);

        for (const key of [...this._imageOverlays.keys()])
            this._removeImageOverlayByKey(key);

        for (const key of [...this._tooltips.keys()])
            this._removeTooltipByKey(key);

        this._stopOverlayCleanupTimer();
        this._cancelAllOverlayReconnectTimers();

        if (this._overlayNameWatchId !== 0) {
            try { Gio.DBus.session.signal_unsubscribe(this._overlayNameWatchId); } catch (_e) {}
            this._overlayNameWatchId = 0;
        }

        if (this._winCreatedId !== null) {
            global.display.disconnect(this._winCreatedId);
            this._winCreatedId = null;
        }

        if (this._mapId) {
            global.window_manager.disconnect(this._mapId);
            this._mapId = 0;
        }

        for (const id of this._placementSources) {
            try { GLib.source_remove(id); } catch (_e) {}
        }

        this._placementSources.clear();

        for (const actor of global.get_window_actors()) {
            const win = actor.get_meta_window();
            if (win)
                this._unhookWindow(win);
        }

        if (this._focusId !== null) {
            global.display.disconnect(this._focusId);
            this._focusId = null;
        }

        if (this._busNameId !== 0) {
            Gio.bus_unown_name(this._busNameId);
            this._busNameId = 0;
        }

        if (this._dbusImpl !== null) {
            this._dbusImpl.unexport();
            this._dbusImpl = null;
        }

    }

    _credentialUid(credentials) {
        try {
            const value = credentials.get_unix_user();
            if (Array.isArray(value))
                return Number(value.length > 1 ? value[1] : value[0]);
            return Number(value);
        } catch (_e) {
            return -1;
        }
    }

    _connectionCredentialUid(connection) {
        let uid = this._credentialUid(connection.get_peer_credentials());
        if (uid >= 0)
            return uid;

        try {
            const stream = connection.get_stream();
            if (stream === null || typeof stream.get_socket !== 'function')
                return -1;
            uid = this._credentialUid(stream.get_socket().get_credentials());
        } catch (_e) {
            return -1;
        }
        return uid;
    }

    _startProvider() {
        try {
            const runtimeDirectory = GLib.build_filenamev([
                GLib.get_user_runtime_dir(), 'keysharp-desktop']);
            if (GLib.mkdir_with_parents(runtimeDirectory, 0o700) !== 0)
                throw new Error('Could not create the provider runtime directory.');
            GLib.chmod(runtimeDirectory, 0o700);
            this._providerSocketPath = GLib.build_filenamev([
                runtimeDirectory, PROVIDER_SOCKET_NAME]);
            if (GLib.file_test(this._providerSocketPath, GLib.FileTest.EXISTS))
                GLib.unlink(this._providerSocketPath);

            // Authentication runs on a GDBus worker thread, which cannot call
            // into GJS safely. Check the kernel credentials on the main-context
            // new-connection signal before exporting any object instead.
            this._providerServer = Gio.DBusServer.new_sync(
                `unix:path=${this._providerSocketPath}`,
                Gio.DBusServerFlags.NONE,
                Gio.dbus_generate_guid(),
                null,
                null);
            this._providerServerSignalId = this._providerServer.connect(
                'new-connection', (_server, connection) => {
                    if (this._connectionCredentialUid(connection) !== 0)
                        return false;
                    try {
                        const implementation = Gio.DBusExportedObject.wrapJSObject(
                            DBUS_IFACE_XML, this);
                        implementation.export(connection, PROVIDER_OBJECT_PATH);
                        const closedSignalId = connection.connect('closed', () => {
                            const entry = this._providerConnections.get(connection);
                            if (entry !== undefined) {
                                try { entry.implementation.unexport(); } catch (_e) {}
                                this._providerConnections.delete(connection);
                            }
                        });
                        this._providerConnections.set(connection,
                            {implementation, closedSignalId});
                        connection.start_message_processing();
                        return true;
                    } catch (e) {
                        env.logError(e, 'Keysharp: could not export private provider connection');
                        return false;
                    }
                });
            this._providerServer.start();
        } catch (e) {
            env.logError(e, 'Keysharp: could not start private desktop provider');
            this._stopProvider();
        }
    }

    _stopProvider() {
        if (this._providerServer !== null) {
            if (this._providerServerSignalId !== 0) {
                try { this._providerServer.disconnect(this._providerServerSignalId); } catch (_e) {}
                this._providerServerSignalId = 0;
            }
            try { this._providerServer.stop(); } catch (_e) {}
            this._providerServer = null;
        }
        for (const [connection, entry] of this._providerConnections.entries()) {
            try { entry.implementation.unexport(); } catch (_e) {}
            try { connection.disconnect(entry.closedSignalId); } catch (_e) {}
            try { connection.close_sync(null); } catch (_e) {}
        }
        this._providerConnections.clear();
        if (this._providerSocketPath !== null) {
            try { GLib.unlink(this._providerSocketPath); } catch (_e) {}
            this._providerSocketPath = null;
        }
    }

    // ----------------------------------------------------------------
    // D-Bus method implementations
    // ----------------------------------------------------------------

    _getWindowList(includeHidden) {
        try {
            const actors  = global.get_window_actors();
            const windows = [];

            for (let i = 0; i < actors.length; i++) {
                const win = this._liveMetaWindow(actors[i]);
                if (!win || !this._isTrackedWindow(win))
                    continue;
                if (!includeHidden && win.minimized)
                    continue;
                windows.push(this._windowInfo(win));
            }

            return JSON.stringify({ok: true, windows});
        } catch (e) {
            return JSON.stringify({ok: false, error: String(e), windows: []});
        }
    }

    _getActiveWindow() {
        try {
            const win = global.display.get_focus_window();
            return JSON.stringify({
                ok: true,
                window: (win && this._isLiveWindow(win) && this._isTrackedWindow(win))
                    ? this._windowInfo(win)
                    : null,
            });
        } catch (e) {
            return JSON.stringify({ok: false, window: null});
        }
    }

    _requireProviderPeer(invocation) {
        if (this._callerIsProviderPeer(invocation))
            return true;

        invocation.return_error_literal(Gio.IOErrorEnum, Gio.IOErrorEnum.PERMISSION_DENIED,
            'This operation is only permitted through keysharp-desktop.');
        return false;
    }

    _returnSensitiveBoolean(params, invocation, method) {
        if (!this._requireProviderPeer(invocation))
            return;

        let ok = false;
        try { ok = Boolean(this[method](...params)); } catch (_e) {}
        invocation.return_value(new GLib.Variant('(b)', [ok]));
    }

    GetWindowListAsync(params, invocation) {
        if (!this._requireProviderPeer(invocation))
            return;
        invocation.return_value(new GLib.Variant('(s)', [this._getWindowList(Boolean(params[0]))]));
    }

    QueryWindowAsync(params, invocation) {
        if (!this._requireProviderPeer(invocation))
            return;
        try {
            const win = this._findWindow(params[0]);
            if (win) {
                const json = JSON.stringify({ok: true, window: this._windowInfo(win)});
                invocation.return_value(new GLib.Variant('(s)', [json]));
                return;
            }
        } catch (_e) {}
        invocation.return_error_literal(Gio.IOErrorEnum, Gio.IOErrorEnum.NOT_FOUND,
            'Window not found.');
    }

    GetActiveWindowAsync(_params, invocation) {
        if (!this._requireProviderPeer(invocation))
            return;
        invocation.return_value(new GLib.Variant('(s)', [this._getActiveWindow()]));
    }

    GetClipboardMimetypesAsync(_params, invocation) {
        if (!this._requireProviderPeer(invocation))
            return;
        invocation.return_value(new GLib.Variant('(as)', [this._getClipboardMimetypes()]));
    }

    SetClipboardContentAsync(params, invocation) { this._returnSensitiveBoolean(params, invocation, '_setClipboardContent'); }

    FocusWindowAsync(params, invocation) { this._returnSensitiveBoolean(params, invocation, '_focusWindow'); }
    RaiseWindowAsync(params, invocation) { this._returnSensitiveBoolean(params, invocation, '_raiseWindow'); }
    LowerWindowAsync(params, invocation) { this._returnSensitiveBoolean(params, invocation, '_lowerWindow'); }
    ReserveWindowAsync(params, invocation) { this._returnSensitiveBoolean(params, invocation, '_reserveWindow'); }
    GetReservedWindowAsync(params, invocation) {
        if (!this._requireProviderPeer(invocation))
            return;
        let id = '';
        try { id = String(this._getReservedWindow(...params) || ''); } catch (_e) {}
        invocation.return_value(new GLib.Variant('(s)', [id]));
    }
    MoveResizeWindowAsync(params, invocation) { this._returnSensitiveBoolean(params, invocation, '_moveResizeWindow'); }
    MoveResizeWindowByXidAsync(params, invocation) { this._returnSensitiveBoolean(params, invocation, '_moveResizeWindowByXid'); }
    SetWindowStateAsync(params, invocation) { this._returnSensitiveBoolean(params, invocation, '_setWindowState'); }
    SetWindowAboveAsync(params, invocation) { this._returnSensitiveBoolean(params, invocation, '_setWindowAbove'); }
    SetWindowDecoratedAsync(params, invocation) { this._returnSensitiveBoolean(params, invocation, '_setWindowDecorated'); }
    SetWindowOpacityAsync(params, invocation) { this._returnSensitiveBoolean(params, invocation, '_setWindowOpacity'); }
    CloseWindowAsync(params, invocation) { this._returnSensitiveBoolean(params, invocation, '_closeWindow'); }
    KillWindowAsync(params, invocation) { this._returnSensitiveBoolean(params, invocation, '_killWindow'); }
    SendMouseMoveAbsoluteAsync(params, invocation) { this._returnSensitiveBoolean(params, invocation, '_sendMouseMoveAbsolute'); }
    SendMouseMoveRelativeAsync(params, invocation) { this._returnSensitiveBoolean(params, invocation, '_sendMouseMoveRelative'); }
    SendMouseButtonAsync(params, invocation) { this._returnSensitiveBoolean(params, invocation, '_sendMouseButton'); }
    SendMouseScrollAsync(params, invocation) { this._returnSensitiveBoolean(params, invocation, '_sendMouseScroll'); }

    _overlayCallerMatches(params, invocation, ownerIndex, busIndex) {
        try {
            const sender = invocation.get_sender();
            if (!sender || String(params[busIndex] || '') !== sender)
                return false;
            const reply = Gio.DBus.session.call_sync(
                'org.freedesktop.DBus', '/org/freedesktop/DBus',
                'org.freedesktop.DBus', 'GetConnectionUnixProcessID',
                new GLib.Variant('(s)', [sender]), new GLib.VariantType('(u)'),
                Gio.DBusCallFlags.NONE, 1000, null);
            const [pid] = reply.deep_unpack();
            const owner = this._parseOverlayOwner(params[ownerIndex]);
            if (!Number.isInteger(pid) || pid <= 0 || owner.pid !== pid
                || !owner.startTime || owner.key !== `${pid}:${owner.startTime}`)
                return false;
            const [ok, bytes] = GLib.file_get_contents(`/proc/${pid}/stat`);
            if (!ok)
                return false;
            const stat = env.decodeBytes(bytes);
            const end = stat.lastIndexOf(')');
            if (end < 0 || end + 2 >= stat.length)
                return false;
            const fields = stat.substring(end + 2).trim().split(/\s+/);
            return fields.length > 19 && fields[19] === owner.startTime;
        } catch (_e) {
            return false;
        }
    }

    _returnOwnedOverlayBoolean(params, invocation, method, ownerIndex, busIndex) {
        let ok = false;
        if (this._overlayCallerMatches(params, invocation, ownerIndex, busIndex)) {
            try { ok = Boolean(this[method](...params)); } catch (_e) {}
        }
        invocation.return_value(new GLib.Variant('(b)', [ok]));
    }

    RegisterHighlightOwnerAsync(params, invocation) { this._returnOwnedOverlayBoolean(params, invocation, '_registerHighlightOwner', 0, 1); }
    ShowHighlightAsync(params, invocation) { this._returnOwnedOverlayBoolean(params, invocation, '_showHighlight', 1, 2); }
    HideHighlightAsync(params, invocation) { this._returnOwnedOverlayBoolean(params, invocation, '_hideHighlight', 1, 2); }
    ShowImageOverlayAsync(params, invocation) { this._returnOwnedOverlayBoolean(params, invocation, '_showImageOverlay', 1, 2); }
    MoveImageOverlayAsync(params, invocation) { this._returnOwnedOverlayBoolean(params, invocation, '_moveImageOverlay', 1, 2); }
    HideImageOverlayAsync(params, invocation) { this._returnOwnedOverlayBoolean(params, invocation, '_hideImageOverlay', 1, 2); }
    ShowTooltipAsync(params, invocation) { this._returnOwnedOverlayBoolean(params, invocation, '_showTooltip', 1, 2); }
    HideTooltipAsync(params, invocation) { this._returnOwnedOverlayBoolean(params, invocation, '_hideTooltip', 1, 2); }

    // Two out-parameters map to a two-element array in GJS D-Bus dispatch.
    GetCursorPosition() {
        const point = global.get_pointer();
        return [Math.round(point[0]), Math.round(point[1])];
    }

    GetWorkArea() {
        return env.workArea();
    }

    // --- Clipboard --------------------------------------------------------------------------------------
    // MetaSelection is the focus-independent, all-MIME path (St.Clipboard alone can only *get* text):
    // get_mimetypes lists the types, transfer_async streams one type's bytes, and a MetaSelectionSourceMemory
    // owner writes one type.
    _selectionObj() {
        return global.display.get_selection();
    }

    _mimetypes() {
        try {
            return (this._selectionObj().get_mimetypes(
                Meta.SelectionType.SELECTION_CLIPBOARD) || [])
                .filter(value => typeof value === 'string'
                    && value.length > 0 && value.length <= 1024)
                .slice(0, 256);
        } catch (_e) {
            return [];
        }
    }

    _getClipboardMimetypes() {
        return this._mimetypes();
    }

    // Async: MetaSelection.transfer_async streams the content of one MIME type into a memory output stream.
    GetClipboardContentAsync(params, invocation) {
        if (!this._requireProviderPeer(invocation))
            return;

        const [mimetype] = params;
        if (typeof mimetype !== 'string' || mimetype.length === 0
            || mimetype.length > 1024) {
            invocation.return_value(new GLib.Variant('(ay)', [new Uint8Array(0)]));
            return;
        }
        try {
            const stream = Gio.MemoryOutputStream.new_resizable();
            this._selectionObj().transfer_async(Meta.SelectionType.SELECTION_CLIPBOARD, String(mimetype), MAX_CLIPBOARD_BYTES + 1, stream, null,
                (sel, res) => {
                    let bytes = new Uint8Array(0);
                    try {
                        sel.transfer_finish(res);
                        stream.close(null);
                        const data = stream.steal_as_bytes().get_data();
                        if (data) {
                            const candidate = data instanceof Uint8Array
                                ? data : new Uint8Array(data);
                            if (candidate.length <= MAX_CLIPBOARD_BYTES)
                                bytes = candidate;
                        }
                    } catch (_e) {
                    }
                    try { invocation.return_value(new GLib.Variant('(ay)', [bytes])); } catch (_e2) {}
                });
        } catch (_e) {
            try { invocation.return_value(new GLib.Variant('(ay)', [new Uint8Array(0)])); } catch (_e2) {}
        }
    }

    _setClipboardContent(mimetype, bytes) {
        try {
            const data = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes || []);
            const type = String(mimetype || '');
            if (type.length === 0 || type.length > 1024
                || data.length > MAX_CLIPBOARD_BYTES)
                return false;
            const source = Meta.SelectionSourceMemory.new(type, new GLib.Bytes(data));
            this._selectionObj().set_owner(Meta.SelectionType.SELECTION_CLIPBOARD, source);
            return true;
        } catch (e) {
            env.logError(e, 'Keysharp: SetClipboardContent failed');
            return false;
        }
    }

    // Text reads use the common A_Clipboard path via St.Clipboard.get_text (async). Writing text is not a
    // provider method: keysharp-desktop encodes UTF-8 itself and calls SetClipboardContent, so the owner is
    // a proper MetaSelection source and one scope check covers every format.
    GetClipboardTextAsync(_params, invocation) {
        if (!this._requireProviderPeer(invocation))
            return;

        try {
            (this._clipboard || St.Clipboard.get_default()).get_text(St.ClipboardType.CLIPBOARD, (_cb, text) => {
                const value = String(text || '');
                try { invocation.return_value(new GLib.Variant('(s)',
                    [value.length <= MAX_CLIPBOARD_BYTES / 4 ? value : ''])); } catch (_e) {}
            });
        } catch (_e) {
            try { invocation.return_value(new GLib.Variant('(s)', [''])); } catch (_e2) {}
        }
    }

    _emitClipboardChanged() {
        if (this._providerConnections.size === 0)
            return;

        const mimetypes = this._mimetypes();
        try {
            (this._clipboard || St.Clipboard.get_default()).get_text(St.ClipboardType.CLIPBOARD, (_cb, text) => {
                if (this._providerConnections.size === 0)
                    return;
                try {
                    const value = String(text || '');
                    this._emitProviderSignal('ClipboardChanged',
                        new GLib.Variant('(sas)',
                            [value.length <= MAX_CLIPBOARD_BYTES / 4 ? value : '',
                             mimetypes]));
                } catch (_e) {}
            });
        } catch (_e) {
        }
    }

    _focusWindow(handle) {
        const win = this._findWindow(handle);
        if (!win)
            return false;
        if (win.minimized)
            win.unminimize();
        try {
            Main.activateWindow(win);
            return true;
        } catch (_e) {
            try {
                win.activate(global.get_current_time());
                return true;
            } catch (_e2) {
                return false;
            }
        }
    }

    _raiseWindow(handle) {
        const win = this._findWindow(handle);
        if (!win)
            return false;

        try {
            if (typeof win.raise === 'function') {
                win.raise();
                return true;
            }
        } catch (_e) {
        }

        // Mutter versions without a public raise(): activating brings it to the front.
        return this._focusWindow(handle);
    }

    _lowerWindow(handle) {
        const win = this._findWindow(handle);
        if (!win)
            return false;

        try {
            if (typeof win.lower_with_transients === 'function') {
                win.lower_with_transients();
                return true;
            }
        } catch (_e) {
        }

        try {
            if (typeof win.lower === 'function') {
                win.lower();
                return true;
            }
        } catch (_e) {
        }

        return false;
    }

    _moveResizeWin(win, x, y, width, height) {
        if (!win)
            return false;

        // Moving a window that has no monitor yet recurses to death inside
        // move_resize_internal -> update_monitor -> wayland_update_main_monitor. Reachable here because a
        // reserved window is correlated as soon as it has geometry, which can precede its monitor.
        if (win.get_monitor() < 0)
            return false;

        // A maximized (or tiled) window ignores move/resize until it is restored — mirror how dragging its
        // titlebar first unmaximizes it. Unmaximize BEFORE reading the frame rect so an unchanged-size move
        // keeps the restored size, not the maximized one.
        if (win.maximized_horizontally || win.maximized_vertically)
            win.unmaximize(env.maximizeFlags);

        // Read the current frame rect so we can substitute unchanged axes.
        const frame = win.get_frame_rect();
        const nx = (x !== INT32_MIN) ? x : frame.x;
        const ny = (y !== INT32_MIN) ? y : frame.y;
        const nw = (width  > 0) ? width  : frame.width;
        const nh = (height > 0) ? height : frame.height;

        // move_resize_frame is the single Mutter call for both move and resize;
        // win.resize() does not exist as a standalone public API in Mutter/GJS.
        // user_op = true marks this a user action, so Mutter applies the looser "keep-minimal on
        // screen" constraint of an interactive drag (allowing off-screen placement) instead of the
        // strict fully-on-screen clamp it forces on app ConfigureRequests. It does not steal focus.
        try {
            win.move_resize_frame(true, nx, ny, nw, nh);
            return true;
        } catch (_e) {
            return false;
        }
    }

    // Claim the NEXT window this pid creates under a caller-chosen cookie, so it can ask afterwards which
    // compositor window its own toplevel became. Consumed in creation order: Show() is synchronous, so the
    // oldest live reservation for a pid belongs to the next window it maps. The pid is the only identity
    // available this early - title, app_id and geometry are all still empty when a window is created, which
    // is exactly why a client cannot recognise its own window without this.
    _reserveWindow(pid, cookie, x, y, ttlMs) {
        if (pid <= 0)
            return false;

        try {
            this._sweepPlacements();
            const existing = this._placements.findIndex(p =>
                p.pid === pid && p.cookie === cookie);
            if (existing >= 0)
                this._placements.splice(existing, 1);
            else if (this._placements.length >= MAX_PLACEMENTS_TOTAL
                || this._placements.filter(p => p.pid === pid).length
                    >= MAX_PLACEMENTS_PER_PID)
                return false;
            this._placements.push({
                pid:     pid,
                cookie:  cookie,
                x:       x,
                y:       y,
                expires: GLib.get_monotonic_time() / 1000 + (ttlMs > 0 ? ttlMs : PLACEMENT_TTL_MS)
            });
            return true;
        } catch (_e) {
            return false;
        }
    }

    // The compositor window a reservation ended up on, or '' if it has not been consumed (or has expired).
    // Keyed by pid too: the cookie is a per-process pointer, so two Keysharp processes can mint the same one.
    _getReservedWindow(pid, cookie) {
        try {
            const now = GLib.get_monotonic_time() / 1000;
            const hit = this._placed[pid + ':' + cookie];

            if (!hit)
                return '';

            delete this._placed[pid + ':' + cookie];
            return hit.expires > now ? hit.id : '';
        } catch (_e) {
            return '';
        }
    }

    _sweepPlacements() {
        const now = GLib.get_monotonic_time() / 1000;
        this._placements = this._placements.filter(p => p.expires > now);

        for (const key in this._placed) {
            if (this._placed[key].expires <= now)
                delete this._placed[key];
        }
    }

    // Consume a reservation for a just-created window: record which compositor window it became, and place
    // it if a position was reserved.
    //
    // The placement is what a client cannot do for itself. It has to wait for the window to exist, learn which
    // one is its own, then move it - by which time the window has been painted where the compositor chose. Here
    // the move happens before it is ever painted.
    //
    // Two hazards, both learned the hard way. A window has no monitor at creation, and moving it then recurses
    // to death inside move_resize_internal -> update_monitor -> wayland_update_main_monitor, so wait for one.
    // And Muffin emits the geometry signals from INSIDE move_resize_internal, so moving from a signal handler
    // re-enters it: the move is posted to run before the next paint instead, outside that call stack.
    _placeOnMap(win) {
        let p = null;

        try {
            this._sweepPlacements();
            const pid = this._clientPid(win, true);

            if (pid <= 0 || this._placements.length === 0)
                return;

            const i = this._placements.findIndex(q => q.pid === pid);

            if (i < 0)
                return;

            p = this._placements.splice(i, 1)[0];

            if (p.cookie) {
                const key = p.pid + ':' + p.cookie;
                if (Object.prototype.hasOwnProperty.call(this._placed, key)
                    || Object.keys(this._placed).length < MAX_PLACED_TOTAL)
                    this._placed[key] = {
                        id: String(win.get_stable_sequence()),
                        expires: p.expires,
                    };
            }
        } catch (_e) {
            return;
        }

        if (!p || (p.x === INT32_MIN && p.y === INT32_MIN))
            return;

        // Wait for the window to become placeable, then move it and hold it there. Never move before
        // get_monitor() answers: a monitorless window recurses to death through
        // move_resize_internal -> update_monitor -> wayland_update_main_monitor.
        let left = PLACEMENT_MAX_TRIES;
        let moved = 0;
        let stable = 0;

        // Every source is tracked so disable() can cancel it: a disabled extension must stop moving windows.
        const defer = (ms, fn) => {
            let id = 0;
            id = GLib.timeout_add(GLib.PRIORITY_DEFAULT, ms, () => {
                this._placementSources.delete(id);
                fn();
                return GLib.SOURCE_REMOVE;
            });
            this._placementSources.add(id);
            return id;
        };

        const retry = () => defer(PLACEMENT_RETRY_MS, attempt);

        // The compositor's lazy geometry sync can yank the painted actor to a stale position while the
        // window's own buffer rect keeps saying where it belongs. Correcting from notify:: runs synchronously
        // inside whatever moved the property - BEFORE the next paint - so a yank never reaches the screen,
        // unlike a timer correction which can be a frame late. Snapping to the live buffer rect also cannot
        // fight a legitimate move, which updates the buffer rect with it.
        // The yank pathology was measured on Muffin; on a compositor that keeps the actor synced, the >1px
        // gate leaves this inert. If windows ever stutter for ~600ms after Show (fractional scaling is the
        // plausible trigger), suspect this block first.
        let watchIds = [];
        let fixing = false;
        let fixes = 0;

        const disarmWatch = (actor) => {
            for (const id of watchIds) {
                try { actor.disconnect(id); } catch (_e) {}
            }

            watchIds = [];
        };

        const armWatch = (actor) => {
            if (watchIds.length)
                return;

            const fix = () => {
                if (fixing || fixes > 60)
                    return;

                try {
                    const br = win.get_buffer_rect();

                    if (Math.abs(actor.x - br.x) > 1 || Math.abs(actor.y - br.y) > 1) {
                        fixing = true;
                        fixes++;
                        actor.set_position(br.x, br.y);
                        fixing = false;
                    }
                } catch (_e) {
                }
            };

            try {
                watchIds.push(actor.connect('notify::x', fix));
                watchIds.push(actor.connect('notify::y', fix));

                defer(PLACEMENT_WATCH_MS, () => disarmWatch(actor));
            } catch (_e) {
            }
        };

        const attempt = () => {
            try {
                if (--left <= 0) {
                    delete win.__ksPlaceNow;
                    return;
                }

                if (win.get_monitor() < 0) {
                    retry();
                    return;
                }

                const f = win.get_frame_rect();
                const tx = p.x !== INT32_MIN ? p.x : f.x;
                const ty = p.y !== INT32_MIN ? p.y : f.y;

                if (moved > 0 && f.x === tx && f.y === ty) {
                    // The rect being right is only half the job: the ACTOR - the thing actually painted - is
                    // synced to it lazily and can still be yanked to a stale position tens of ms later. Only
                    // finish once it exists and has sat at the window's buffer rect for two consecutive
                    // ticks; a rect that converged before the actor existed would otherwise paint at its
                    // spawn position unwatched.
                    const held = win.get_compositor_private();

                    if (!held) {
                        retry();
                        return;
                    }

                    armWatch(held);

                    const br = win.get_buffer_rect();

                    if (Math.abs(held.x - br.x) > 1 || Math.abs(held.y - br.y) > 1) {
                        held.set_position(br.x, br.y);
                        stable = 0;
                        retry();
                        return;
                    }

                    if (++stable < 2) {
                        retry();
                        return;
                    }

                    delete win.__ksPlaceNow;
                    return;
                }

                // Position only, never a size. The size the client chose is already on its way, and a resize
                // it never asked for is at best held until the client acks it - the position with it - and at
                // worst is what crashes the compositor when issued this early.
                win.move_frame(true, tx, ty);

                // Carry the actor along: the buffer rect is where it belongs now that the rect has moved.
                const a2 = win.get_compositor_private();

                if (a2) {
                    const nbr = win.get_buffer_rect();
                    a2.set_position(nbr.x, nbr.y);
                    armWatch(a2);
                }

                moved++;
                stable = 0;
                retry();
            } catch (_e) {
                delete win.__ksPlaceNow;
            }
        };

        win.__ksPlaceNow = attempt;

        // Deferred out of the window-created signal, but only just: an idle fires long before anything is
        // presented, and pre-show moves both seed the rect and mean the map-time correction is tiny.
        defer(0, attempt);
    }

    _moveResizeWindow(handle, x, y, width, height) {
        return this._moveResizeWin(this._findWindow(handle), x, y, width, height);
    }

    // Move by X11 window id. On an X11 session the caller identifies windows by their XID
    // (XQueryTree / _NET_CLIENT_LIST), not the Mutter stable_sequence, so route the move through the
    // compositor this way to reach off-screen placement a raw XMoveWindow can't (Mutter clamps that
    // on screen).
    _moveResizeWindowByXid(xid, x, y, width, height) {
        return this._moveResizeWin(this._findWindowByXid(xid), x, y, width, height);
    }

    _setWindowState(handle, state) {
        const win = this._findWindow(handle);
        if (!win)
            return false;

        try {
            if (state === STATE_MINIMIZED) {
                win.minimize();
            } else if (state === STATE_MAXIMIZED) {
                if (win.minimized)
                    win.unminimize();
                win.maximize(env.maximizeFlags);
            } else {
                if (win.minimized)
                    win.unminimize();
                win.unmaximize(env.maximizeFlags);
            }
            return true;
        } catch (_e) {
            return false;
        }
    }

    _setWindowAbove(handle, above) {
        const win = this._findWindow(handle);
        if (!win)
            return false;

        try {
            if (above) {
                if (!win.is_above())
                    win.make_above();
            } else if (win.is_above()) {
                win.unmake_above();
            }
            return true;
        } catch (_e) {
            return false;
        }
    }

    _closeWindow(handle) {
        const win = this._findWindow(handle);
        if (!win)
            return false;
        try {
            win.delete(global.get_current_time());
            return true;
        } catch (_e) {
            return false;
        }
    }

    _killWindow(handle) {
        const win = this._findWindow(handle);
        if (!win)
            return false;

        try {
            if (typeof win.kill === 'function') {
                win.kill();
                return true;
            }
        } catch (_e) {
        }

        // No kill() on this Mutter version: fall back to a graceful close.
        try {
            win.delete(global.get_current_time());
            return true;
        } catch (_e) {
            return false;
        }
    }

    // Virtual pointer operations are reachable only through the private provider interface.
    _sendMouseMoveAbsolute(x, y) {
        if (this._vPointer === null)
            return false;
        this._vPointer.notify_absolute_motion(GLib.get_monotonic_time(), x, y);
        return true;
    }

    _sendMouseMoveRelative(dx, dy) {
        if (this._vPointer === null)
            return false;
        const point = global.get_pointer();
        this._vPointer.notify_absolute_motion(
            GLib.get_monotonic_time(), point[0] + dx, point[1] + dy);
        return true;
    }

    _sendMouseButton(button, pressed) {
        if (this._vPointer === null || [1, 2, 3, 8, 9].indexOf(button) < 0)
            return false;
        this._vPointer.notify_button(
            GLib.get_monotonic_time(), button,
            pressed ? Clutter.ButtonState.PRESSED : Clutter.ButtonState.RELEASED);
        return true;
    }

    _sendMouseScroll(delta, vertical) {
        if (this._vPointer === null || !Number.isInteger(delta)
            || delta === 0 || Math.abs(delta) > 12000)
            return false;
        const direction = vertical
            ? (delta > 0 ? Clutter.ScrollDirection.UP : Clutter.ScrollDirection.DOWN)
            : (delta > 0 ? Clutter.ScrollDirection.RIGHT : Clutter.ScrollDirection.LEFT);
        const time = GLib.get_monotonic_time();
        const notches = Math.max(1, Math.abs(Math.round(delta / 120)));
        for (let index = 0; index < notches; index++)
            this._vPointer.notify_discrete_scroll(
                time, direction, Clutter.ScrollSource.WHEEL);
        return true;
    }

    _setWindowDecorated(handle, decorated) {
        const win = this._findWindow(handle);
        if (!win)
            return false;

        try {
            return env.setDecorated(win, decorated);
        } catch (e) {
            env.logError(e, 'Keysharp: SetWindowDecorated failed');
            return false;
        }
    }

    // Set the window's opacity by setting it on its compositor actor. opacity is 0 (transparent)..255 (opaque).
    _setWindowOpacity(handle, opacity) {
        const win = this._findWindow(handle);
        if (!win)
            return false;

        const value = Math.max(0, Math.min(255, Math.round(Number(opacity))));

        try {
            const actor = (typeof win.get_compositor_private === 'function')
                ? win.get_compositor_private()
                : null;

            if (!actor)
                return false;

            if (typeof actor.set_opacity === 'function')
                actor.set_opacity(value);
            else
                actor.opacity = value;

            return true;
        } catch (_e) {
            return false;
        }
    }

    // The installed, root-owned keysharp-desktop broker is the provider entry point for screen capture,
    // global window operations, and clipboard observation. It checks the matching grant before calling us.
    // Capture uses the async D-Bus form so the invocation identifies the caller and can be completed later.
    // Truly async: the D-Bus reply is sent from the screenshot callback, so the shell's main loop
    // keeps running normally in between — no nested main-context pump inside the compositor (which
    // would re-dispatch input/D-Bus re-entrantly and unwind concurrent captures in LIFO order).
    _returnCapture(invocation, label, capture) {
        if (!this._requireProviderPeer(invocation))
            return;

        let replied = false;
        const reply = bytes => {
            if (replied)
                return;
            replied = true;
            const value = bytes && bytes.length > 0 && bytes.length <= MAX_CAPTURE_BYTES
                ? bytes : new Uint8Array(0);
            invocation.return_value(new GLib.Variant('(ay)', [value]));
        };

        try {
            capture(reply);
        } catch (e) {
            env.logError(e, `Keysharp: ${label} failed`);
            reply(null);
        }
    }

    CaptureAreaAsync(params, invocation) {
        this._returnCapture(invocation, 'CaptureArea', reply => {
            if (env.captureArea === null)
                reply(null);
            else
                env.captureArea(this, params, reply);
        });
    }

    CaptureWindowAsync(params, invocation) {
        this._returnCapture(invocation, 'CaptureWindow', reply => {
            env.captureWindow(this, params[0], Boolean(params[1]), reply);
        });
    }
    _callerIsProviderPeer(invocation) {
        try {
            const connection = invocation.get_connection();
            return this._providerConnections.has(connection)
                && this._connectionCredentialUid(connection) === 0;
        } catch (_e) {
            return false;
        }
    }

    // Register a client as an overlay owner. The client re-registers whenever its D-Bus connection is
    // (re)established; this re-stamps the current busName onto any overlays it already owns and cancels a
    // pending reconnect-grace deletion, so a client that merely reconnected keeps its overlays.
    _registerHighlightOwner(ownerKey, busName) {
        const owner = this._parseOverlayOwner(ownerKey);
        const bus = String(busName || '');

        // Registration is the first extension call made by a new Keysharp process. Reap dead owners here as
        // well as from the periodic timer so stale actors/content cannot accumulate across process launches if
        // GNOME delayed or dropped that timer while the shell was busy (for example during screen capture).
        // Live owners are retained, so concurrently-running HUD scripts are unaffected.
        this._cleanupDeadOverlays();
        this._cancelOverlayReconnectTimer(owner.key);

        for (const entry of this._highlights.values()) {
            if (entry.ownerKey === owner.key) {
                entry.ownerPid = owner.pid;
                entry.ownerStartTime = owner.startTime;
                entry.busName = bus;
            }
        }

        for (const entry of this._imageOverlays.values()) {
            if (entry.ownerKey === owner.key) {
                entry.ownerPid = owner.pid;
                entry.ownerStartTime = owner.startTime;
                entry.busName = bus;
            }
        }

        for (const entry of this._tooltips.values()) {
            if (entry.ownerKey === owner.key) {
                entry.ownerPid = owner.pid;
                entry.ownerStartTime = owner.startTime;
                entry.busName = bus;
            }
        }

        return true;
    }

    // Draw or update a click-through rectangle-outline overlay. GNOME has no wlr-layer-shell, so
    // instead of a layer surface we add four edge actors to the shell's top chrome. Two things make
    // them click-through: reactive:false (they never grab events) and addTopChrome(..., {
    // affectsInputRegion: false }) (they are excluded from the shell's input region, so clicks fall
    // through to the window beneath — the compositor-side equivalent of the layer surface's empty
    // input region on KWin). Four edges (rather than one filled rect) keep the centre visually clear.
    // The overlay is tagged with its owner (see _overlayKey) so it can be reaped if the client dies.
    _showHighlight(id, ownerKey, busName, x, y, width, height, color, thickness) {
        try {
            const owner = this._parseOverlayOwner(ownerKey);
            const key = this._overlayKey(id, owner.key);
            const bus = String(busName || '');

            if ((width >= 1 || height >= 1)
                && (!this._validOverlayGeometry(width, height)
                    || !this._overlayQuotaAvailable(owner.key,
                                                    this._highlights.has(key))))
                return false;

            this._cancelOverlayReconnectTimer(owner.key);
            this._removeHighlightByKey(key);

            if (width < 1 || height < 1) {
                if (!this._hasAnyOverlays())
                    this._stopOverlayCleanupTimer();
                return true;
            }

            const t   = Math.max(1, thickness);
            const css = `background-color: #${color};`;

            const rects = [
                [x,             y,              width,             t],                          // top
                [x,             y + height - t, width,             t],                          // bottom
                [x,             y + t,          t,                 Math.max(0, height - 2 * t)],// left
                [x + width - t, y + t,          t,                 Math.max(0, height - 2 * t)],// right
            ];

            const edges = [];

            for (const [ex, ey, ew, eh] of rects) {
                if (ew < 1 || eh < 1)
                    continue;

                const a = new St.Widget({ reactive: false, style: css });
                a.set_position(ex, ey);
                a.set_size(ew, eh);
                env.addClickThroughChrome(a);
                edges.push(a);
            }

            this._highlights.set(key, {
                edges: edges,
                ownerKey: owner.key,
                ownerPid: owner.pid,
                ownerStartTime: owner.startTime,
                busName: bus,
            });
            this._ensureOverlayCleanupTimer();
            return true;
        } catch (e) {
            env.logError(e, 'Keysharp: ShowHighlight failed');
            return false;
        }
    }

    _hideHighlight(id, ownerKey, _busName) {
        const owner = this._parseOverlayOwner(ownerKey);
        this._removeHighlightByKey(this._overlayKey(id, owner.key));

        if (!this._hasOverlaysForOwner(owner.key))
            this._cancelOverlayReconnectTimer(owner.key);

        if (!this._hasAnyOverlays())
            this._stopOverlayCleanupTimer();

        return true;
    }

    _showImageOverlay(id, ownerKey, busName, x, y, width, height, pngData) {
        try {
            const owner = this._parseOverlayOwner(ownerKey);
            const key = this._overlayKey(id, owner.key);

            this._cancelOverlayReconnectTimer(owner.key);

            // Empty pixels = a clear request.
            if (!pngData || pngData.length === 0 || width < 1 || height < 1)
                return this._clearImageOverlay(key);

            if (!this._validOverlayGeometry(width, height)
                || pngData.length > MAX_OVERLAY_PNG_BYTES
                || !this._overlayQuotaAvailable(owner.key,
                                                this._imageOverlays.has(key)))
                return false;

            return this._presentImageOverlay(key, owner, busName, x, y, width, height,
                () => this._decodeImageFrame(pngData));
        } catch (e) {
            env.logError(e, 'Keysharp: ShowImageOverlay failed');
            return false;
        }
    }

    // An empty frame is how a client asks for the overlay to go away.
    _clearImageOverlay(key) {
        this._removeImageOverlayByKey(key);

        if (!this._hasAnyOverlays())
            this._stopOverlayCleanupTimer();

        return true;
    }

    // The shared tail of both Show paths: makeFrame() produces the pixels -- decoded, or mapped out of the
    // client's shared buffer -- and everything from there on is one actor, created once and updated after.
    _presentImageOverlay(key, owner, busName, x, y, width, height, makeFrame) {
        const bus = String(busName || '');
        const existing = this._imageOverlays.get(key);
        const frame = makeFrame(existing);

        if (!frame || !this._validOverlayGeometry(frame.width, frame.height)
            || frame.rowStride < frame.width * 3
            || frame.rowStride * frame.height > MAX_OVERLAY_PIXELS * 4)
            return false;
        const decodedBytes = Math.max(frame.rowStride * frame.height,
                                      frame.width * frame.height * 4);
        if (!Number.isSafeInteger(decodedBytes)
            || !this._overlayBytesAvailable(owner.key, decodedBytes,
                                             existing))
            return false;

        if (existing && existing.actor) {
            try {
                this._updateImageActor(existing.actor, frame, x, y, width, height);
                existing.ownerPid = owner.pid;
                existing.ownerStartTime = owner.startTime;
                existing.busName = bus;
                existing.decodedBytes = decodedBytes;
                this._ensureOverlayCleanupTimer();
                return true;
            } catch (_e) {
                this._removeImageOverlayByKey(key);
            }
        }

        const actor = new Clutter.Actor({ reactive: false });
        this._updateImageActor(actor, frame, x, y, width, height);
        env.addClickThroughChrome(actor);
        this._imageOverlays.set(key, {
            actor: actor,
            ownerKey: owner.key,
            ownerPid: owner.pid,
            ownerStartTime: owner.startTime,
            busName: bus,
            decodedBytes: decodedBytes,
        });
        this._ensureOverlayCleanupTimer();
        return true;
    }

    _moveImageOverlay(id, ownerKey, _busName, x, y, width, height) {
        try {
            const owner = this._parseOverlayOwner(ownerKey);
            const entry = this._imageOverlays.get(this._overlayKey(id, owner.key));

            // No live actor for this id: report failure so the caller re-sends the pixels via ShowImageOverlay.
            if (!entry || !entry.actor
                || !this._validOverlayGeometry(width, height))
                return false;

            entry.actor.set_position(x, y);
            entry.actor.set_size(width, height);
            return true;
        } catch (e) {
            env.logError(e, 'Keysharp: MoveImageOverlay failed');
            return false;
        }
    }

    _hideImageOverlay(id, ownerKey, _busName) {
        const owner = this._parseOverlayOwner(ownerKey);
        this._removeImageOverlayByKey(this._overlayKey(id, owner.key));

        if (!this._hasOverlaysForOwner(owner.key))
            this._cancelOverlayReconnectTimer(owner.key);

        if (!this._hasAnyOverlays())
            this._stopOverlayCleanupTimer();

        return true;
    }

    SupportsTooltip() {
        return env.supportsTooltip;
    }

    // Draw or update a click-through tooltip label. slot identifies it per owner; empty text clears it.
    // Click-through and owner-tagged like the other overlays (see ShowHighlight for the mechanism).
    _showTooltip(slot, ownerKey, busName, text, x, y) {
        if (!env.supportsTooltip)
            return false;
        try {
            const owner = this._parseOverlayOwner(ownerKey);
            const key = this._overlayKey(slot, owner.key);
            const bus = String(busName || '');
            const labelText = String(text || '');

            if (labelText.length > MAX_TOOLTIP_CHARS
                || (labelText && !this._overlayQuotaAvailable(owner.key,
                                                    this._tooltips.has(key))))
                return false;

            this._cancelOverlayReconnectTimer(owner.key);
            this._removeTooltipByKey(key);

            if (!labelText) {
                if (!this._hasAnyOverlays())
                    this._stopOverlayCleanupTimer();
                return true;
            }

            const actor = new St.Label({
                reactive: false,
                text: labelText,
                style: 'background-color: #ffffe1; color: #000000; border: 1px solid #000000; padding: 6px; font-size: 10pt; font-family: sans-serif;',
            });
            actor.set_position(x, y);
            env.addClickThroughChrome(actor);

            this._tooltips.set(key, {
                actor: actor,
                ownerKey: owner.key,
                ownerPid: owner.pid,
                ownerStartTime: owner.startTime,
                busName: bus,
            });
            this._ensureOverlayCleanupTimer();
            return true;
        } catch (e) {
            env.logError(e, 'Keysharp: ShowTooltip failed');
            return false;
        }
    }

    _hideTooltip(slot, ownerKey, _busName) {
        if (!env.supportsTooltip)
            return false;
        const owner = this._parseOverlayOwner(ownerKey);
        this._removeTooltipByKey(this._overlayKey(slot, owner.key));

        if (!this._hasOverlaysForOwner(owner.key))
            this._cancelOverlayReconnectTimer(owner.key);

        if (!this._hasAnyOverlays())
            this._stopOverlayCleanupTimer();

        return true;
    }

    // ----------------------------------------------------------------
    // Private helpers
    // ----------------------------------------------------------------

    _removeHighlightByKey(key) {
        const entry = this._highlights.get(key);
        if (!entry)
            return;

        for (const a of (entry.edges || [])) {
            try { Main.layoutManager.removeChrome(a); } catch (_e) {}
            try { a.destroy(); } catch (_e) {}
        }

        this._highlights.delete(key);
    }

    _removeImageOverlayByKey(key) {
        const entry = this._imageOverlays.get(key);
        if (!entry)
            return;

        try { Main.layoutManager.removeChrome(entry.actor); } catch (_e) {}
        try { entry.actor.destroy(); } catch (_e) {}
        this._imageOverlays.delete(key);
    }

    _removeTooltipByKey(key) {
        const entry = this._tooltips.get(key);
        if (!entry)
            return;

        try { Main.layoutManager.removeChrome(entry.actor); } catch (_e) {}
        try { entry.actor.destroy(); } catch (_e) {}
        this._tooltips.delete(key);
    }

    // ---- overlay ownership: keying + owner parsing -------------------------------

    // Namespaces an overlay id under its owner so two clients using the same id never collide.
    _overlayKey(id, ownerKey) {
        return `${ownerKey}:${id}`;
    }

    _validOverlayGeometry(width, height) {
        return Number.isInteger(width) && Number.isInteger(height)
            && width > 0 && height > 0
            && width <= MAX_OVERLAY_DIMENSION
            && height <= MAX_OVERLAY_DIMENSION
            && width * height <= MAX_OVERLAY_PIXELS;
    }

    _validCaptureGeometry(width, height) {
        return Number.isInteger(width) && Number.isInteger(height)
            && width > 0 && height > 0
            && width <= MAX_CAPTURE_DIMENSION
            && height <= MAX_CAPTURE_DIMENSION
            && width * height <= MAX_CAPTURE_PIXELS;
    }

    _overlayQuotaAvailable(ownerKey, existing) {
        if (existing)
            return true;
        const total = this._highlights.size + this._imageOverlays.size
            + this._tooltips.size;
        if (total >= MAX_OVERLAYS_TOTAL)
            return false;
        let ownerCount = 0;
        for (const entries of [this._highlights, this._imageOverlays,
                               this._tooltips])
            for (const entry of entries.values())
                if (entry.ownerKey === ownerKey)
                    ownerCount++;
        return ownerCount < MAX_OVERLAYS_PER_OWNER;
    }

    _overlayBytesAvailable(ownerKey, decodedBytes, existing) {
        let total = 0;
        let ownerTotal = 0;
        for (const entry of this._imageOverlays.values()) {
            const bytes = entry.decodedBytes || 0;
            total += bytes;
            if (entry.ownerKey === ownerKey)
                ownerTotal += bytes;
        }
        const previous = existing ? existing.decodedBytes || 0 : 0;
        return total - previous + decodedBytes <= MAX_OVERLAY_BYTES_TOTAL
            && ownerTotal - previous + decodedBytes
                <= MAX_OVERLAY_BYTES_PER_OWNER;
    }

    // ownerKey is "<pid>:<starttime>" (see WaylandOverlayOwner on the C# side).
    _parseOverlayOwner(ownerKey) {
        const key = String(ownerKey || '');
        const parts = key.split(':');
        const pid = parts.length > 0 ? Number(parts[0]) : 0;
        const startTime = parts.length > 1 ? parts[1] : '';
        return {
            key: key,
            pid: Number.isFinite(pid) ? Math.round(pid) : 0,
            startTime: startTime,
        };
    }

    // True unless the owning process is provably gone. Reads field 22 (starttime) of /proc/<pid>/stat
    // and requires it to match the value captured when the overlay was created, so a reused pid (a
    // different process now holding <pid>) is treated as dead. Fails open on any read/parse error.
    _overlayOwnerAlive(pid, startTime) {
        if (!pid || pid <= 0)
            return true;

        const statPath = `/proc/${pid}/stat`;

        if (!GLib.file_test(statPath, GLib.FileTest.EXISTS))
            return false;

        if (!startTime)
            return true;

        try {
            const [ok, bytes] = GLib.file_get_contents(statPath);
            if (!ok)
                return true;

            const stat = env.decodeBytes(bytes);
            const end = stat.lastIndexOf(')');

            if (end < 0 || end + 2 >= stat.length)
                return true;

            const fields = stat.substring(end + 2).trim().split(/\s+/);
            return fields.length > 19 && fields[19] === startTime;
        } catch (_e) {
            return true;
        }
    }

    // ---- overlay ownership: reconnect grace timers -------------------------------

    // Called when a client's D-Bus name vanishes. If the owning process is already dead, reap now;
    // otherwise wait OVERLAY_RECONNECT_GRACE_MS and reap only if it stayed dead or never came back under
    // a new bus name (a reconnecting client re-registers, replacing busName -> _ownerHasReplacementBus).
    _handleOverlayConnectionLost(ownerKey, lostBusName, sampleEntry) {
        if (!this._overlayOwnerAlive(sampleEntry.ownerPid, sampleEntry.ownerStartTime)) {
            this._removeOwnerOverlays(ownerKey);
            return;
        }

        this._cancelOverlayReconnectTimer(ownerKey);
        const timerId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, OVERLAY_RECONNECT_GRACE_MS, () => {
            this._overlayReconnectTimers.delete(ownerKey);

            const entry = this._firstOverlayForOwner(ownerKey);

            if (!entry)
                return GLib.SOURCE_REMOVE;

            if (!this._overlayOwnerAlive(entry.ownerPid, entry.ownerStartTime)
                || !this._ownerHasReplacementBus(ownerKey, lostBusName))
                this._removeOwnerOverlays(ownerKey);

            return GLib.SOURCE_REMOVE;
        });
        this._overlayReconnectTimers.set(ownerKey, timerId);
    }

    _cancelOverlayReconnectTimer(ownerKey) {
        const timerId = this._overlayReconnectTimers.get(ownerKey);

        if (!timerId)
            return;

        try { GLib.source_remove(timerId); } catch (_e) {}
        this._overlayReconnectTimers.delete(ownerKey);
    }

    _cancelAllOverlayReconnectTimers() {
        for (const timerId of this._overlayReconnectTimers.values()) {
            try { GLib.source_remove(timerId); } catch (_e) {}
        }

        this._overlayReconnectTimers.clear();
    }

    _ownerHasReplacementBus(ownerKey, lostBusName) {
        for (const entry of this._highlights.values()) {
            if (entry.ownerKey === ownerKey && entry.busName && entry.busName !== lostBusName)
                return true;
        }

        for (const entry of this._imageOverlays.values()) {
            if (entry.ownerKey === ownerKey && entry.busName && entry.busName !== lostBusName)
                return true;
        }

        for (const entry of this._tooltips.values()) {
            if (entry.ownerKey === ownerKey && entry.busName && entry.busName !== lostBusName)
                return true;
        }

        return false;
    }

    // ---- overlay ownership: periodic /proc liveness sweep ------------------------

    // Safety net for a client that dies without its D-Bus name-drop reaching us: a 2s sweep reaps any
    // overlay whose owning process is gone. Self-terminating once no overlays remain.
    _ensureOverlayCleanupTimer() {
        if (this._overlayCleanupId !== 0)
            return;

        this._overlayCleanupId = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, 2, () => {
            this._cleanupDeadOverlays();

            if (!this._hasAnyOverlays()) {
                this._overlayCleanupId = 0;
                return GLib.SOURCE_REMOVE;
            }

            return GLib.SOURCE_CONTINUE;
        });
    }

    _stopOverlayCleanupTimer() {
        if (this._overlayCleanupId === 0)
            return;

        try { GLib.source_remove(this._overlayCleanupId); } catch (_e) {}
        this._overlayCleanupId = 0;
    }

    _cleanupDeadOverlays() {
        const owners = new Map();

        for (const entry of this._highlights.values()) {
            if (!this._overlayOwnerAlive(entry.ownerPid, entry.ownerStartTime))
                owners.set(entry.ownerKey, true);
        }

        for (const entry of this._imageOverlays.values()) {
            if (!this._overlayOwnerAlive(entry.ownerPid, entry.ownerStartTime))
                owners.set(entry.ownerKey, true);
        }

        for (const entry of this._tooltips.values()) {
            if (!this._overlayOwnerAlive(entry.ownerPid, entry.ownerStartTime))
                owners.set(entry.ownerKey, true);
        }

        for (const ownerKey of owners.keys())
            this._removeOwnerOverlays(ownerKey);
    }

    // ---- overlay ownership: NameOwnerChanged watch -------------------------------

    _handleNameOwnerChanged(parameters) {
        let name, oldOwner, newOwner;

        try {
            [name, oldOwner, newOwner] = parameters.deep_unpack();
        } catch (_e) {
            return;
        }

        // Only react to a name being released (had an owner, now has none).
        if (!name || !oldOwner || newOwner)
            return;

        const owners = new Map();

        for (const entry of this._highlights.values()) {
            if (entry.busName === name && !owners.has(entry.ownerKey))
                owners.set(entry.ownerKey, entry);
        }

        for (const entry of this._imageOverlays.values()) {
            if (entry.busName === name && !owners.has(entry.ownerKey))
                owners.set(entry.ownerKey, entry);
        }

        for (const entry of this._tooltips.values()) {
            if (entry.busName === name && !owners.has(entry.ownerKey))
                owners.set(entry.ownerKey, entry);
        }

        for (const [ownerKey, entry] of owners.entries())
            this._handleOverlayConnectionLost(ownerKey, name, entry);
    }

    // ---- overlay ownership: per-owner queries + removal --------------------------

    _firstOverlayForOwner(ownerKey) {
        for (const entry of this._highlights.values())
            if (entry.ownerKey === ownerKey)
                return entry;

        for (const entry of this._imageOverlays.values())
            if (entry.ownerKey === ownerKey)
                return entry;

        for (const entry of this._tooltips.values())
            if (entry.ownerKey === ownerKey)
                return entry;

        return null;
    }

    _hasOverlaysForOwner(ownerKey) {
        return this._firstOverlayForOwner(ownerKey) !== null;
    }

    _hasAnyOverlays() {
        return this._highlights.size !== 0 || this._imageOverlays.size !== 0 || this._tooltips.size !== 0;
    }

    _removeOwnerOverlays(ownerKey) {
        for (const [key, entry] of [...this._highlights.entries()]) {
            if (entry.ownerKey === ownerKey)
                this._removeHighlightByKey(key);
        }

        for (const [key, entry] of [...this._imageOverlays.entries()]) {
            if (entry.ownerKey === ownerKey)
                this._removeImageOverlayByKey(key);
        }

        for (const [key, entry] of [...this._tooltips.entries()]) {
            if (entry.ownerKey === ownerKey)
                this._removeTooltipByKey(key);
        }

        this._cancelOverlayReconnectTimer(ownerKey);

        if (!this._hasAnyOverlays())
            this._stopOverlayCleanupTimer();
    }

    _decodeImageFrame(pngData) {
        const data = pngData instanceof Uint8Array
            ? pngData : new Uint8Array(pngData || []);
        if (data.length < 33 || data.length > MAX_OVERLAY_PNG_BYTES)
            throw new Error('Invalid PNG overlay image.');
        const signature = [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a];
        if (!signature.every((value, index) => data[index] === value)
            || String.fromCharCode.apply(null, data.slice(12, 16)) !== 'IHDR')
            throw new Error('Invalid PNG overlay image.');
        const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
        const width = view.getUint32(16, false);
        const height = view.getUint32(20, false);
        if (view.getUint32(8, false) !== 13
            || !this._validOverlayGeometry(width, height))
            throw new Error('Invalid PNG overlay dimensions.');
        const loader = GdkPixbuf.PixbufLoader.new();
        loader.write(data);
        loader.close();
        const pixbuf = loader.get_pixbuf();

        if (!pixbuf || pixbuf.get_width() !== width
            || pixbuf.get_height() !== height
            || pixbuf.get_rowstride() * height > MAX_OVERLAY_PIXELS * 4)
            throw new Error('Could not decode PNG overlay image.');

        return {
            pixbuf, // keeps the borrowed pixel array alive until the texture copy completes
            pixels: pixbuf.get_pixels(),
            format: pixbuf.get_has_alpha() ? Cogl.PixelFormat.RGBA_8888 : Cogl.PixelFormat.RGB_888,
            width: pixbuf.get_width(),
            height: pixbuf.get_height(),
            rowStride: pixbuf.get_rowstride(),
        };
    }

    _updateImageActor(actor, frame, x, y, width, height) {
        const previous = actor.get_content();
        const sameSize = actor._keysharpImageWidth === frame.width
            && actor._keysharpImageHeight === frame.height;
        const content = env.makeImageContent(previous, frame, sameSize);
        if (!content)
            throw new Error('Could not create image overlay content.');
        if (content !== previous)
            actor.set_content(content);
        actor._keysharpImageWidth = frame.width;
        actor._keysharpImageHeight = frame.height;
        actor.set_position(x, y);
        actor.set_size(width, height);
    }
    _emitWindowEvent(type, win) {
        if (!win) return;
        let json;
        try {
            json = JSON.stringify(this._windowInfo(win));
        } catch (_e) {
            try { json = JSON.stringify({id: String(win.get_stable_sequence())}); }
            catch (_e2) { return; }
        }
        this._emitWindowEventRaw(type, json);
    }

    _emitWindowEventRaw(type, json) {
        if (this._providerConnections.size === 0) return;
        try {
            this._emitProviderSignal('WindowEvent',
                new GLib.Variant('(ss)', [type, json]));
        } catch (_e) {
            // best-effort; never throw inside a signal handle
        }
    }

    _emitProviderSignal(name, parameters) {
        for (const entry of this._providerConnections.values()) {
            try { entry.implementation.emit_signal(name, parameters); } catch (_e) {}
        }
    }

    // Connect per-window signals once. Handler ids are stashed on the window so they
    // can be disconnected in _unhookWindow / on 'unmanaged'.
    _hookWindow(win) {
        if (!win || win._keysharpHooked)
            return;
        win._keysharpHooked = true;

        // Read the pid now, while the window is certainly alive; see _clientPid.
        this._clientPid(win, true);

        const ids = [];
        ids.push(win.connect('notify::title', () => this._emitWindowEvent('title', win)));
        ids.push(win.connect('notify::minimized',
            () => this._emitWindowEvent(win.minimized ? 'minimize' : 'restore', win)));
        ids.push(win.connect('position-changed', () => this._emitWindowEvent('move', win)));
        ids.push(win.connect('size-changed',     () => this._emitWindowEvent('move', win)));
        ids.push(win.connect('unmanaged', () => {
            try {
                this._emitWindowEventRaw('close',
                    JSON.stringify({id: String(win.get_stable_sequence())}));
            } catch (_e) {}
            this._unhookWindow(win);
        }));
        win._keysharpHandlerIds = ids;
    }

    _unhookWindow(win) {
        if (!win || !win._keysharpHooked)
            return;
        for (const id of (win._keysharpHandlerIds || [])) {
            try { win.disconnect(id); } catch (_e) {}
        }
        win._keysharpHandlerIds = null;
        win._keysharpHooked = false;
    }

    // An unmanaged window lingers in global.get_window_actors() for as long as the shell's close animation
    // runs. It is not a window any more -- reporting it as one is wrong, and asking it about its Wayland
    // client is fatal: Muffin's meta_window_wayland_get_client_pid() hands the already-freed wl_resource
    // straight to wl_resource_get_client(), which dereferences it without a NULL check and takes the whole
    // shell down with it. Everything that walks the actor list therefore goes through here first.
    _isLiveWindow(win) {
        try {
            const actor = win ? win.get_compositor_private() : null;

            if (!actor)
                return false;

            return typeof actor.is_destroyed !== 'function' || !actor.is_destroyed();
        } catch (_e) {
            return false;
        }
    }

    _liveMetaWindow(actor) {
        const win = actor ? actor.get_meta_window() : null;
        return (win && this._isLiveWindow(win)) ? win : null;
    }

    // The window's client pid, read once while the window is unquestionably alive and cached on it after
    // that, so a later query -- which may well arrive mid-teardown -- never has to ask the compositor again.
    // A window that is already gone has no pid to give: -1, the same answer as the platforms that cannot say.
    //
    // `alive` is for the callers that hold that guarantee themselves -- window-created, where the window has
    // no compositor actor yet and _isLiveWindow cannot tell "not composited" from "destroyed" -- and it is
    // exactly those callers that seed the cache for everyone else.
    _clientPid(win, alive) {
        if (typeof win.__ksClientPid === 'number')
            return win.__ksClientPid;

        if (!alive && !this._isLiveWindow(win))
            return -1;

        let pid = -1;

        try {
            // Muffin answers the client pid only via get_client_pid() (get_pid() is -1 for Wayland
            // clients); modern Mutter removed get_client_pid() and made get_pid() answer it instead.
            pid = win.get_pid() > 0 ? win.get_pid() : (win.get_client_pid ? win.get_client_pid() : -1);
        } catch (_e) {
            pid = -1;
        }

        if (pid > 0)
            win.__ksClientPid = pid;

        return pid;
    }

    _isTrackedWindow(win) {
        switch (win.window_type) {
        case Meta.WindowType.NORMAL:
        case Meta.WindowType.DIALOG:
        case Meta.WindowType.MODAL_DIALOG:
        case Meta.WindowType.UTILITY:
            return true;
        default:
            return false;
        }
    }

    _windowInfo(win) {
        const frame = win.get_frame_rect();
        let buffer = null;
        try {
            const value = win.get_buffer_rect();
            if (value)
                buffer = {x: value.x, y: value.y,
                    width: value.width, height: value.height};
        } catch (_e) {
        }

        let alwaysOnTop = false;
        try {
            alwaysOnTop = typeof win.is_above === 'function'
                ? Boolean(win.is_above()) : Boolean(win.above);
        } catch (_e) {
        }

        const extras = env.windowExtras(win);
        const snapshot = {
            id: String(win.get_stable_sequence()),
            title: win.get_title() || '',
            appId: win.get_wm_class() || win.get_wm_class_instance() || '',
            pid: this._clientPid(win),
            frame: {x: frame.x, y: frame.y, width: frame.width, height: frame.height},
            client: {x: frame.x, y: frame.y, width: frame.width, height: frame.height},
            buffer: buffer,
            active: Boolean(win.appears_focused),
            minimized: Boolean(win.minimized),
            maximized: Boolean(win.maximized_horizontally && win.maximized_vertically),
            visible: !win.minimized,
            alwaysOnTop: alwaysOnTop,
            decorated: typeof win.decorated === 'boolean' ? win.decorated : true,
            transparency: this._windowOpacity(win),
        };
        Object.assign(snapshot, extras.values);
        snapshot.validFields = ['id', 'title', 'appId', 'frame', 'client', 'active',
            'minimized', 'maximized', 'visible', 'alwaysOnTop', 'transparency'];
        if (snapshot.pid > 0)
            snapshot.validFields.push('pid');
        if (buffer !== null)
            snapshot.validFields.push('buffer');
        if (typeof win.decorated === 'boolean')
            snapshot.validFields.push('decorated');
        for (const field of extras.validFields)
            snapshot.validFields.push(field);
        return snapshot;
    }
    _windowOpacity(win) {
        try {
            const actor = (typeof win.get_compositor_private === 'function')
                ? win.get_compositor_private()
                : null;
            if (!actor)
                return 255;
            const value = (typeof actor.get_opacity === 'function') ? actor.get_opacity() : actor.opacity;
            return Math.max(0, Math.min(255, Math.round(Number(value))));
        } catch (_e) {
            return 255;
        }
    }

    _findWindow(handle) {
        const seq = Number(handle);
        for (const actor of global.get_window_actors()) {
            const win = this._liveMetaWindow(actor);
            if (win && win.get_stable_sequence() === seq)
                return win;
        }
        return null;
    }

    _findWindowByXid(xid) {
        const target = Number(xid);
        for (const actor of global.get_window_actors()) {
            const win = this._liveMetaWindow(actor);
            if (win && typeof win.get_xwindow === 'function' && Number(win.get_xwindow()) === target)
                return win;
        }
        return null;
    }
}


export default class KeysharpExtension extends KeysharpExtensionCore {}
