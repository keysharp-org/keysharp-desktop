const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname,
    '../providers/shared/cinnamon-shim.js'), 'utf8').split('@PROVIDER_CORE@')[0];
let captures = [];
let stageCalls = 0;
let resourceScale = 2;
const images = [];
const draws = [];
const sandbox = vm.createContext({
    Uint8Array,
    imports: {
        gi: {
            versions: {}, Clutter: {}, Cogl: {}, Gio: {}, GdkPixbuf: {}, GLib: {},
            Meta: {MaximizeFlags: {BOTH: 3}}, St: {},
            cairo: {RectangleInt: class {}},
            Gdk: {
                pixbuf_get_from_surface: (_surface, _x, _y, width, height) => ({width, height}),
            },
        },
        ui: {main: {}}, byteArray: {},
        cairo: {
            Format: {ARGB32: 0},
            ImageSurface: class {
                constructor(_format, width, height) {
                    this.width = width; this.height = height; this.finished = false;
                    images.push(this);
                }
                finish() { this.finished = true; }
            },
            Context: class {
                scale(x, y) { draws.push(['scale', x, y]); }
                save() {}
                restore() {}
                rectangle(x, y, width, height) { draws.push(['clip', x, y, width, height]); }
                clip() {}
                setSourceSurface(image, x, y) { draws.push(['source', image.id, x, y]); }
                paint() {}
                $dispose() { draws.push(['dispose']); }
            },
        },
    },
    global: {
        logError: () => {},
        stage: {
            get_resource_scale: () => [true, resourceScale],
            capture(paint, rect) {
                stageCalls++;
                assert.equal(paint, true);
                assert.equal(rect.width, 150);
                return [captures.length > 0, captures];
            },
        },
    },
});
vm.runInContext(source, sandbox);
const provider = {
    _validCaptureGeometry: (width, height) => width > 0 && height > 0
        && width <= 16384 && height <= 16384 && width * height <= 16777216,
};
const part = (id, x, width, scale) => ({
    rect: {x, y: 20, width, height: 40},
    image: {
        id, finished: false,
        getDeviceScale: () => [scale, scale],
        finish() { this.finished = true; },
    },
});
captures = [part(1, 10, 50, 1), part(2, 110, 50, 2)];
let result = sandbox.captureAreaPixbuf(provider, [10, 20, 150, 40]);
assert.equal(result.width, 300);
assert.equal(result.height, 80);
assert.deepEqual(draws.filter(x => x[0] === 'source'), [
    ['source', 1, 0, 0], ['source', 2, 100, 0],
]);
assert.deepEqual(draws[0], ['scale', 2, 2]);
assert.equal(captures.every(capture => capture.image.finished), true);
assert.equal(images[0].finished, true);
assert.equal(draws.some(draw => draw[0] === 'dispose'), true);

captures = [part(3, 10, 50, 256)];
result = sandbox.captureAreaPixbuf(provider, [10, 20, 150, 40]);
assert.equal(result, null);
assert.equal(captures[0].image.finished, true);
assert.equal(images.length, 1);

resourceScale = 256;
const beforeOversized = stageCalls;
assert.equal(sandbox.captureAreaPixbuf(provider, [10, 20, 150, 40]), null);
assert.equal(stageCalls, beforeOversized);
resourceScale = 2;

captures = [];
assert.equal(sandbox.captureAreaPixbuf(provider, [10, 20, 150, 40]), null);
const before = stageCalls;
assert.equal(sandbox.captureAreaPixbuf(provider, [10, 20, 0, 40]), null);
assert.equal(stageCalls, before);
