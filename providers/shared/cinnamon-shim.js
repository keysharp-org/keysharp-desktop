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

const LAUNCHER_ENTRY_IFACE = 'com.canonical.Unity.LauncherEntry';
const GROUPED_WINDOW_LIST_UUID = 'grouped-window-list@cinnamon.org';
const LAUNCHER_ATTENTION_CLASS = 'grouped-window-list-item-demands-attention';

class CinnamonLauncherEntryBridge {
    constructor() {
        this._entries = new Map();
        this._serial = 0;
        this._updateSignalId = 0;
        this._nameWatchId = 0;
        this._windowCreatedId = 0;
        this._workspaceChangedId = 0;
        this._applySourceId = 0;
    }

    enable() {
        try {
            this._updateSignalId = CinnamonGio.DBus.session.signal_subscribe(
                null,
                LAUNCHER_ENTRY_IFACE,
                'Update',
                null,
                null,
                CinnamonGio.DBusSignalFlags.NONE,
                (_connection, sender, _path, _iface, _signal, parameters) =>
                    this._onUpdate(sender, parameters));
            this._nameWatchId = CinnamonGio.DBus.session.signal_subscribe(
                'org.freedesktop.DBus',
                'org.freedesktop.DBus',
                'NameOwnerChanged',
                '/org/freedesktop/DBus',
                null,
                CinnamonGio.DBusSignalFlags.NONE,
                (_connection, _sender, _path, _iface, _signal, parameters) =>
                    this._onNameOwnerChanged(parameters));
            this._windowCreatedId = global.display.connect('window-created', () =>
                this._scheduleApply(50));
            this._workspaceChangedId = global.workspace_manager.connect(
                'active-workspace-changed', () => this._scheduleApply());
        } catch (e) {
            global.logError(e, 'Keysharp: could not start LauncherEntry integration');
            this.disable();
        }
    }

    disable() {
        if (this._applySourceId !== 0) {
            try { CinnamonGLib.source_remove(this._applySourceId); } catch (_e) {}
            this._applySourceId = 0;
        }
        if (this._updateSignalId !== 0) {
            try { CinnamonGio.DBus.session.signal_unsubscribe(this._updateSignalId); } catch (_e) {}
            this._updateSignalId = 0;
        }
        if (this._nameWatchId !== 0) {
            try { CinnamonGio.DBus.session.signal_unsubscribe(this._nameWatchId); } catch (_e) {}
            this._nameWatchId = 0;
        }
        if (this._windowCreatedId !== 0) {
            try { global.display.disconnect(this._windowCreatedId); } catch (_e) {}
            this._windowCreatedId = 0;
        }
        if (this._workspaceChangedId !== 0) {
            try { global.workspace_manager.disconnect(this._workspaceChangedId); } catch (_e) {}
            this._workspaceChangedId = 0;
        }

        this._entries.clear();
        this._apply();
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
            global.logError(e, 'Keysharp: invalid LauncherEntry update');
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
        this._applySourceId = CinnamonGLib.timeout_add(
            CinnamonGLib.PRIORITY_DEFAULT, delay, () => {
                this._applySourceId = 0;
                this._apply();
                return CinnamonGLib.SOURCE_REMOVE;
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

    _appGroups() {
        const groups = [];
        try {
            const applets = CinnamonMain.AppletManager.getRunningInstancesForUuid(
                GROUPED_WINDOW_LIST_UUID) || [];
            for (const applet of applets) {
                for (const workspace of (applet.workspaces || [])) {
                    for (const group of (workspace.appGroups || [])) {
                        if (group && group.groupState && !group.groupState.willUnmount)
                            groups.push(group);
                    }
                }
            }
        } catch (e) {
            global.logError(e, 'Keysharp: could not find Cinnamon taskbar buttons');
        }
        return groups;
    }

    _entryForGroup(group, current) {
        const ids = [group.groupState.appId];
        for (const win of (group.groupState.metaWindows || [])) {
            try { ids.push(win.get_gtk_application_id()); } catch (_e) {}
            try { ids.push(win.get_wm_class()); } catch (_e) {}
            try { ids.push(win.get_wm_class_instance()); } catch (_e) {}
        }
        for (const value of ids) {
            const entry = current.get(this._normalizeDesktopId(value));
            if (entry)
                return entry;
        }
        return null;
    }

    _apply() {
        const current = this._currentEntries();
        for (const group of this._appGroups()) {
            const entry = this._entryForGroup(group, current);
            if (entry)
                this._applyToGroup(group, entry);
            else
                this._clearGroup(group);
        }
    }

    _applyToGroup(group, entry) {
        group.__launcherEntryDesktopId = entry.desktopId;

        if (entry.countVisible && Number.isFinite(entry.count)) {
            group.notificationsBadgeLabel.text = String(Math.trunc(entry.count));
            group.notificationsBadge.show();
            group.__launcherEntryBadge = true;
        } else if (group.__launcherEntryBadge) {
            group.__launcherEntryBadge = false;
            if (typeof group.updateNotificationsBadge === 'function')
                group.updateNotificationsBadge();
            else
                group.notificationsBadge.hide();
        }

        if (entry.progressVisible && Number.isFinite(entry.progress)) {
            group.progress = Math.max(0, Math.min(100, entry.progress * 100));
            if (group.progress > 0)
                group.progressOverlay.show();
            else
                group.progressOverlay.hide();
            group.actor.queue_relayout();
            group.__launcherEntryProgress = true;
        } else if (group.__launcherEntryProgress) {
            this._restoreProgress(group);
        }

        if (entry.urgent && !group.__launcherEntryUrgent) {
            group.__launcherEntryUrgentHadClass = group.actor.has_style_class_name(
                LAUNCHER_ATTENTION_CLASS);
            group.actor.add_style_class_name(LAUNCHER_ATTENTION_CLASS);
            group.__launcherEntryUrgent = true;
        } else if (!entry.urgent && group.__launcherEntryUrgent) {
            this._restoreUrgency(group);
        }
    }

    _restoreProgress(group) {
        group.__launcherEntryProgress = false;
        let total = 0;
        let count = 0;
        for (const win of (group.groupState.metaWindows || [])) {
            const progress = Number(win.progress);
            if (Number.isFinite(progress) && progress >= 1) {
                total += progress;
                count++;
            }
        }
        group.progress = count === 0 ? 0 : total / count;
        if (group.progress > 0)
            group.progressOverlay.show();
        else
            group.progressOverlay.hide();
        group.actor.queue_relayout();
    }

    _restoreUrgency(group) {
        if (!group.__launcherEntryUrgentHadClass)
            group.actor.remove_style_class_name(LAUNCHER_ATTENTION_CLASS);
        group.__launcherEntryUrgent = false;
        group.__launcherEntryUrgentHadClass = false;
    }

    _clearGroup(group) {
        if (!group.__launcherEntryDesktopId)
            return;
        if (group.__launcherEntryBadge) {
            group.__launcherEntryBadge = false;
            if (typeof group.updateNotificationsBadge === 'function')
                group.updateNotificationsBadge();
            else
                group.notificationsBadge.hide();
        }
        if (group.__launcherEntryProgress)
            this._restoreProgress(group);
        if (group.__launcherEntryUrgent)
            this._restoreUrgency(group);
        group.__launcherEntryDesktopId = null;
    }
}

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
let launcherEntryBridge = null;

function init(_metadata) {
}

function enable() {
    extension = new KeysharpExtension();
    extension.enable();
    launcherEntryBridge = new CinnamonLauncherEntryBridge();
    launcherEntryBridge.enable();
}

function disable() {
    if (launcherEntryBridge !== null) {
        launcherEntryBridge.disable();
        launcherEntryBridge = null;
    }
    if (extension !== null) {
        extension.disable();
        extension = null;
    }
}
