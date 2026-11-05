// L3: tx_result tracking, per-chunk retry and receiver verification.
load("data_main/lib.js");
var L = VoyagerLib;

// Fake clock: timers run only when tick() is called.
function fakeTimers() {
  var q = [], now = 0, nextId = 1;
  return {
    setTimeout: function (fn, ms) { var id = nextId++; q.push({ id: id, at: now + (ms || 0), fn: fn }); return id; },
    clearTimeout: function (id) { q = q.filter(function (t) { return t.id !== id; }); },
    tick: function (ms) {
      var target = now + ms;
      for (;;) {
        q.sort(function (a, b) { return a.at - b.at || a.id - b.id; });
        if (!q.length || q[0].at > target) break;
        var t = q.shift();
        now = t.at;
        t.fn();
      }
      now = target;
    },
    pending: function () { return q.length; },
  };
}

test("tracker resolves the matching id only", function () {
  var t = fakeTimers();
  var tr = L.createTxTracker({ setTimeout: t.setTimeout, clearTimeout: t.clearTimeout, timeoutMs: 30000 });
  var got = {};
  tr.register(7, function (r) { got[7] = r; });
  tr.register(8, function (r) { got[8] = r; });
  tr.resolve({ response_type: "tx_result", id: 8, akn: 1, username: "<b>bob</b>" });
  assertEq(got[8], { id: 8, akn: 1, username: "<b>bob</b>", timeout: false });
  assertTrue(got[7] === undefined, "7 still pending");
  tr.resolve({ id: "7", akn: 0 });
  assertEq(got[7].akn, 0);
  assertEq(tr.pendingCount(), 0);
  assertEq(t.pending(), 0, "timers cleared");
});

test("tracker times out after 30 s with akn 0", function () {
  var t = fakeTimers();
  var tr = L.createTxTracker({ setTimeout: t.setTimeout, clearTimeout: t.clearTimeout, timeoutMs: 30000 });
  var r = null;
  tr.register(1, function (x) { r = x; });
  t.tick(29999);
  assertEq(r, null);
  t.tick(1);
  assertEq(r.akn, 0);
  assertEq(r.timeout, true);
  tr.resolve({ id: 1, akn: 1 }); // late result: ignored, callback not called again
  assertEq(r.timeout, true);
});

test("tracker handles tx_result that arrives before the 202", function () {
  var t = fakeTimers();
  var tr = L.createTxTracker({ setTimeout: t.setTimeout, clearTimeout: t.clearTimeout });
  assertEq(tr.resolve({ id: 5, akn: 1, username: "a" }), false);
  var r = null;
  tr.register(5, function (x) { r = x; });
  assertEq(r.akn, 1);
  assertEq(t.pending(), 0);
});

test("akn other than 1 is a failure", function () {
  var t = fakeTimers();
  var tr = L.createTxTracker({ setTimeout: t.setTimeout, clearTimeout: t.clearTimeout });
  var r;
  tr.register(1, function (x) { r = x; });
  tr.resolve({ id: 1, akn: 2 });
  assertEq(r.akn, 0);
});

done();
