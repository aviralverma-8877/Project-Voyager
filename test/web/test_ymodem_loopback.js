// End-to-end YMODEM sender <-> receiver over an emulated LoRa serial bridge.
//
// The bridge is modelled the way the firmware behaves: every write is cut into
// LORA_SERIAL packets of at most 251 bytes, each delivered after a fixed latency,
// unacknowledged, so a lost packet is simply gone. Time is virtual (this context has
// no setTimeout), so a 2 s protocol timeout costs nothing and runs are deterministic.

load("data_main/ymodem.js");

/* ---------- virtual clock ---------- */

var __now = 0, __seq = 0, __timers = [];
function setTimeout(fn, ms) {
  var t = { at: __now + (ms || 0), seq: __seq++, fn: fn };
  __timers.push(t);
  return t;
}
function clearTimeout(t) {
  var i = __timers.indexOf(t);
  if (i >= 0) __timers.splice(i, 1);
}
function runClock(limitMs) {
  while (__timers.length > 0) {
    __timers.sort(function (a, b) { return a.at - b.at || a.seq - b.seq; });
    var t = __timers.shift();
    if (t.at > limitMs) return;
    __now = t.at;
    t.fn();
  }
}

/* ---------- emulated bridge ---------- */

var FRAG = 251, LATENCY_MS = 300;

// opts.drop(dir, n, bytes) -> true drops the n-th packet (1-based) in that direction
// ("AB" sender->receiver, "BA" receiver->sender). opts.inject(dir, n) -> bytes
// delivered just before that packet (line noise / another node's traffic).
function transfer(fileBytes, opts) {
  opts = opts || {};
  __now = 0; __timers = [];
  var toA = [], toB = [], count = { AB: 0, BA: 0 };
  var result = { sender: null, receiver: null, name: null, data: null };

  function link(dir, bytes, sinks) {
    for (var i = 0; i < bytes.length; i += FRAG) {
      var f = bytes.slice(i, i + FRAG);
      var n = ++count[dir];
      var noise = opts.inject && opts.inject(dir, n);
      if (noise) deliver(sinks, new Uint8Array(noise));
      if (opts.drop && opts.drop(dir, n, f)) continue;
      deliver(sinks, f);
    }
  }
  function deliver(sinks, f) {
    setTimeout(function () { sinks.forEach(function (cb) { cb(f); }); }, LATENCY_MS);
  }
  function sub(list) {
    return function (cb) {
      list.push(cb);
      return function () { var i = list.indexOf(cb); if (i >= 0) list.splice(i, 1); };
    };
  }

  var sender = YMODEM.createSender({
    file: { name: "photo.jpg", size: fileBytes.length, data: fileBytes },
    timeout: 2000,
    write: function (b) { link("AB", b, toB); },
    onData: sub(toA),
    onComplete: function (ok, err) { result.sender = ok ? "ok" : "fail: " + err; },
  });
  var receiver = YMODEM.createReceiver({
    timeout: 2000,
    write: function (b) { link("BA", b, toA); },
    onData: sub(toB),
    onFileStart: function (name) { result.name = name; },
    onComplete: function (ok, data, err) {
      result.receiver = ok ? "ok" : "fail: " + err;
      result.data = data;
    },
  });
  sender.start();
  receiver.start();
  runClock(10 * 60 * 1000);
  result.listeners = toA.length + toB.length;
  return result;
}

function pattern(n) {
  var a = new Uint8Array(n);
  for (var i = 0; i < n; i++) a[i] = (i * 131 + (i >> 8)) & 0xff; // includes 0x00 bytes
  return a;
}

function assertDelivered(r, file) {
  assertEq(r.sender, "ok", "sender");
  assertEq(r.receiver, "ok", "receiver");
  assertEq(r.name, "photo.jpg", "file name");
  assertEq(r.data.length, file.length, "length");
  for (var i = 0; i < file.length; i++) {
    if (r.data[i] !== file[i]) throw new Error("byte " + i + " differs");
  }
  assertEq(r.listeners, 0, "data listeners released");
}

// Packets per direction at the start: BA#1 'C'; AB#1 block 0; BA#2 ACK; BA#3 'C';
// then each 1029-byte STX block is AB packets 5k-3 .. 5k+1 and its ACK one BA packet.

test("loopback: clean link delivers the file byte-exact", function () {
  var f = pattern(5000);
  assertDelivered(transfer(f), f);
});

test("loopback: sizes at block boundaries (1, 128, 129, 1024, 2048)", function () {
  [1, 128, 129, 1024, 2048].forEach(function (n) {
    var f = pattern(n);
    assertDelivered(transfer(f), f);
  });
});

test("loopback: lost packet in the middle of a data block is resent", function () {
  var f = pattern(5000);
  assertDelivered(transfer(f, { drop: function (d, n) { return d === "AB" && n === 8; } }), f);
});

test("loopback: lost ACK of block 0 (receiver's 'C' still arrives) recovers", function () {
  var f = pattern(3000);
  assertDelivered(transfer(f, { drop: function (d, n) { return d === "BA" && n === 2; } }), f);
});

test("loopback: lost 'C' after block 0 recovers", function () {
  var f = pattern(3000);
  assertDelivered(transfer(f, { drop: function (d, n) { return d === "BA" && n === 3; } }), f);
});

test("loopback: lost data-block ACK makes a duplicate that is not stored twice", function () {
  var f = pattern(4000);
  assertDelivered(transfer(f, { drop: function (d, n) { return d === "BA" && n === 4; } }), f);
});

test("loopback: lost EOT ACK still completes on both sides", function () {
  var f = pattern(1500);
  // 1500 bytes = 2 STX blocks; BA: C, ACK, C, ACK(b1), ACK(b2), ACK(EOT)=#6
  assertDelivered(transfer(f, { drop: function (d, n) { return d === "BA" && n === 6; } }), f);
});

// Before the fix, any unexpected byte at the head of the buffer stalled every later
// expect(). (A stray ACK reaching the sender can never be told apart from a real one;
// that is why the bridge must stay point-to-point, see send_to_serial's lora_serial gate.)
test("loopback: stray bytes on the line do not stall the parser", function () {
  var f = pattern(3000);
  assertDelivered(transfer(f, {
    inject: function (d, n) {
      if (d === "AB" && (n === 4 || n === 9)) return [0x43, 0x00, 0x7f, 0x06, 0x41];
      if (d === "BA" && n === 3) return [0x00, 0x7f, 0x41];
      return null;
    },
  }), f);
});

test("loopback: a dead link fails both sides instead of hanging", function () {
  var r = transfer(pattern(3000), { drop: function (d, n) { return d === "AB" && n >= 3; } });
  assertTrue(/^fail/.test(r.sender), "sender should fail, got " + r.sender);
  assertTrue(/^fail/.test(r.receiver), "receiver should fail, got " + r.receiver);
});

done();
