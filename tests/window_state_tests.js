const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = process.argv[2] || path.resolve(__dirname, '..');

const core = fs.readFileSync(path.join(root, 'providers/shared/provider-core.js'), 'utf8');
const Shell = new Function('env', core + '\nreturn KeysharpExtensionCore;')({});
const kwin = vm.createContext({});
vm.runInContext(fs.readFileSync(path.join(root, 'providers/kwin/contents/code/main.js'), 'utf8')
    .replace(/\nstart\(\);\s*$/, '\n'), kwin);

for (const backend of ['shell', 'kwin']) {
    const window = {
        minimized: false, maximized: true, fullScreen: true,
        minimize() { this.minimized = true; },
        unminimize() { this.minimized = false; },
        maximize() { this.maximized = true; },
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
        for (let repeat = 0; repeat < 2; repeat++) {
            assert.equal(set(3), true);
            assert.equal(window.minimized, false);
            assert.equal(window.maximized, maximized);
            assert.equal(window.fullScreen, true);
        }
    }
    assert.equal(set(0), true);
    assert.equal(window.maximized, false);
    assert.equal(set(4), false);
}

const callbacks = new Map();
const removed = [];
let nextSource = 1;
const EventShell = new Function('env', core + '\nreturn KeysharpExtensionCore;')({
    GLib: {
        PRIORITY_DEFAULT: 0,
        SOURCE_REMOVE: false,
        SOURCE_CONTINUE: true,
        timeout_add(_priority, _interval, callback) {
            const id = nextSource++;
            callbacks.set(id, callback);
            return id;
        },
        source_remove(id) { removed.push(id); callbacks.delete(id); },
    },
    windowExtras: () => ({values: {}, validFields: []}),
});
const provider = Object.create(EventShell.prototype);
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
assert.equal(removed.includes(cancelled), true);

assert.equal(provider._emitWindowEvent('move', {
    get_frame_rect: () => { throw new Error('Window is not ready'); },
    get_stable_sequence: () => 42,
}), false);
assert.equal(events.length, 1);
console.log('Shell and KWin window-state tests passed.');
