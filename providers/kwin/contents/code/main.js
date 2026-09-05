/*
 * keysharp-desktop KWin script.
 *
 * A KWin script cannot be called. Nothing outside the compositor can invoke it,
 * and its only outbound path is callDBus. So the direction is inverted from
 * every other provider here: the script asks the daemon for work, the daemon
 * parks that request until it has some, and the reply carries a batch of jobs.
 *
 * Everything in this file runs on the compositor's main thread. Concurrency
 * inside KWin is exactly one, permanently, and nothing here changes that. What
 * the two lanes buy is ORDER: a cheap query is never behind a queue of
 * enumerations, only ever behind at most one executing job.
 *
 * Written in conservative ES5. The engine is QJSEngine, which accepts more,
 * but this file is also parsed by the build's syntax gate as a plain script and
 * there is nothing here that needs newer syntax.
 */

"use strict";

var SERVICE = "io.github.keysharp.KWinProvider1";
var PATH = "/io/github/keysharp/KWinProvider";
var IFACE = "io.github.keysharp.KWinProvider1";

/* Mirrors src/protocol.h. A number that appears in both places is a number that
 * can drift, so each is named once here and nowhere else in this file. */
var OP_WINDOW_LIST = 0x2010;
var OP_WINDOW_HANDLES = 0x2013;
var OP_WINDOW_ACTIVE = 0x2011;
var OP_WINDOW_QUERY = 0x2014;
var OP_WINDOW_FOCUS = 0x2020;
var OP_WINDOW_RAISE = 0x2021;
var OP_WINDOW_LOWER = 0x2022;
var OP_WINDOW_CLOSE = 0x2023;
var OP_WINDOW_MOVE_RESIZE = 0x2025;
var OP_WINDOW_SET_STATE = 0x2027;
var OP_WINDOW_SET_OPACITY = 0x2028;
var OP_WINDOW_SET_ABOVE = 0x2029;
var OP_WINDOW_SET_DECORATED = 0x202a;
var OP_WINDOW_SET_SKIP_TASKBAR = 0x202d;
var OP_CURSOR_POSITION = 0x2050;
var OP_WORK_AREA = 0x2051;

/* Mirrors the status codes the wire uses. */
var STATUS_OK = 0;
var STATUS_UNSUPPORTED = 2;
var STATUS_INVALID_REQUEST = 3;
var STATUS_BUSY = 5;
var STATUS_NOT_FOUND = 6;
var STATUS_INTERNAL = 255;

var generation = "";
var running = false;

/*
 * KWin 6 renamed the window API. Both spellings are probed once at start rather
 * than at every call, and a spelling that is missing disables the verbs that
 * need it instead of throwing later: a script that throws inside a callback
 * takes the whole channel down, and the daemon then sees a wedge rather than an
 * unsupported operation.
 */
var api = {
    windowList: null,
    activeWindow: null,
    setActiveWindow: null,
    signalAdded: null,
    signalRemoved: null
};

function sortedWindowList(list) {
    var result = [];
    var index;

    for (index = 0; index < list.length; index++)
        result.push(list[index]);
    result.sort(function (left, right) {
        return Number(left.stackingOrder || 0)
            - Number(right.stackingOrder || 0);
    });
    return result;
}

function detectApi() {
    if (typeof workspace === "undefined" || workspace === null)
        return false;
    if (workspace.stackingOrder
            && workspace.stackingOrder.length !== undefined) {
        api.windowList = function () { return workspace.stackingOrder; };
    } else if (typeof workspace.windowList === "function") {
        api.windowList = function () { return workspace.windowList(); };
    } else if (typeof workspace.clientList === "function") {
        api.windowList = function () {
            return sortedWindowList(workspace.clientList());
        };
    } else {
        return false;
    }
    api.activeWindow = function () {
        if (typeof workspace.activeWindow !== "undefined")
            return workspace.activeWindow;
        return workspace.activeClient;
    };
    api.setActiveWindow = function (window) {
        if (typeof workspace.activeWindow !== "undefined")
            workspace.activeWindow = window;
        else
            workspace.activeClient = window;
    };
    api.signalAdded = workspace.windowAdded || workspace.clientAdded || null;
    api.signalRemoved = workspace.windowRemoved || workspace.clientRemoved
        || null;
    return true;
}

/* ------------------------------------------------------------- envelope -- */

function hex(value, digits) {
    var text = (value >>> 0).toString(16);

    while (text.length < digits)
        text = "0" + text;
    return text.slice(text.length - digits);
}

/*
 * The daemon's parser is strict on purpose, so what is written here has to obey
 * the same three rules it enforces: fixed-width lowercase hex, decimals with no
 * leading zeros, and declared body lengths that sum to exactly the remainder.
 * A value that cannot be spelled that way is a bug here, not there.
 */
function buildPoll(lane, roundTripMs, lost) {
    return "KSK1\n"
        + "gen " + generation + "\n"
        + "lane " + lane + "\n"
        + "rtt " + String(Math.max(0, Math.floor(roundTripMs))) + "\n"
        + "lost " + String(Math.max(0, Math.floor(lost))) + "\n"
        + "end\n";
}

function buildReport(results) {
    var header = "KSK1\ngen " + generation + "\n";
    var bodies = "";
    var index;

    for (index = 0; index < results.length; index++) {
        var body = results[index].body === undefined ? "" : results[index].body;
        /* The length is in BYTES, and a caption can hold anything a user typed,
         * so it is measured after encoding rather than by string length. */
        var encoded = utf8Length(body);

        header += "done " + results[index].sequence + " "
            + String(results[index].status) + " " + String(encoded) + "\n";
        bodies += body;
    }
    return header + "end\n" + bodies;
}

function utf8Length(text) {
    var length = 0;
    var index;

    for (index = 0; index < text.length; index++) {
        var code = text.charCodeAt(index);

        if (code < 0x80)
            length += 1;
        else if (code < 0x800)
            length += 2;
        else if (code >= 0xd800 && code <= 0xdbff) {
            /* A surrogate pair is one code point of four bytes, and the low
             * half must not be counted again on the next iteration. */
            length += 4;
            index++;
        } else {
            length += 3;
        }
    }
    return length;
}

/*
 * Parses a poll reply. Returns null on anything not fully understood, and the
 * caller treats that as a dead channel rather than guessing: a reply this
 * cannot read is one the daemon and the script disagree about, and continuing
 * would act on a job that may not be the one that was sent.
 */
function parsePollReply(text) {
    var terminator = text.indexOf("\nend\n");
    var lines;
    var jobs = [];
    var index;
    var offset;

    if (text.indexOf("KSK1\n") !== 0 || terminator < 0)
        return null;
    lines = text.slice(5, terminator).split("\n");
    if (lines.length < 1 || lines[0].indexOf("gen ") !== 0)
        return null;
    if (lines[0].slice(4) !== generation)
        return null;

    offset = terminator + 5;
    for (index = 1; index < lines.length; index++) {
        var line = lines[index];

        if (line.indexOf("lane ") === 0 || line.indexOf("idle ") === 0)
            continue;
        if (line.indexOf("job ") !== 0)
            return null;

        var parts = line.slice(4).split(" ");
        if (parts.length !== 4)
            return null;
        var length = parseInt(parts[3], 10);
        if (isNaN(length) || length < 0)
            return null;
        jobs.push({
            sequence: parts[0],
            opcode: parseInt(parts[1], 16),
            budget: parseInt(parts[2], 10),
            body: text.substr(offset, length)
        });
        offset += length;
    }
    return jobs;
}

/* ---------------------------------------------------------------- verbs -- */

function jsonString(value) {
    return JSON.stringify(value === undefined || value === null
        ? "" : String(value));
}

/* Stable public handles for KWin's native UUID identifiers. */
var handleByNativeId = {};
var nativeIdByHandle = {};
var fallbackWindows = [];
var fallbackHandles = [];
var nextFallbackHandle = 4294967295;

function fallbackWindowHandle(window) {
    var index;

    for (index = 0; index < fallbackWindows.length; index++) {
        if (fallbackWindows[index] === window)
            return fallbackHandles[index];
    }
    var handle = String(nextFallbackHandle);
    while (nativeIdByHandle[handle] !== undefined && nextFallbackHandle > 1) {
        nextFallbackHandle--;
        handle = String(nextFallbackHandle);
    }
    fallbackWindows.push(window);
    fallbackHandles.push(handle);
    nativeIdByHandle[handle] = "fallback";
    if (nextFallbackHandle > 1)
        nextFallbackHandle--;
    return handle;
}

function windowHandle(window) {
    if (window === null || window === undefined)
        return "";
    var nativeId = window.internalId === undefined
        || window.internalId === null ? "" : String(window.internalId);

    if (nativeId.length === 0)
        return fallbackWindowHandle(window);
    var existing = handleByNativeId[nativeId];

    if (existing !== undefined)
        return existing;

    /* The public control API takes a uint64. KWin's native identifier is a
     * UUID, so give it a stable numeric token for this script lifetime. Keep
     * old assignments reserved: reusing a closed window's token could make a
     * delayed request act on an unrelated window. */
    var hash = 5381;
    var index;

    for (index = 0; index < nativeId.length; index++) {
        hash = ((hash * 33) ^ nativeId.charCodeAt(index)) >>> 0;
    }
    if (hash === 0)
        hash = 1;
    var handle = String(hash);
    while (nativeIdByHandle[handle] !== undefined
            && nativeIdByHandle[handle] !== nativeId) {
        hash = (hash + 1) >>> 0;
        if (hash === 0)
            hash = 1;
        handle = String(hash);
    }
    handleByNativeId[nativeId] = handle;
    nativeIdByHandle[handle] = nativeId;
    return handle;
}

function findWindow(handle) {
    var windows = api.windowList();
    var index;

    for (index = 0; index < windows.length; index++) {
        if (windowHandle(windows[index]) === handle)
            return windows[index];
    }
    return null;
}

function isMaximized(window) {
    if (window.maximizeMode !== undefined) {
        var mode = Number(window.maximizeMode);

        if (isFinite(mode))
            return mode === 3;
    }
    if (window.maximized !== undefined)
        return window.maximized === true;
    if (typeof workspace.clientArea !== "function")
        return false;
    var area;

    try {
        area = workspace.clientArea(KWin.MaximizeArea, window);
    } catch (error) {
        return false;
    }
    if (area === null || area === undefined)
        return false;
    var geometry = window.frameGeometry || window.geometry;

    return geometry !== null && geometry !== undefined
        && Math.abs(Number(geometry.x) - Number(area.x)) <= 1
        && Math.abs(Number(geometry.y) - Number(area.y)) <= 1
        && Math.abs(Number(geometry.width) - Number(area.width)) <= 1
        && Math.abs(Number(geometry.height) - Number(area.height)) <= 1;
}

function clearMaximize(window) {
    if (typeof window.setMaximize !== "function")
        return;
    if (window.maximizeMode !== undefined) {
        if (Number(window.maximizeMode) !== 0)
            window.setMaximize(false, false);
        return;
    }
    if (window.maximized === true)
        window.setMaximize(false, false);
}

function raiseWindow(window) {
    if (typeof workspace.raiseWindow === "function") {
        workspace.raiseWindow(window);
        return true;
    }
    if (typeof workspace.slotWindowRaise !== "function")
        return false;

    api.setActiveWindow(window);
    workspace.slotWindowRaise();
    return true;
}

function windowJson(window) {
    var geometry = window.frameGeometry || window.geometry
        || { x: 0, y: 0, width: 0, height: 0 };

    var valid = ['id'];
    if (window.caption !== undefined) valid.push('title');
    if (window.internalId !== undefined) valid.push('captureId');
    if (window.resourceClass || window.desktopFileName || window.resourceName) valid.push('appId');
    if (window.pid > 0) valid.push('pid');
    // 'client' repeats the frame rect: the scripting API exposes no separate client area, and under the
    // client-side decorations Wayland clients use the two coincide.
    if (window.frameGeometry || window.geometry) { valid.push('frame'); valid.push('client'); }
    if (window.minimized !== undefined) valid.push('minimized');
    if (window.maximizeMode !== undefined || window.maximized !== undefined) valid.push('maximized');
    if (window.active !== undefined) valid.push('active');
    if (window.hidden !== undefined && window.minimized !== undefined) valid.push('visible');
    if (window.keepAbove !== undefined) valid.push('alwaysOnTop');
    if (window.noBorder !== undefined) valid.push('decorated');
    if (window.opacity !== undefined) valid.push('transparency');

    return "{\"validFields\":" + JSON.stringify(valid) + ",\"id\":" + jsonString(windowHandle(window))
        + ",\"captureId\":" + jsonString(window.internalId)
        + ",\"title\":" + jsonString(window.caption)
        + ",\"appId\":" + jsonString(window.resourceClass
            || window.desktopFileName || window.resourceName)
        + ",\"pid\":" + String(Math.max(0, Math.round(window.pid || 0)))
        + ",\"frame\":{\"x\":" + String(Math.round(geometry.x))
        + ",\"y\":" + String(Math.round(geometry.y))
        + ",\"width\":" + String(Math.round(geometry.width))
        + ",\"height\":" + String(Math.round(geometry.height))
        + "},\"client\":{\"x\":" + String(Math.round(geometry.x))
        + ",\"y\":" + String(Math.round(geometry.y))
        + ",\"width\":" + String(Math.round(geometry.width))
        + ",\"height\":" + String(Math.round(geometry.height))
        + "},\"minimized\":" + (window.minimized ? "true" : "false")
        + ",\"active\":" + (window.active ? "true" : "false")
        + ",\"maximized\":" + (isMaximized(window) ? "true" : "false")
        + ",\"visible\":"
        + (!(window.hidden || window.minimized) ? "true" : "false")
        + ",\"alwaysOnTop\":" + (window.keepAbove ? "true" : "false")
        + ",\"decorated\":" + (!window.noBorder ? "true" : "false")
        /* 0 to 255, the scale every backend of this service reports, not the
         * 0..1 KWin uses. Rounded rather than truncated so a fully opaque
         * window reports 255 and not 254. */
        + ",\"transparency\":"
        + String(Math.round((window.opacity === undefined ? 1
            : window.opacity) * 255))
        + "}";
}

/* A window this service will report or act on. Excludes the furniture: panels,
 * docks, menus and the desktop are not what a caller asking for windows means,
 * and closing one would be a surprise rather than an operation. */
function isOrdinary(window) {
    if (window === null || window === undefined)
        return false;
    if (window.deleted)
        return false;
    return !(window.specialWindow || window.dock || window.desktopWindow
        || window.splash || window.utility || window.dropdownMenu
        || window.popupMenu || window.toolbar || window.tooltip
        || window.notification || window.onScreenDisplay);
}

function executeJob(job) {
    var window;
    var body;
    var index;
    var windows;

    /* A job whose budget has already run out is refused rather than run. The
     * daemon has by then told the caller BUSY, and doing the work anyway is
     * the exact defect the budget exists to prevent. */
    if (job.budget <= 0)
        return { sequence: job.sequence, status: STATUS_BUSY, body: "" };

    switch (job.opcode) {
    case OP_WINDOW_LIST:
        var includeHidden = job.body === "1";
        windows = api.windowList();
        body = "{\"ok\":true,\"windows\":[";
        var first = true;
        for (index = 0; index < windows.length; index++) {
            if (!isOrdinary(windows[index]))
                continue;
            if (!includeHidden
                    && (windows[index].hidden || windows[index].minimized))
                continue;
            if (!first)
                body += ",";
            first = false;
            body += windowJson(windows[index]);
        }
        body += "]}";
        return { sequence: job.sequence, status: STATUS_OK, body: body };

    case OP_WINDOW_HANDLES:
        /* Identifiers and nothing else. This carries no grant, so it must not
         * carry a title, a class or a pid either -- the whole difference
         * between it and the window list is what it declines to say. */
        windows = api.windowList();
        body = "{\"ok\":true,\"handles\":[";
        var firstHandle = true;
        for (index = 0; index < windows.length; index++) {
            if (!isOrdinary(windows[index]))
                continue;
            if (!firstHandle)
                body += ",";
            firstHandle = false;
            body += jsonString(windowHandle(windows[index]));
        }
        body += "]}";
        return { sequence: job.sequence, status: STATUS_OK, body: body };

    case OP_WINDOW_ACTIVE:
        window = api.activeWindow();
        body = isOrdinary(window)
            ? "{\"ok\":true,\"window\":" + windowJson(window) + "}"
            : "{\"ok\":true,\"window\":null}";
        return { sequence: job.sequence, status: STATUS_OK, body: body };

    case OP_CURSOR_POSITION:
        /* Deliberately not cached and not piggybacked on a poll header. A
         * cached cursor is a wrong cursor, and the caller wants now. */
        var cursor = workspace.cursorPos;
        return {
            sequence: job.sequence,
            status: STATUS_OK,
            body: "{\"x\":" + String(Math.round(cursor.x))
                + ",\"y\":" + String(Math.round(cursor.y)) + "}"
        };

    case OP_WORK_AREA:
        var area = null;
        if (typeof workspace.clientArea === "function") {
            try {
                area = workspace.clientArea(KWin.MaximizeArea,
                    workspace.activeScreen, workspace.currentDesktop);
            } catch (error) {
                area = null;
            }
            if (area === null || area === undefined) {
                try {
                    area = workspace.clientArea(KWin.PlacementArea,
                        workspace.activeScreen, workspace.currentDesktop);
                } catch (fallbackError) {
                    area = null;
                }
            }
            if (area === null || area === undefined) {
                try {
                    area = workspace.clientArea(KWin.MaximizeArea,
                        api.activeWindow());
                } catch (windowAreaError) {
                    area = null;
                }
            }
        }
        if (area === null || area === undefined)
            return {
                sequence: job.sequence,
                status: STATUS_UNSUPPORTED,
                body: ""
            };
        return {
            sequence: job.sequence,
            status: STATUS_OK,
            body: "{\"x\":" + String(Math.round(area.x))
                + ",\"y\":" + String(Math.round(area.y))
                + ",\"width\":" + String(Math.round(area.width))
                + ",\"height\":" + String(Math.round(area.height)) + "}"
        };

    default:
        break;
    }

    /* Everything below takes a handle as its body. */
    window = findWindow(job.body.split(" ")[0]);
    if (!isOrdinary(window))
        return { sequence: job.sequence, status: STATUS_NOT_FOUND, body: "" };

    switch (job.opcode) {
    case OP_WINDOW_QUERY:
        return {
            sequence: job.sequence,
            status: STATUS_OK,
            body: "{\"ok\":true,\"window\":" + windowJson(window) + "}"
        };

    case OP_WINDOW_FOCUS:
        window.minimized = false;
        api.setActiveWindow(window);
        return { sequence: job.sequence, status: STATUS_OK, body: "" };

    case OP_WINDOW_RAISE:
        return {
            sequence: job.sequence,
            status: raiseWindow(window) ? STATUS_OK : STATUS_UNSUPPORTED,
            body: ""
        };

    case OP_WINDOW_LOWER:
        /* KWin exposes no lower. Sending the window behind every other one is
         * not the same operation and would be a worse surprise than saying so,
         * which is why this reports unsupported rather than approximating. */
        return { sequence: job.sequence, status: STATUS_UNSUPPORTED, body: "" };

    case OP_WINDOW_CLOSE:
        window.closeWindow();
        return { sequence: job.sequence, status: STATUS_OK, body: "" };

    case OP_WINDOW_MOVE_RESIZE:
        var fields = job.body.split(" ");
        if (fields.length !== 5)
            return {
                sequence: job.sequence,
                status: STATUS_INVALID_REQUEST,
                body: ""
            };
        clearMaximize(window);
        var geometry = window.frameGeometry || window.geometry;
        var requestedX = parseInt(fields[1], 10);
        var requestedY = parseInt(fields[2], 10);
        var requestedWidth = parseInt(fields[3], 10);
        var requestedHeight = parseInt(fields[4], 10);
        window.frameGeometry = {
            x: requestedX === -2147483648 ? geometry.x : requestedX,
            y: requestedY === -2147483648 ? geometry.y : requestedY,
            width: requestedWidth === 0 ? geometry.width : requestedWidth,
            height: requestedHeight === 0 ? geometry.height : requestedHeight
        };
        return { sequence: job.sequence, status: STATUS_OK, body: "" };

    case OP_WINDOW_SET_STATE:
        var state = parseInt(job.body.split(" ")[1], 10);
        if (state === 1) {
            window.minimized = true;
        } else if (state === 2) {
            window.minimized = false;
            window.setMaximize(true, true);
        } else {
            window.minimized = false;
            window.setMaximize(false, false);
        }
        return { sequence: job.sequence, status: STATUS_OK, body: "" };

    case OP_WINDOW_SET_OPACITY:
        /* The wire carries 0..255; KWin wants 0..1. */
        window.opacity = parseInt(job.body.split(" ")[1], 10) / 255;
        return { sequence: job.sequence, status: STATUS_OK, body: "" };

    case OP_WINDOW_SET_ABOVE:
        window.keepAbove = parseInt(job.body.split(" ")[1], 10) !== 0;
        return { sequence: job.sequence, status: STATUS_OK, body: "" };

    case OP_WINDOW_SET_DECORATED:
        window.noBorder = parseInt(job.body.split(" ")[1], 10) === 0;
        return { sequence: job.sequence, status: STATUS_OK, body: "" };

    case OP_WINDOW_SET_SKIP_TASKBAR:
        var skipTaskbar = parseInt(job.body.split(" ")[1], 10) !== 0;
        window.skipTaskbar = skipTaskbar;
        window.skipPager = skipTaskbar;
        window.skipSwitcher = skipTaskbar;
        return { sequence: job.sequence, status: STATUS_OK, body: "" };

    default:
        return { sequence: job.sequence, status: STATUS_UNSUPPORTED, body: "" };
    }
}

/* ----------------------------------------------------------------- loop -- */

/*
 * One lane's poll. The daemon holds this call until it has work or until its
 * idle timer fires, so there is no timer on this side and no busy loop: the
 * reply IS the wakeup. Re-parked from the callback, which is what keeps a
 * request outstanding at all times.
 */
function pollLane(lane, lost) {
    if (!running)
        return;
    callDBus(SERVICE, PATH, IFACE, "Poll", generation,
             buildPoll(lane, 0, lost),
             function (reply) {
        var jobs;
        var results = [];
        var index;

        if (!running)
            return;
        /* callDBus drops its callback on any error, including a signature
         * mismatch, so an undefined reply here is a channel that has gone
         * quiet rather than an empty answer. Re-polling counts it as lost, and
         * the count rides the next poll so the daemon can see it. */
        if (reply === undefined || reply === null) {
            pollLane(lane, lost + 1);
            return;
        }
        jobs = parsePollReply(String(reply));
        if (jobs === null) {
            /* A reply this cannot read means the two ends disagree about the
             * protocol. Stopping is the only safe answer: acting on a job that
             * may not be the one sent is worse than not acting. */
            running = false;
            print("keysharp-desktop: unreadable poll reply, stopping");
            return;
        }
        for (index = 0; index < jobs.length; index++) {
            try {
                results.push(executeJob(jobs[index]));
            } catch (error) {
                results.push({
                    sequence: jobs[index].sequence,
                    status: STATUS_INTERNAL,
                    body: ""
                });
            }
        }
        if (results.length === 0) {
            pollLane(lane, 0);
            return;
        }
        callDBus(SERVICE, PATH, IFACE, "Report", generation,
                 buildReport(results), function () {
            pollLane(lane, 0);
        });
    });
}

function start() {
    if (!detectApi()) {
        print("keysharp-desktop: no usable KWin window API, not starting");
        return;
    }
    callDBus(SERVICE, PATH, IFACE, "Hello", "", "KSK1\nend\n",
             function (reply) {
        var text = reply === undefined ? "" : String(reply);
        var line = text.indexOf("gen ");

        if (line < 0) {
            print("keysharp-desktop: no generation from the daemon");
            return;
        }
        generation = text.slice(line + 4, line + 4 + 32);
        if (generation.length !== 32) {
            print("keysharp-desktop: malformed generation from the daemon");
            return;
        }
        running = true;
        /* Both lanes are parked at once. That is the whole point of the split:
         * a bounded verb is never behind an enumeration, only ever behind at
         * most one executing job. */
        pollLane("fast", 0);
        pollLane("slow", 0);
    });
}

start();
