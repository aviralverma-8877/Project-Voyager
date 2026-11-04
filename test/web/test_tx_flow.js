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

function runSender(chunks, results, opts) {
  // results: array of {ok,fatal} returned per attempt, in order
  var t = fakeTimers();
  var sentLog = [], progress = [], fin = null, i = 0;
  var s = L.createChunkSender({
    chunks: chunks, waitMs: 10, retryDelayMs: 1000, maxRetries: 2,
    schedule: t.setTimeout,
    isCancelled: opts && opts.isCancelled,
    send: function (chunk, idx, done) {
      sentLog.push(idx);
      var r = results[i++] || { ok: true };
      t.setTimeout(function () { done(r); }, 1);
    },
    onProgress: function (sent, total) { progress.push(sent + "/" + total); },
    onFinish: function (r) { fin = r; },
  });
  s.start();
  t.tick(100000);
  return { sent: sentLog, progress: progress, fin: fin };
}

test("sender: all chunks ok, progress reported after each increment", function () {
  var r = runSender(["a", "b", "c"], []);
  assertEq(r.sent, [0, 1, 2]);
  assertEq(r.progress, ["1/3", "2/3", "3/3"]);
  assertEq(r.fin, { ok: true, sent: 3, total: 3, reason: "" });
});

test("sender: akn 0 retries THE SAME chunk, then continues", function () {
  var r = runSender(["a", "b", "c"], [{ ok: true }, { ok: false }, { ok: false }, { ok: true }, { ok: true }]);
  assertEq(r.sent, [0, 1, 1, 1, 2]);
  assertEq(r.fin.ok, true);
});

test("sender: aborts after 1 try + 2 retries of the same chunk", function () {
  var r = runSender(["a", "b", "c"], [{ ok: true }, { ok: false, reason: "timeout" }, { ok: false }, { ok: false, reason: "no result" }]);
  assertEq(r.sent, [0, 1, 1, 1]);
  assertEq(r.fin.ok, false);
  assertEq(r.fin.sent, 1);
  assertEq(r.fin.total, 3);
  assertEq(r.fin.reason, "no result");
});

test("sender: fatal error (413) aborts without retry", function () {
  var r = runSender(["a", "b"], [{ ok: false, fatal: true, reason: "message too long" }]);
  assertEq(r.sent, [0]);
  assertEq(r.fin, { ok: false, sent: 0, total: 2, reason: "message too long" });
});

test("sender: cancel stops before the next chunk", function () {
  var cancelled = false;
  var t = fakeTimers();
  var fin = null, sent = [];
  var s = L.createChunkSender({
    chunks: ["a", "b", "c"], waitMs: 10, schedule: t.setTimeout,
    isCancelled: function () { return cancelled; },
    send: function (c, idx, done) { sent.push(idx); if (idx === 1) cancelled = true; t.setTimeout(function () { done({ ok: true }); }, 1); },
    onFinish: function (r) { fin = r; },
  });
  s.start();
  t.tick(1000);
  assertEq(sent, [0, 1]);
  assertEq(fin.ok, false);
  assertEq(fin.reason, "cancelled");
  assertEq(fin.sent, 2);
});

test("sender: empty chunk list finishes immediately", function () {
  var r = runSender([], []);
  assertEq(r.sent, []);
  assertEq(r.fin.ok, true);
});

test("receiver: verified when count and SHA-256 match", function () {
  var url = "data:text/plain;base64,SGVsbG8sIFdvcmxkIQ==";
  var chunks = L.chunkString(url, 7);
  var rx = L.createFileReceiver();
  assertTrue(!rx.addChunk("x"), "ignores chunks when idle");
  rx.start({ totalPackets: chunks.length, fileName: "<b>a.txt" });
  chunks.forEach(function (c, i) {
    assertTrue(rx.addChunk(c));
    assertEq(rx.progress().received, i + 1, "count incremented before display");
  });
  assertEq(rx.progress().percent, 100);
  var res = rx.finish({ totalPackets: chunks.length, sha256: L.sha256Hex(url).toUpperCase() });
  assertTrue(res.verified, res.reason);
  assertTrue(rx.canDownload());
  assertEq(rx.data(), url);
  assertEq(rx.fileName(), "<b>a.txt");
});

test("receiver: missing chunk -> corrupted", function () {
  var url = "data:text/plain;base64,SGVsbG8sIFdvcmxkIQ==";
  var chunks = L.chunkString(url, 7);
  var rx = L.createFileReceiver();
  rx.start({ totalPackets: chunks.length });
  chunks.slice(1).forEach(function (c) { rx.addChunk(c); });
  var res = rx.finish({ totalPackets: chunks.length, sha256: L.sha256Hex(url) });
  assertTrue(!res.verified);
  assertTrue(!res.countOk);
  assertTrue(!rx.canDownload());
});

test("receiver: duplicated chunk -> corrupted", function () {
  var rx = L.createFileReceiver();
  rx.start({ totalPackets: 2 });
  rx.addChunk("ab"); rx.addChunk("ab"); rx.addChunk("cd");
  var res = rx.finish({ totalPackets: 2, sha256: L.sha256Hex("abcd") });
  assertTrue(!res.verified);
});

test("receiver: right count, wrong hash -> corrupted", function () {
  var rx = L.createFileReceiver();
  rx.start({ totalPackets: 2 });
  rx.addChunk("ab"); rx.addChunk("cX");
  var res = rx.finish({ totalPackets: 2, sha256: L.sha256Hex("abcd") });
  assertTrue(res.countOk);
  assertTrue(!res.hashOk);
  assertTrue(!res.verified);
  assertEq(res.reason, "SHA-256 mismatch");
});

test("receiver: missing sha256 -> not verified", function () {
  var rx = L.createFileReceiver();
  rx.start({ totalPackets: 1 });
  rx.addChunk("ab");
  assertTrue(!rx.finish({ totalPackets: 1, sha256: "" }).verified);
});

test("receiver: cancel stops accepting chunks and blocks download", function () {
  var rx = L.createFileReceiver();
  rx.start({ totalPackets: 3 });
  rx.addChunk("a");
  assertTrue(rx.cancel());
  assertTrue(!rx.isActive());
  assertTrue(!rx.addChunk("b"));
  assertTrue(!rx.canDownload());
  assertEq(rx.finish({ totalPackets: 3, sha256: "" }), null, "late disable ignored");
});

test("receiver: no data -> no download", function () {
  var rx = L.createFileReceiver();
  assertTrue(!rx.canDownload());
});

test("end to end: sender data == receiver data, verified", function () {
  var url = "data:application/octet-stream;base64,";
  for (var i = 0; i < 5000; i++) url += "QUJD"[i % 4];
  var chunks = L.chunkString(url, 200);
  var rx = L.createFileReceiver();
  rx.start({ totalPackets: L.totalPackets(url.length, 200) });
  var t = fakeTimers(), fin = null;
  L.createChunkSender({
    chunks: chunks, waitMs: 10, schedule: t.setTimeout,
    send: function (c, idx, done) { rx.addChunk(c); t.setTimeout(function () { done({ ok: true }); }, 1); },
    onFinish: function (r) { fin = r; },
  }).start();
  t.tick(1000000);
  assertTrue(fin.ok);
  var disable = L.parseLoraRx(JSON.stringify({ name: "a", mac: "m", data: L.buildDisableFileTx(fin.total, L.sha256Hex(url)) }));
  var res = rx.finish(disable);
  assertTrue(res.verified, res.reason);
});

done();
