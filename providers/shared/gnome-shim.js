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
const LAUNCHER_ENTRY_IFACE = 'com.canonical.Unity.LauncherEntry';

class GnomeLauncherEntryBridge {
    constructor() {
        this._entries = new Map();
        this._decorations = new Map();
        this._serial = 0;
        this._updateSignalId = 0;
        this._nameWatchId = 0;
        this._appStateId = 0;
        this._overviewShowingId = 0;
        this._windowCreatedId = 0;
        this._dashBoxSignalId = 0;
        this._applySourceId = 0;
    }

    enable() {
        try {
            this._updateSignalId = GnomeGio.DBus.session.signal_subscribe(
                null,
                LAUNCHER_ENTRY_IFACE,
                'Update',
                null,
                null,
                GnomeGio.DBusSignalFlags.NONE,
                (_connection, sender, _path, _iface, _signal, parameters) =>
                    this._onUpdate(sender, parameters));
            this._nameWatchId = GnomeGio.DBus.session.signal_subscribe(
                'org.freedesktop.DBus',
                'org.freedesktop.DBus',
                'NameOwnerChanged',
                '/org/freedesktop/DBus',
                null,
                GnomeGio.DBusSignalFlags.NONE,
                (_connection, _sender, _path, _iface, _signal, parameters) =>
                    this._onNameOwnerChanged(parameters));
            this._appStateId = GnomeShell.AppSystem.get_default().connect(
                'app-state-changed', () => this._scheduleApply(50));
            this._overviewShowingId = GnomeMain.overview.connect(
                'showing', () => this._scheduleApply());
            this._windowCreatedId = global.display.connect(
                'window-created', () => this._scheduleApply(50));

            const box = GnomeMain.overview?.dash?._box;
            if (box) {
                const signal = SHELL_MAJOR >= 46 ? 'child-added' : 'actor-added';
                this._dashBoxSignalId = box.connect(signal, () => this._scheduleApply());
            }
            this._scheduleApply();
        } catch (e) {
            logError(e, 'Keysharp: could not start LauncherEntry integration');
            this.disable();
        }
    }

    disable() {
        if (this._applySourceId !== 0) {
            try { GnomeGLib.source_remove(this._applySourceId); } catch (_e) {}
            this._applySourceId = 0;
        }
        if (this._updateSignalId !== 0) {
            try { GnomeGio.DBus.session.signal_unsubscribe(this._updateSignalId); } catch (_e) {}
            this._updateSignalId = 0;
        }
        if (this._nameWatchId !== 0) {
            try { GnomeGio.DBus.session.signal_unsubscribe(this._nameWatchId); } catch (_e) {}
            this._nameWatchId = 0;
        }
        if (this._appStateId !== 0) {
            try { GnomeShell.AppSystem.get_default().disconnect(this._appStateId); } catch (_e) {}
            this._appStateId = 0;
        }
        if (this._overviewShowingId !== 0) {
            try { GnomeMain.overview.disconnect(this._overviewShowingId); } catch (_e) {}
            this._overviewShowingId = 0;
        }
        if (this._windowCreatedId !== 0) {
            try { global.display.disconnect(this._windowCreatedId); } catch (_e) {}
            this._windowCreatedId = 0;
        }
        if (this._dashBoxSignalId !== 0) {
            try { GnomeMain.overview?.dash?._box?.disconnect(this._dashBoxSignalId); } catch (_e) {}
            this._dashBoxSignalId = 0;
        }

        this._entries.clear();
        for (const icon of [...this._decorations.keys()])
            this._removeDecoration(icon);
    }

    _onUpdate(sender, parameters) {
        try {
            const [uri, rawProperties] = parameters.deep_unpack();
            const desktopId = this._normalizeDesktopId(uri);
            if (!sender || !desktopId || rawProperties === null
                || typeof rawProperties !== 'object')
                return;

            const key = `${sender}\u0000${desktopId}`;
            const previous = this._entries.get(key) || {
                sender,
                desktopId,
                count: 0,
                countVisible: false,
                progress: 0,
                progressVisible: false,
                urgent: false,
            };
            const value = name => this._variantValue(rawProperties[name]);

            if (Object.prototype.hasOwnProperty.call(rawProperties, 'count'))
                previous.count = Number(value('count'));
            if (Object.prototype.hasOwnProperty.call(rawProperties, 'count-visible'))
                previous.countVisible = Boolean(value('count-visible'));
            if (Object.prototype.hasOwnProperty.call(rawProperties, 'progress'))
                previous.progress = Number(value('progress'));
            if (Object.prototype.hasOwnProperty.call(rawProperties, 'progress-visible'))
                previous.progressVisible = Boolean(value('progress-visible'));
            if (Object.prototype.hasOwnProperty.call(rawProperties, 'urgent'))
                previous.urgent = Boolean(value('urgent'));

            previous.serial = ++this._serial;
            this._entries.set(key, previous);
            this._scheduleApply();
        } catch (e) {
            logError(e, 'Keysharp: invalid LauncherEntry update');
        }
    }

    _onNameOwnerChanged(parameters) {
        let name, oldOwner, newOwner;
        try {
            [name, oldOwner, newOwner] = parameters.deep_unpack();
        } catch (_e) {
            return;
        }
        if (!name || !oldOwner || newOwner)
            return;

        let removed = false;
        for (const [key, entry] of this._entries.entries()) {
            if (entry.sender === name) {
                this._entries.delete(key);
                removed = true;
            }
        }
        if (removed)
            this._scheduleApply();
    }

    _variantValue(value) {
        let result = value;
        for (let depth = 0; depth < 2
                && result && typeof result.deep_unpack === 'function'; depth++)
            result = result.deep_unpack();
        return result;
    }

    _normalizeDesktopId(value) {
        let id = String(value || '').trim();
        if (id.toLowerCase().startsWith('application://'))
            id = id.substring('application://'.length);
        try { id = decodeURIComponent(id); } catch (_e) {}
        if (id.toLowerCase().endsWith('.desktop'))
            id = id.substring(0, id.length - '.desktop'.length);
        return id.toLowerCase();
    }

    _scheduleApply(delay = 0) {
        if (this._applySourceId !== 0)
            return;
        this._applySourceId = GnomeGLib.timeout_add(
            GnomeGLib.PRIORITY_DEFAULT, delay, () => {
                this._applySourceId = 0;
                this._apply();
                return GnomeGLib.SOURCE_REMOVE;
            });
    }

    _currentEntries() {
        const current = new Map();
        for (const entry of this._entries.values()) {
            const prior = current.get(entry.desktopId);
            if (!prior || prior.serial < entry.serial)
                current.set(entry.desktopId, entry);
        }
        return current;
    }

    _dashIcons() {
        const icons = [];
        const box = GnomeMain.overview?.dash?._box;
        if (!box)
            return icons;

        for (const item of box.get_children()) {
            const child = item.child || (typeof item.get_child === 'function'
                ? item.get_child() : null);
            const icon = child?._delegate || child;
            if (icon?.app && icon._iconContainer)
                icons.push(icon);
        }
        return icons;
    }

    _entryForIcon(icon, current) {
        const ids = [];
        try { ids.push(icon.app.get_id()); } catch (_e) {}
        try {
            for (const win of (icon.app.get_windows() || [])) {
                try { ids.push(win.get_gtk_application_id()); } catch (_e) {}
                try { ids.push(win.get_wm_class()); } catch (_e) {}
                try { ids.push(win.get_wm_class_instance()); } catch (_e) {}
            }
        } catch (_e) {}

        for (const value of ids) {
            const entry = current.get(this._normalizeDesktopId(value));
            if (entry)
                return entry;
        }
        return null;
    }

    _apply() {
        const current = this._currentEntries();
        const live = new Set();
        for (const icon of this._dashIcons()) {
            live.add(icon);
            const entry = this._entryForIcon(icon, current);
            if (entry)
                this._applyToIcon(icon, entry);
            else
                this._removeDecoration(icon);
        }

        for (const icon of [...this._decorations.keys()]) {
            if (!live.has(icon))
                this._removeDecoration(icon);
        }
    }

    _applyToIcon(icon, entry) {
        const decoration = this._ensureDecoration(icon);
        if (!decoration)
            return;

        decoration.badge.text = String(Math.trunc(entry.count));
        if (entry.countVisible && Number.isFinite(entry.count))
            decoration.badge.show();
        else
            decoration.badge.hide();

        decoration.progress.__launcherEntryValue = Number.isFinite(entry.progress)
            ? Math.max(0, Math.min(1, entry.progress)) : 0;
        decoration.progress.queue_repaint();
        if (entry.progressVisible)
            decoration.progress.show();
        else
            decoration.progress.hide();

        if (entry.urgent)
            decoration.urgent.show();
        else
            decoration.urgent.hide();
        icon.__launcherEntryDesktopId = entry.desktopId;
    }

    _ensureDecoration(icon) {
        const existing = this._decorations.get(icon);
        if (existing)
            return existing;

        const container = icon._iconContainer;
        if (!container || typeof container.add_child !== 'function')
            return null;

        const progress = new GnomeSt.DrawingArea({
            reactive: false,
            x_align: GnomeClutter.ActorAlign.FILL,
            y_align: GnomeClutter.ActorAlign.FILL,
            x_expand: true,
            y_expand: true,
        });
        progress.__launcherEntryValue = 0;
        progress.connect('repaint', actor => this._drawProgress(actor));
        const urgent = new GnomeSt.Widget({
            reactive: false,
            x_align: GnomeClutter.ActorAlign.FILL,
            y_align: GnomeClutter.ActorAlign.FILL,
            x_expand: true,
            y_expand: true,
            style: 'border: 2px solid rgba(237, 51, 59, 0.95); border-radius: 9px;',
        });
        const badge = new GnomeSt.Label({
            reactive: false,
            x_align: GnomeClutter.ActorAlign.END,
            y_align: GnomeClutter.ActorAlign.START,
            style: 'background-color: rgb(224, 27, 36); color: white; '
                + 'font-weight: bold; font-size: 10pt; border-radius: 9px; padding: 1px 4px;',
        });
        container.add_child(progress);
        container.add_child(urgent);
        container.add_child(badge);
        progress.hide();
        urgent.hide();
        badge.hide();

        const decoration = {badge, progress, urgent, destroyId: 0};
        decoration.destroyId = icon.connect('destroy', () => this._decorations.delete(icon));
        this._decorations.set(icon, decoration);
        return decoration;
    }

    _drawProgress(actor) {
        let cr = null;
        try {
            cr = actor.get_context();
            const [width, height] = actor.get_surface_size();
            const barHeight = Math.max(3, Math.min(6, Math.round(height / 12)));
            const y = height - barHeight;
            cr.setSourceRGBA(0.08, 0.08, 0.08, 0.72);
            cr.rectangle(0, y, width, barHeight);
            cr.fill();
            cr.setSourceRGBA(0.21, 0.52, 0.89, 1);
            cr.rectangle(0, y, width * actor.__launcherEntryValue, barHeight);
            cr.fill();
        } catch (_e) {
        } finally {
            if (cr)
                cr.$dispose();
        }
    }

    _removeDecoration(icon) {
        const decoration = this._decorations.get(icon);
        if (!decoration)
            return;
        this._decorations.delete(icon);
        try { icon.disconnect(decoration.destroyId); } catch (_e) {}
        try { decoration.badge.destroy(); } catch (_e) {}
        try { decoration.progress.destroy(); } catch (_e) {}
        try { decoration.urgent.destroy(); } catch (_e) {}
        icon.__launcherEntryDesktopId = null;
    }
}

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

export default class KeysharpExtension extends KeysharpExtensionCore {
    enable() {
        super.enable();
        this._launcherEntryBridge = new GnomeLauncherEntryBridge();
        this._launcherEntryBridge.enable();
    }

    disable() {
        if (this._launcherEntryBridge) {
            this._launcherEntryBridge.disable();
            this._launcherEntryBridge = null;
        }
        super.disable();
    }
}
