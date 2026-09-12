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
console.log('Shell and KWin window-state tests passed.');
