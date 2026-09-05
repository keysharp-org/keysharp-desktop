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

@PROVIDER_CORE@

export default class KeysharpExtension extends KeysharpExtensionCore {}
