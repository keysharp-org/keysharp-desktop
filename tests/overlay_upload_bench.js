// Run manually with cjs (Cinnamon) or gjs (GNOME 48+); no shell extension is loaded.
// Cinnamon: GI_TYPELIB_PATH=/usr/lib/x86_64-linux-gnu/muffin \
// LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu/muffin cjs tests/overlay_upload_bench.js
// GNOME: set GI_TYPELIB_PATH/LD_LIBRARY_PATH for Mutter and St, then run with
// --backend=gnome --version=18 (GNOME 50). Keep this outside the live shell.
// On small machines, use systemd-run --user --scope -p MemoryMax=512M
// -p MemorySwapMax=0 to bound the benchmark process's native allocations.
// Older toArray() implementations expose a view. An explicit copy below models
// the first copy in GJS 1.88; the upload's second GI marshaling copy is real.

const options = {backend: 'cinnamon', version: '18', frames: 12, rounds: 3};
for (const argument of ARGV) {
    const match = /^--(backend|version|frames|rounds)=(.+)$/.exec(argument);
    if (!match)
        throw new Error(`Unknown argument: ${argument}`);
    options[match[1]] = match[1] === 'frames' || match[1] === 'rounds'
        ? Number(match[2]) : match[2];
}
if (!['gnome', 'cinnamon'].includes(options.backend)
    || !Number.isInteger(options.frames) || options.frames < 1
    || !Number.isInteger(options.rounds) || options.rounds < 1)
    throw new Error('Invalid benchmark options');

imports.gi.versions.Cogl = options.backend === 'cinnamon' ? '0' : options.version;
imports.gi.versions.Clutter = options.backend === 'cinnamon' ? '0' : options.version;
if (options.backend === 'gnome')
    imports.gi.versions.St = options.version;
const {Cogl, Clutter, GLib, Gio} = imports.gi;
const System = imports.system;
const St = options.backend === 'gnome' ? imports.gi.St : null;
const context = St ? Cogl.Context.new(null) : null;
const format = Cogl.PixelFormat.BGRA_8888_PRE;
const probe = new GLib.Bytes(new Uint8Array([1]));
probe.toArray()[0] = 2;
const nativeArrayCopies = probe.toArray()[0] === 1;

function makeImage(width, height) {
    return St ? new St.ImageContent({preferredWidth: width, preferredHeight: height})
        : new Clutter.Image();
}

function setBytes(image, bytes, width, height, stride) {
    const ok = St ? image.set_bytes(context, bytes, format, width, height, stride)
        : image.set_bytes(bytes, format, width, height, stride);
    if (!ok)
        throw new Error('set_bytes failed');
}

function offscreen(image) {
    const framebuffer = Cogl.Offscreen.new_with_texture(image.get_texture());
    if (!framebuffer.allocate())
        throw new Error('Offscreen allocation failed');
    return framebuffer;
}

function blit(source, target, x, y, width, height) {
    const ok = typeof source.blit === 'function'
        ? source.blit(target, 0, 0, x, y, width, height)
        : Cogl.blit_framebuffer(source, target, 0, 0, x, y, width, height);
    if (!ok)
        throw new Error('Framebuffer blit failed');
}

function areaBytes(bytes, stride, area) {
    const offset = area.y * stride + area.x * 4;
    const length = (area.height - 1) * stride + area.width * 4;
    return typeof bytes.new_from_bytes === 'function'
        ? bytes.new_from_bytes(offset, length) : GLib.Bytes.new_from_bytes(bytes, offset, length);
}

function arrayPixels(bytes) {
    const pixels = bytes.toArray();
    return nativeArrayCopies ? pixels : new Uint8Array(pixels);
}

function median(values) {
    const sorted = [...values].sort((a, b) => a - b);
    return sorted[Math.floor(sorted.length / 2)];
}

function collect() {
    // GObject finalizers can defer native unrefs to the main context. A tight
    // loop otherwise retains textures even after the JS wrappers are collected.
    const main = GLib.MainContext.default();
    for (let pass = 0; pass < 2; pass++) {
        System.gc();
        for (let idle = 0; idle < 32 && main.pending(); idle++)
            main.iteration(false);
    }
}

function runCase(width, height, area, bytes) {
    const stride = width * 4;
    const modes = area.width === width && area.height === height
        ? ['array-region', 'set-bytes'] : ['array-region', 'bytes-blit'];
    const results = new Map();
    for (const mode of modes)
        results.set(mode, {submit: [], complete: [], housekeeping: []});
    const span = (area.height - 1) * stride + area.width * 4;
    const batchFrames = Math.max(1, Math.min(4, Math.floor(16 * 1024 * 1024 / (span * 2))));

    for (let round = 0; round < options.rounds; round++) {
        // Rotate execution order so warm caches do not consistently favor one path.
        for (let index = 0; index < modes.length; index++) {
            const mode = modes[(index + round) % modes.length];
            const result = results.get(mode);
            if (result.unsupported)
                continue;
            let image = makeImage(width, height);
            setBytes(image, bytes, width, height, stride);
            let target = offscreen(image);
            const update = () => {
                if (mode === 'set-bytes') {
                    setBytes(image, bytes, width, height, stride);
                } else if (mode === 'array-region') {
                    const pixels = arrayPixels(areaBytes(bytes, stride, area));
                    if (St) {
                        if (!image.get_texture().set_region(0, 0, area.x, area.y,
                            area.width, area.height, area.width, area.height,
                            format, stride, pixels))
                            throw new Error('set_region failed');
                        image.invalidate();
                    } else {
                        const rectangle = new imports.gi.cairo.RectangleInt();
                        Object.assign(rectangle, area);
                        if (!image.set_area(pixels, format, rectangle, stride))
                            throw new Error('set_area failed');
                    }
                } else {
                    const patch = makeImage(area.width, area.height);
                    setBytes(patch, areaBytes(bytes, stride, area),
                        area.width, area.height, stride);
                    const source = offscreen(patch);
                    blit(source, target, area.x, area.y, area.width, area.height);
                    image.invalidate();
                }
            };

            try {
                for (let warmup = 0; warmup < 2; warmup++) {
                    update();
                    target.finish();
                    collect();
                }
                let submitUs = 0;
                let completeUs = 0;
                let housekeepingUs = 0;
                for (let frame = 0; frame < options.frames; frame += batchFrames) {
                    const count = Math.min(batchFrames, options.frames - frame);
                    const begin = GLib.get_monotonic_time();
                    for (let batch = 0; batch < count; batch++)
                        update();
                    const submitted = GLib.get_monotonic_time();
                    // finish() waits for the whole GL context, including uploads
                    // to replacement textures, without allocating another FBO.
                    target.finish();
                    const completed = GLib.get_monotonic_time();
                    collect();
                    submitUs += submitted - begin;
                    completeUs += completed - begin;
                    housekeepingUs += GLib.get_monotonic_time() - completed;
                }
                result.submit.push(submitUs / options.frames / 1000);
                result.complete.push(completeUs / options.frames / 1000);
                result.housekeeping.push(housekeepingUs / options.frames / 1000);
            } catch (error) {
                result.unsupported = String(error);
            }
            target = null;
            image = null;
            collect();
        }
    }
    for (const [mode, result] of results) {
        print(JSON.stringify({screen: `${width}x${height}`,
            damage: `${area.width}x${area.height}`, mode,
            batchFrames,
            rowSpanBytes: span,
            jsAndGiCopyBytes: mode === 'array-region' ? span * 2 : 0,
            uploadedBytes: area.width * area.height * 4,
            ...(result.unsupported ? {unsupported: result.unsupported} : {
                submitMs: median(result.submit), completeMs: median(result.complete),
                housekeepingMs: median(result.housekeeping),
                submitRoundsMs: result.submit, completeRoundsMs: result.complete,
            })}));
    }
}

print(JSON.stringify({benchmark: 'overlay-upload', backend: options.backend,
    coglVersion: imports.gi.versions.Cogl, frames: options.frames, rounds: options.rounds,
    runtime: System.version, glib: `${GLib.MAJOR_VERSION}.${GLib.MINOR_VERSION}.${GLib.MICRO_VERSION}`,
    nativeArrayCopies, emulatesGjs188ArrayCopy: !nativeArrayCopies,
    timing: 'bounded batches; GPU finish in completeMs; forced GC/main-context cleanup reported separately'}));

for (const [width, height] of [[1920, 1080], [3840, 2160]]) {
    const [file, stream] = Gio.File.new_tmp('keysharp-overlay-bench-XXXXXX');
    try {
        const pixels = new Uint8Array(width * height * 4).fill(128);
        stream.get_output_stream().write_all(pixels, null);
        stream.get_output_stream().flush(null);
        const mapped = GLib.MappedFile.new(file.get_path(), false);
        const bytes = mapped.get_bytes();
        for (const [damageWidth, damageHeight] of [
            [64, 64], [512, 512], [width / 2, height / 2], [64, height], [width, 64], [width, height],
        ]) {
            runCase(width, height, {
                x: Math.floor((width - damageWidth) / 2),
                y: Math.floor((height - damageHeight) / 2),
                width: damageWidth, height: damageHeight,
            }, bytes);
        }
    } finally {
        stream.close(null);
        file.delete(null);
        collect();
    }
}
