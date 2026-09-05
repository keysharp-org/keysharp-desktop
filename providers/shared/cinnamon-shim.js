// Cinnamon loader and Muffin compositor adapter.
const CinnamonClutter = imports.gi.Clutter;
const CinnamonCogl = imports.gi.Cogl;
const CinnamonGio = imports.gi.Gio;
const CinnamonGdkPixbuf = imports.gi.GdkPixbuf;
const CinnamonGLib = imports.gi.GLib;
const CinnamonMeta = imports.gi.Meta;
const CinnamonSt = imports.gi.St;
const CinnamonCairoGI = imports.gi.cairo;
const CinnamonMain = imports.ui.main;
const CinnamonByteArray = imports.byteArray;

function workArea() {
    try {
        const workspace = global.workspace_manager.get_active_workspace();
        const index = CinnamonMain.layoutManager.primaryIndex;
        const area = workspace.get_work_area_for_monitor(index);
        return [Math.round(area.x), Math.round(area.y),
            Math.round(area.width), Math.round(area.height)];
    } catch (_e) {
        const monitor = CinnamonMain.layoutManager.primaryMonitor;
        return monitor ? [Math.round(monitor.x), Math.round(monitor.y),
            Math.round(monitor.width), Math.round(monitor.height)] : [0, 0, 0, 0];
    }
}

function addClickThroughChrome(actor) {
    CinnamonMain.layoutManager.addChrome(actor, {
        visibleInFullscreen: true,
        affectsStruts: false,
        affectsInputRegion: false,
    });
}

function makeImageContent(content, frame, sameSize) {
    if (content && sameSize && typeof content.set_area === 'function') {
        try {
            const area = new CinnamonCairoGI.RectangleInt();
            area.x = 0;
            area.y = 0;
            area.width = frame.width;
            area.height = frame.height;
            if (content.set_area(frame.pixels, frame.format, area, frame.rowStride))
                return content;
        } catch (_e) {
        }
    }

    const image = new CinnamonClutter.Image();
    image.set_data(frame.pixels, frame.format, frame.width, frame.height,
        frame.rowStride);
    return image;
}

let gdk = null;
let gdkChecked = false;

function gdkPixbufBridge() {
    if (gdkChecked)
        return gdk;
    gdkChecked = true;

    try {
        const wanted = imports.gi.versions.Gdk;
        if (wanted !== undefined && wanted !== '3.0')
            return gdk;
        imports.gi.versions.Gdk = '3.0';
        const candidate = imports.gi.Gdk;
        if (typeof candidate.pixbuf_get_from_surface === 'function')
            gdk = candidate;
    } catch (e) {
        global.logError(e, 'Keysharp: window capture needs Gdk 3');
    }
    return gdk;
}

function captureWindowPixbuf(provider, handle, includeDecoration) {
    const bridge = gdkPixbufBridge();
    const win = provider._findWindow(handle);
    const actor = win ? win.get_compositor_private() : null;
    if (bridge === null || !actor || typeof actor.get_image !== 'function')
        return null;

    let captureScale = 1;
    if (typeof actor.get_resource_scale === 'function') {
        const raw = actor.get_resource_scale();
        const unpacked = Array.isArray(raw) ? raw : [true, raw];
        if (unpacked[0] && Number.isFinite(unpacked[1]) && unpacked[1] >= 1)
            captureScale = Math.ceil(unpacked[1]);
    }

    const buffer = win.get_buffer_rect();
    const wanted = includeDecoration ? buffer : win.get_frame_rect();
    const clip = new CinnamonCairoGI.RectangleInt();
    clip.x = Math.round(wanted.x - buffer.x);
    clip.y = Math.round(wanted.y - buffer.y);
    clip.width = Math.round(wanted.width);
    clip.height = Math.round(wanted.height);
    if (clip.x < 0 || clip.y < 0
        || !provider._validCaptureGeometry(clip.width * captureScale,
            clip.height * captureScale)
        || clip.x + clip.width > Math.round(buffer.width)
        || clip.y + clip.height > Math.round(buffer.height))
        return null;

    const surface = actor.get_image(clip);
    if (!surface || typeof surface.getWidth !== 'function'
        || typeof surface.getHeight !== 'function')
        return null;

    const width = surface.getWidth();
    const height = surface.getHeight();
    if (!provider._validCaptureGeometry(width, height))
        return null;
    const pixbuf = bridge.pixbuf_get_from_surface(surface, 0, 0, width, height);
    return pixbuf && pixbuf.get_width() === width && pixbuf.get_height() === height
        ? pixbuf : null;
}

function encodePixbuf(pixbuf) {
    const saved = pixbuf.save_to_bufferv('png', [], []);
    const data = Array.isArray(saved) ? saved[saved.length - 1] : saved;
    return data instanceof Uint8Array
        ? data : (data && typeof data.length === 'number' ? new Uint8Array(data) : null);
}

function captureWindow(provider, handle, includeDecoration, reply) {
    let pixbuf = null;
    try {
        pixbuf = captureWindowPixbuf(provider, handle, includeDecoration);
    } catch (e) {
        global.logError(e, 'Keysharp: CaptureWindow failed');
    }
    if (pixbuf === null) {
        reply(null);
        return;
    }
    if (typeof pixbuf.save_to_streamv_async !== 'function') {
        reply(encodePixbuf(pixbuf));
        return;
    }

    const stream = CinnamonGio.MemoryOutputStream.new_resizable();
    let watchdog = CinnamonGLib.timeout_add(CinnamonGLib.PRIORITY_DEFAULT, 15000, () => {
        watchdog = 0;
        reply(null);
        return CinnamonGLib.SOURCE_REMOVE;
    });
    pixbuf.save_to_streamv_async(stream, 'png', [], [], null, (_source, result) => {
        if (watchdog !== 0) {
            CinnamonGLib.source_remove(watchdog);
            watchdog = 0;
        }
        try {
            if (!CinnamonGdkPixbuf.Pixbuf.save_to_stream_finish(result)) {
                reply(null);
                return;
            }
            stream.close(null);
            reply(stream.steal_as_bytes().get_data());
        } catch (e) {
            global.logError(e, 'Keysharp: CaptureWindow encode failed');
            reply(null);
        }
    });
}

function windowExtras(win) {
    let workspace = -1;
    let monitor = -1;
    let onCurrentWorkspace = true;
    let workspaceKnown = false;
    try {
        const current = win.get_workspace();
        if (current)
            workspace = current.index();
        const active = global.workspace_manager
            ? global.workspace_manager.get_active_workspace()
            : global.screen.get_active_workspace();
        onCurrentWorkspace = win.is_on_all_workspaces()
            || (current !== null && current === active);
        workspaceKnown = true;
    } catch (_e) {
    }
    try {
        if (typeof win.get_monitor === 'function')
            monitor = win.get_monitor();
    } catch (_e) {
    }

    const validFields = [];
    if (workspaceKnown)
        validFields.push('onCurrentWorkspace');
    if (workspace >= 0)
        validFields.push('workspace');
    if (monitor >= 0)
        validFields.push('monitor');
    return {
        values: {
            workspace: workspace,
            monitor: monitor,
            onCurrentWorkspace: onCurrentWorkspace,
        },
        validFields: validFields,
    };
}

const env = {
    Clutter: CinnamonClutter,
    Cogl: CinnamonCogl,
    GdkPixbuf: CinnamonGdkPixbuf,
    Gio: CinnamonGio,
    GLib: CinnamonGLib,
    Main: CinnamonMain,
    Meta: CinnamonMeta,
    St: CinnamonSt,
    serviceName: 'io.github.keysharp.CinnamonShell',
    objectPath: '/io/github/keysharp/CinnamonShell',
    providerSocketName: 'provider-cinnamon.sock',
    logError: global.logError,
    decodeBytes: bytes => CinnamonByteArray.toString(bytes),
    addClickThroughChrome: addClickThroughChrome,
    makeImageContent: makeImageContent,
    captureArea: null,
    captureWindow: captureWindow,
    setDecorated: (win, decorated) => {
        win.decorated = decorated;
        return true;
    },
    workArea: workArea,
    maximizeFlags: CinnamonMeta.MaximizeFlags.BOTH,
    windowExtras: windowExtras,
    supportsTooltip: false,
};

@PROVIDER_CORE@

const KeysharpExtension = KeysharpExtensionCore;
let extension = null;

function init(_metadata) {
}

function enable() {
    extension = new KeysharpExtension();
    extension.enable();
}

function disable() {
    if (extension !== null) {
        extension.disable();
        extension = null;
    }
}
