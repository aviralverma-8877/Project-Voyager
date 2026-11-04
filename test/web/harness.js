// Minimal test harness for the macOS JavaScriptCore shell (jsc).
// Loaded before each test/web/test_*.js by test/web/run.sh.
//
// API (fixed; web UI tests depend on it):
//   test(name, fn)                 run fn, record pass/fail, never throws
//   assertEq(actual, expected, msg) deep compare via JSON.stringify
//   assertTrue(cond, msg)
//   done()                         print "N passed, M failed"; quit(1) on failure
//
// Test files call load("data_main/lib.js") themselves (paths are relative to the
// repo root, which run.sh cds into) and must end with done().
//
// NOTE: some jsc builds ignore quit()'s exit code, so run.sh also parses the
// "N passed, M failed" summary line. Don't print a line of that shape yourself.

var window = this;

var __harness = { passed: 0, failed: 0, failures: [] };

function __log(s) {
    if (typeof print === "function") print(s);
    else if (typeof console !== "undefined") console.log(s);
}

function __show(v) {
    if (v === undefined) return "undefined";
    try {
        var s = JSON.stringify(v);
        return s === undefined ? String(v) : s;
    } catch (e) {
        return String(v);
    }
}

function test(name, fn) {
    try {
        fn();
        __harness.passed++;
        __log("ok   - " + name);
    } catch (e) {
        __harness.failed++;
        var msg = (e && e.message) ? e.message : String(e);
        __harness.failures.push(name + ": " + msg);
        __log("FAIL - " + name + ": " + msg);
        if (e && e.stack) __log("       " + String(e.stack).split("\n").join("\n       "));
    }
}

function assertEq(actual, expected, msg) {
    var a = __show(actual);
    var b = __show(expected);
    if (a !== b) {
        throw new Error((msg ? msg + ": " : "") + "expected " + b + ", got " + a);
    }
}

function assertTrue(cond, msg) {
    if (!cond) {
        throw new Error(msg || ("expected truthy, got " + __show(cond)));
    }
}

function done() {
    __log(__harness.passed + " passed, " + __harness.failed + " failed");
    if (__harness.failed > 0) quit(1);
}
