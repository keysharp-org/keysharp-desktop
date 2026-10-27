const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = process.argv[2] || path.resolve(__dirname, '..');

const core = fs.readFileSync(path.join(root, 'providers/shared/provider-core.js'), 'utf8');
const callbacks = new Map();
const ioErrors = {NOT_SUPPORTED: 15};
let nextSource = 1;
const Shell = new Function('env', core + '\nreturn KeysharpExtensionCore;')({
    Gio: {IOErrorEnum: ioErrors},
    GLib: {
        PRIORITY_DEFAULT: 0,
        SOURCE_REMOVE: false,
        SOURCE_CONTINUE: true,
        timeout_add(_priority, _interval, callback) {
            const id = nextSource++;
            callbacks.set(id, callback);
            return id;
        },
        source_remove(id) { callbacks.delete(id); },
    },
    windowExtras: () => ({values: {}, validFields: []}),
    setDecorated: () => null,
});
const kwin = vm.createContext({});
vm.runInContext(fs.readFileSync(path.join(root, 'providers/kwin/contents/code/main.js'), 'utf8')
    .replace(/\nstart\(\);\s*$/, '\n'), kwin);

for (const backend of ['shell', 'kwin']) {
    const calls = [];
    const window = {
        minimized: false, maximized: true, fullScreen: true,
        get maximized_horizontally() { return this.maximized; },
        get maximized_vertically() { return this.maximized; },
        minimize() { calls.push('minimize'); this.minimized = true; },
        unminimize() { calls.push('unminimize'); this.minimized = false; },
        maximize() { calls.push('maximize'); this.maximized = true; },
        unmaximize() { this.maximized = false; },
        setMaximize(value) { this.maximized = value; }
    };
    const shell = Object.create(Shell.prototype);
    shell._findWindow = () => window;
    kwin.findWindow = () => window;
    const set = state => backend === 'shell'
        ? shell._setWindowState('1', state)
        : kwin.executeJob({opcode: 0x2027, body: '1 ' + state, sequence: '1'}).status === 0;
    for (const maximized of [false, true]) {
        window.maximized = maximized;
        assert.equal(set(1), true);
        assert.equal(window.minimized, true);
        assert.equal(set(3), true);
        assert.equal(window.minimized, false);
        assert.equal(window.maximized, maximized);
        assert.equal(window.fullScreen, true);
    }
    if (backend === 'shell') {
        calls.length = 0;
        assert.equal(set(2), true);
        assert.equal(set(2), true);
        assert.deepEqual(calls, []);
        assert.equal(set(1), true);
        assert.equal(set(1), true);
        assert.deepEqual(calls, ['minimize']);
        assert.equal(set(3), true);
        assert.equal(set(3), true);
        assert.deepEqual(calls, ['minimize', 'unminimize']);
    }
    assert.equal(set(0), true);
    assert.equal(window.maximized, false);
    assert.equal(set(4), false);
}

const provider = Object.create(Shell.prototype);
provider._providerConnections = new Map([['authority', {}]]);
provider._mapRefreshSources = new Map();
provider._isLiveWindow = () => true;
provider._clientPid = () => 123;
provider._windowOpacity = () => 255;
const events = [];
provider._emitWindowEventRaw = (type, json) => events.push({type, window: JSON.parse(json)});
let frame = {x: 0, y: 0, width: 0, height: 0};
const mapped = {
    _keysharpHooked: true,
    get_frame_rect: () => frame,
    get_buffer_rect: () => null,
    get_stable_sequence: () => 42,
    get_title: () => 'Mapped window',
    get_wm_class: () => 'test',
    get_compositor_private: () => ({visible: true}),
    is_above: () => false,
    appears_focused: false,
    minimized: false,
    maximized_horizontally: false,
    maximized_vertically: false,
};
assert.equal(provider._windowInfo(mapped).validFields.includes('frame'), false);
provider._refreshMappedWindow(mapped);
const refresh = provider._mapRefreshSources.get(mapped);
assert.equal(callbacks.get(refresh)(), true);
assert.equal(events.length, 0);
frame = {x: 25, y: 35, width: 200, height: 100};
assert.equal(callbacks.get(refresh)(), false);
assert.equal(provider._mapRefreshSources.has(mapped), false);
assert.equal(events.length, 1);
assert.equal(events[0].type, 'changed');
assert.equal(events[0].window.frame.width, 200);
assert.equal(events[0].window.validFields.includes('frame'), true);

provider._refreshMappedWindow(mapped);
const cancelled = provider._mapRefreshSources.get(mapped);
mapped._keysharpHandlerIds = [];
provider._unhookWindow(mapped);
assert.equal(provider._mapRefreshSources.has(mapped), false);
assert.equal(callbacks.has(cancelled), false);

assert.equal(provider._emitWindowEvent('move', {
    get_frame_rect: () => { throw new Error('Window is not ready'); },
    get_stable_sequence: () => 42,
}), false);
assert.equal(events.length, 1);

function signalObject(values = {}) {
    const signals = new Map();
    return Object.assign(values, {
        signals,
        connect(name, callback) { signals.set(name, callback); return name; },
        disconnect(name) { signals.delete(name); },
        emit(name) { signals.get(name)?.(); },
    });
}

const actor = signalObject({visible: true, opacity: 255});
const tracked = signalObject(Object.assign({}, mapped, {
    _keysharpHooked: false,
    above: false,
    decorated: true,
    get_compositor_private: () => actor,
    is_above() { return this.above; },
}));
provider._windowOpacity = Shell.prototype._windowOpacity;
provider._hookWindow(tracked);
tracked.above = true;
tracked.emit('notify::above');
assert.equal(events.at(-1).window.alwaysOnTop, true);
tracked.decorated = false;
tracked.emit('notify::decorated');
assert.equal(events.at(-1).window.decorated, false);
actor.opacity = 100;
actor.emit('notify::opacity');
assert.equal(events.at(-1).window.transparency, 100);
tracked.maximized_horizontally = true;
tracked.maximized_vertically = true;
tracked.emit('notify::maximized-horizontally');
assert.equal(events.at(-1).window.maximized, true);
provider._unhookWindow(tracked);
assert.equal(actor.signals.size, 0);
assert.equal(tracked.signals.size, 0);

const booleanProvider = Object.create(Shell.prototype);
booleanProvider._requireProviderPeer = () => true;
booleanProvider._findWindow = () => tracked;
const responses = [];
booleanProvider.SetWindowDecoratedAsync(['42', false], {
    return_value() { assert.fail('Unsupported decorations must return an error'); },
    return_error_literal(domain, code) { responses.push({domain, code}); },
});
assert.deepEqual(responses, [{domain: ioErrors, code: ioErrors.NOT_SUPPORTED}]);

const gnomeShim = fs.readFileSync(path.join(root, 'providers/shared/gnome-shim.js'), 'utf8');
const spawned = [];
const setGnomeDecorated = new Function('GnomeMeta', 'GnomeGLib',
    gnomeShim.slice(gnomeShim.indexOf('function setDecorated('),
        gnomeShim.indexOf('\nfunction addClickThroughChrome(')) + '\nreturn setDecorated;')(
    {WindowClientType: {X11: 0}}, {spawn_command_line_async: command => spawned.push(command)});
assert.equal(setGnomeDecorated({get_client_type: () => 1, get_xwindow: () => 123}, false), null);
assert.equal(setGnomeDecorated({get_client_type: () => 0, get_xwindow: () => 0}, false), null);
assert.equal(spawned.length, 0);
assert.equal(setGnomeDecorated({get_client_type: () => 0, get_xwindow: () => 123}, false), true);
assert.equal(spawned.length, 1);
assert.match(spawned[0], /xprop -id 123 .*_MOTIF_WM_HINTS/);

provider._providerConnections.clear();
const timerCount = callbacks.size;
provider._refreshMappedWindow(mapped);
assert.equal(callbacks.size, timerCount);
assert.equal(provider._emitWindowEvent('move', {
    get_frame_rect: () => { throw new Error('Unobserved windows must not be serialized'); },
}), true);

for (const backend of ['shell', 'kwin']) {
    let geometry = {x: 25, y: 35, width: 200, height: 100};
    const calls = [];
    const window = {
        get_monitor: () => 0,
        get_frame_rect: () => geometry,
        get frameGeometry() { return geometry; },
        set frameGeometry(value) { calls.push('geometry'); geometry = {...value}; },
        move_resize_frame(_user, x, y, width, height) {
            calls.push('geometry'); geometry = {x, y, width, height};
        },
        unmaximize() {
            calls.push('restore');
            this.maximized_horizontally = this.maximized_vertically = false;
            geometry = {x: 50, y: 60, width: 160, height: 80};
        },
        setMaximize() { this.unmaximize(); this.maximized = false; },
    };
    const shell = Object.create(Shell.prototype);
    kwin.findWindow = () => window;
    const move = (x, y, width, height) => backend === 'shell'
        ? shell._moveResizeWin(window, x, y, width, height)
        : kwin.executeJob({opcode: 0x2025, body: [1, x, y, width, height].join(' '), sequence: '1'}).status === 0;
    assert.equal(move(-2147483648, -2147483648, 0, 0), true);
    assert.deepEqual(calls, []);
    assert.equal(move(-2147483648, -2147483648, 220, 120), true);
    assert.deepEqual(geometry, {x: 25, y: 35, width: 220, height: 120});
    assert.deepEqual(calls, ['geometry']);
    calls.length = 0;
    window.maximized = window.maximized_horizontally = window.maximized_vertically = true;
    assert.equal(move(75, -2147483648, 0, 0), true);
    assert.deepEqual(geometry, {x: 75, y: 60, width: 160, height: 80});
    assert.deepEqual(calls, ['restore', 'geometry']);
}
console.log('Shell and KWin window-state tests passed.');
