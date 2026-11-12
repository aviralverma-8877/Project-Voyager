// L3: Contract 3 tx_started handling in createTxTracker: queued/sending states,
// timer re-arm, safety timeout, and every arrival order of 202 / tx_started / tx_result.
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

function setup() {
  var t = fakeTimers();
  var tr = L.createTxTracker({ setTimeout: t.setTimeout, clearTimeout: t.clearTimeout });
  var rec = { result: null, calls: 0, states: [] };
  rec.cb = function (r) { rec.result = r; rec.calls++; };
  rec.onState = function (s, info) { rec.states.push(s === "sending" ? s + ":" + info.expectedMs + ":" + info.attempts : s); };
  return { t: t, tr: tr, rec: rec };
}

test("defaults: 10 min queue safety timer, 5 s slack", function () {
  assertEq(L.TX_QUEUE_TIMEOUT_MS, 600000);
  assertEq(L.TX_START_SLACK_MS, 5000);
});

test("202 -> queued; only the 10 min safety timer is armed", function () {
  var s = setup();
  s.tr.register(1, s.rec.cb, s.rec.onState);
  assertEq(s.rec.states, ["queued"]);
  assertEq(s.tr.state(1), "queued");
  s.t.tick(599999);
  assertEq(s.rec.result, null, "no timeout before 10 min");
  s.t.tick(1);
  assertEq(s.rec.result, { id: 1, akn: 0, username: "", timeout: true, phase: "queued" });
  assertEq(s.tr.pendingCount(), 0);
});

test("tx_started re-arms the timer to expected_ms + 5000", function () {
  var s = setup();
  s.tr.register(2, s.rec.cb, s.rec.onState);
  s.t.tick(300000); // waited 5 min in the queue
  assertEq(s.tr.started({ response_type: "tx_started", id: 2, expected_ms: 20000, attempts: 4 }), true);
  assertEq(s.rec.states, ["queued", "sending:20000:4"]);
  assertEq(s.tr.state(2), "sending");
  assertEq(s.t.pending(), 1, "old safety timer cleared, one new timer");
  s.t.tick(24999);
  assertEq(s.rec.result, null);
  s.t.tick(1);
  assertEq(s.rec.result.timeout, true);
  assertEq(s.rec.result.phase, "sending");
});

test("202 -> tx_started -> tx_result resolves once and clears timers", function () {
  var s = setup();
  s.tr.register(3, s.rec.cb, s.rec.onState);
  s.tr.started({ id: 3, expected_ms: 1000, attempts: 4 });
  assertEq(s.tr.resolve({ id: 3, akn: 1, username: "B" }), true);
  assertEq(s.rec.result, { id: 3, akn: 1, noack: false, username: "B", timeout: false });
  assertEq(s.t.pending(), 0);
  s.t.tick(700000);
  assertEq(s.rec.calls, 1);
});

test("tx_started before the 202 -> register starts in 'sending'", function () {
  var s = setup();
  assertEq(s.tr.started({ id: "4", expected_ms: 8000, attempts: 4 }), false);
  s.tr.register(4, s.rec.cb, s.rec.onState);
  assertEq(s.rec.states, ["queued", "sending:8000:4"]);
  assertEq(s.tr.state(4), "sending");
  s.t.tick(12999);
  assertEq(s.rec.result, null);
  s.t.tick(1);
  assertEq(s.rec.result.phase, "sending");
});

test("tx_started and tx_result both before the 202 -> resolved at register", function () {
  var s = setup();
  s.tr.started({ id: 5, expected_ms: 8000, attempts: 4 });
  s.tr.resolve({ id: 5, akn: 0, username: "" });
  s.tr.register(5, s.rec.cb, s.rec.onState);
  assertEq(s.rec.result.akn, 0);
  assertEq(s.rec.result.timeout, false);
  assertEq(s.rec.states, [], "no state callbacks for an already finished item");
  assertEq(s.t.pending(), 0);
});

test("tx_result before tx_started (both before 202) still resolves", function () {
  var s = setup();
  s.tr.resolve({ id: 6, akn: 1, username: "B" });
  s.tr.started({ id: 6, expected_ms: 8000 });
  s.tr.register(6, s.rec.cb, s.rec.onState);
  assertEq(s.rec.result.akn, 1);
  assertEq(s.t.pending(), 0);
});

test("202 -> tx_result without tx_started (older firmware) works", function () {
  var s = setup();
  s.tr.register(7, s.rec.cb, s.rec.onState);
  s.tr.resolve({ id: 7, akn: 1 });
  assertEq(s.rec.result.akn, 1);
  assertEq(s.rec.states, ["queued"]);
});

test("late tx_started after the result is ignored", function () {
  var s = setup();
  s.tr.register(8, s.rec.cb, s.rec.onState);
  s.tr.resolve({ id: 8, akn: 1 });
  assertEq(s.tr.started({ id: 8, expected_ms: 1000 }), false);
  assertEq(s.rec.states, ["queued"]);
  assertEq(s.t.pending(), 0);
});

test("tx_started for another id does not touch this item", function () {
  var s = setup();
  s.tr.register(9, s.rec.cb, s.rec.onState);
  s.tr.started({ id: 10, expected_ms: 1000 });
  assertEq(s.tr.state(9), "queued");
  s.t.tick(600000);
  assertEq(s.rec.result.phase, "queued");
});

test("bad expected_ms falls back to 30 s + slack; bad attempts -> null", function () {
  [undefined, null, "abc", -5].forEach(function (bad) {
    var s = setup();
    s.tr.register(11, s.rec.cb, s.rec.onState);
    s.tr.started({ id: 11, expected_ms: bad, attempts: "x" });
    assertEq(s.rec.states[1], "sending:null:null", "info for " + bad);
    s.t.tick(34999);
    assertEq(s.rec.result, null, "no timeout yet for " + bad);
    s.t.tick(1);
    assertEq(s.rec.result.timeout, true, "timeout for " + bad);
  });
});

test("expected_ms 0 (time unknown) keeps the running 10 min safety timer", function () {
  var s = setup();
  s.tr.register(14, s.rec.cb, s.rec.onState);
  s.t.tick(100000); // 100 s in the queue
  assertEq(s.tr.started({ id: 14, expected_ms: 0, attempts: 4 }), true);
  assertEq(s.tr.state(14), "sending");
  assertEq(s.rec.states, ["queued", "sending:0:4"]);
  assertEq(s.t.pending(), 1, "still one timer");
  s.t.tick(35000);
  assertEq(s.rec.result, null, "no 30 s fallback");
  s.t.tick(464999); // 599999 ms since the 202
  assertEq(s.rec.result, null);
  s.t.tick(1); // 10 min since the 202: original timer fires
  assertEq(s.rec.result.timeout, true);
  assertEq(s.rec.result.phase, "sending");
});

test("expected_ms 0 before the 202 arms the 10 min safety timer", function () {
  var s = setup();
  s.tr.started({ id: 15, expected_ms: "0" });
  s.tr.register(15, s.rec.cb, s.rec.onState);
  assertEq(s.tr.state(15), "sending");
  s.t.tick(599999);
  assertEq(s.rec.result, null);
  s.t.tick(1);
  assertEq(s.rec.result.phase, "sending");
});

test("expected_ms 0 then tx_result resolves normally", function () {
  var s = setup();
  s.tr.register(16, s.rec.cb, s.rec.onState);
  s.tr.started({ id: 16, expected_ms: 0 });
  s.tr.resolve({ id: 16, akn: 1 });
  assertEq(s.rec.result.akn, 1);
  assertEq(s.t.pending(), 0);
});

test("huge expected_ms is capped at 10 min + slack", function () {
  var s = setup();
  s.tr.register(12, s.rec.cb, s.rec.onState);
  s.tr.started({ id: 12, expected_ms: 1e12 });
  s.t.tick(605000);
  assertEq(s.rec.result.timeout, true);
});

test("malformed tx_started is ignored", function () {
  var s = setup();
  assertEq(s.tr.started(null), false);
  assertEq(s.tr.started({}), false);
  assertEq(s.tr.started({ id: null }), false);
  assertEq(s.tr.started("x"), false);
});

test("early maps are bounded (oldest dropped after 50)", function () {
  var s = setup();
  for (var i = 1000; i < 1060; i++) s.tr.started({ id: i, expected_ms: 1000 });
  s.tr.register(1000, s.rec.cb, s.rec.onState); // dropped -> plain queued
  assertEq(s.rec.states, ["queued"]);
  var got = [];
  s.tr.register(1059, function () {}, function (st) { got.push(st); }); // newest kept
  assertEq(got, ["queued", "sending"]);
});

test("register without onState still works", function () {
  var s = setup();
  s.tr.register(13, s.rec.cb);
  s.tr.started({ id: 13, expected_ms: 1000 });
  s.tr.resolve({ id: 13, akn: 1 });
  assertEq(s.rec.result.akn, 1);
});

done();
