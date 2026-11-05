// Kermit protocol: encoding, framing, negotiation, and end-to-end transfers over an
// emulated LoRa serial bridge (lossy, fragmenting, noisy). Time is virtual: this
// context has no setTimeout, so protocol timeouts cost nothing and runs are exact.

load("data_main/kermit.js");
var K = KERMIT;

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

function str(bytes) {
  var s = "";
  for (var i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]);
  return s;
}
function bytesOf(s) {
  var a = [];
  for (var i = 0; i < s.length; i++) a.push(s.charCodeAt(i));
  return a;
}

/* ---------- unit ---------- */

test("CRC-16/KERMIT check value", function () {
  assertEq(K.crc16(bytesOf("123456789"), 0, 9), 0x2189);
});

test("type-1 checksum: the classic NAK packet ^A# N3", function () {
  assertEq(str(K.buildPacket(0, "N", [], 1)), "\x01# N3\r");
});

test("encode/decode round-trips every byte value, all prefix options", function () {
  var all = [];
  for (var i = 0; i < 256; i++) all.push(i);
  all = all.concat([0, 0, 0, 0, 0, 0x23, 0x23, 0x23, 0x7e, 0x7e, 0x7e, 0x26, 0xff, 0xff, 0xff, 0xff]);
  [{ qbin: 0, rept: 0 }, { qbin: 0, rept: 0x7e }, { qbin: 0x26, rept: 0 }, { qbin: 0x26, rept: 0x7e }].forEach(function (q) {
    var pos = 0, out = [];
    while (pos < all.length) {
      var c = K.encodeChunk(all, pos, 90, q);
      assertTrue(c.bytes.length <= 90, "chunk over limit");
      for (var j = 0; j < c.bytes.length; j++) {
        assertTrue(c.bytes[j] >= 32 && c.bytes[j] !== 127, "control char in encoded data: " + c.bytes[j]);
        if (q.qbin) assertTrue(c.bytes[j] < 128, "8-bit byte despite 8th-bit prefixing");
      }
      out = out.concat(K.decode(c.bytes, q));
      pos = c.next;
    }
    assertEq(out, all, "round trip with " + JSON.stringify(q));
  });
});

test("repeat prefix compresses runs", function () {
  var zeros = [];
  for (var i = 0; i < 200; i++) zeros.push(0);
  var c = K.encodeChunk(zeros, 0, 90, { qbin: 0, rept: 0x7e });
  assertEq(c.next, 200, "200 zeros fit one packet");
  assertTrue(c.bytes.length <= 12, "compressed to " + c.bytes.length + " bytes");
});

test("basic and long packets parse back, with both check types", function () {
  [1, 3].forEach(function (ck) {
    [5, 80, 200].forEach(function (n) {
      var data = [];
      for (var i = 0; i < n; i++) data.push(33 + (i % 90));
      var pkt = K.buildPacket(7, "D", data, ck);
      assertTrue(pkt.length <= 251, "fits one LoRa payload");
      var got = [];
      var p = K.createParser(function (x) { got.push(x); });
      p.feed(pkt);
      assertEq(got.length, 1, "one packet (" + n + "B, ck " + ck + ")");
      assertEq([got[0].seq, got[0].type], [7, "D"]);
      assertEq(got[0].data(ck), data);
      if (ck === 3) assertEq(got[0].data(1), null, "wrong check type rejected");
    });
  });
});

test("parser drops a frame cut short and resyncs on the next one", function () {
  var a = K.buildPacket(1, "D", bytesOf("first-packet-data"), 3);
  var b = K.buildPacket(2, "D", bytesOf("second"), 3);
  var got = [];
  var p = K.createParser(function (x) { got.push(x); });
  p.feed(a.slice(0, 9)); // first frame lost its tail
  p.feed(b);
  assertEq(got.length, 1);
  assertEq(got[0].seq, 2);
  assertEq(str(got[0].data(3)), "second");
});

test("negotiation: our own init gives CRC, repeat, long packets, attributes", function () {
  var n = K.negotiate(K.initParams(10000));
  assertEq([n.ckType, n.rept, n.qbin, n.long, n.attr, n.maxl], [3, 0x7e, 0, true, true, K.MAX_LONG]);
});

test("negotiation: a minimal peer falls back to basic packets and type-1 checks", function () {
  // MAXL 80, TIME 5, NPAD 0, PADC @, EOL CR, QCTL # ... nothing else.
  var n = K.negotiate(bytesOf("p%" + " @-#"));
  assertEq([n.ckType, n.rept, n.qbin, n.long, n.attr, n.maxl], [1, 0, 0, false, false, 80]);
});

test("negotiation: a peer asking for 8th-bit prefixing gets it", function () {
  var init = K.initParams(10000);
  init[6] = 0x26; // '&'
  assertEq(K.negotiate(init).qbin, 0x26);
});

/* ---------- emulated LoRa bridge ---------- */

var FRAG = 251, LATENCY_MS = 300;

// The firmware closes a LoRa frame at CR (end of a Kermit packet) or at 251 bytes.
// opts.drop(dir, n) drops the n-th frame in a direction ("AB" sender->receiver);
// opts.inject(dir, n) returns noise bytes delivered just before frame n;
// opts.split: cut frames at random-ish points too (bridge reading mid-packet).
function frames(bytes, split) {
  var out = [], cur = [];
  for (var i = 0; i < bytes.length; i++) {
    cur.push(bytes[i]);
    var cut = bytes[i] === 0x0d || cur.length >= FRAG || (split && cur.length === 37);
    if (cut) { out.push(new Uint8Array(cur)); cur = []; }
  }
  if (cur.length) out.push(new Uint8Array(cur));
  return out;
}

function transfer(fileBytes, opts) {
  opts = opts || {};
  __now = 0; __timers = [];
  var toA = [], toB = [], count = { AB: 0, BA: 0 };
  var r = { sender: null, receiver: null, name: null, data: null, maxFrame: 0, frames: 0 };

  function link(dir, bytes, sinks) {
    frames(bytes, opts.split).forEach(function (f) {
      var n = ++count[dir];
      r.frames++;
      if (f.length > r.maxFrame) r.maxFrame = f.length;
      var noise = opts.inject && opts.inject(dir, n);
      if (noise) deliver(sinks, new Uint8Array(noise));
      if (opts.drop && opts.drop(dir, n)) return;
      deliver(sinks, f);
    });
  }
  function deliver(sinks, f) {
    setTimeout(function () { sinks.slice().forEach(function (cb) { cb(f); }); }, LATENCY_MS);
  }
  function sub(list) {
    return function (cb) {
      list.push(cb);
      return function () { var i = list.indexOf(cb); if (i >= 0) list.splice(i, 1); };
    };
  }

  var sender = K.createSender({
    file: { name: "C:\\pics\\photo.jpg", size: fileBytes.length, data: fileBytes },
    timeout: 2000,
    write: function (b) { link("AB", b, toB); },
    onData: sub(toA),
    onComplete: function (ok, err) { r.sender = ok ? "ok" : "fail: " + err; },
  });
  var receiver = K.createReceiver({
    timeout: 2000,
    write: function (b) { link("BA", b, toA); },
    onData: sub(toB),
    onFileStart: function (name) { r.name = name; },
    onComplete: function (ok, data, err) { r.receiver = ok ? "ok" : "fail: " + err; r.data = data; },
  });
  // Receiver first (as in the page: it starts on "go"), sender a moment later.
  receiver.start();
  setTimeout(function () { sender.start(); }, opts.senderDelay || 100);
  runClock(30 * 60 * 1000);
  r.listeners = toA.length + toB.length;
  return r;
}

function pattern(n) {
  var a = new Uint8Array(n);
  for (var i = 0; i < n; i++) a[i] = (i * 131 + (i >> 8)) & 0xff; // all byte values, incl. 0x00/0x0D
  return a;
}

function assertDelivered(r, file) {
  assertEq(r.sender, "ok", "sender");
  assertEq(r.receiver, "ok", "receiver");
  assertEq(r.name, "photo.jpg", "path stripped from the name");
  assertEq(r.data.length, file.length, "length");
  for (var i = 0; i < file.length; i++) if (r.data[i] !== file[i]) throw new Error("byte " + i + " differs");
  assertEq(r.listeners, 0, "data listeners released");
  assertTrue(r.maxFrame <= 251, "a frame exceeded the LoRa payload: " + r.maxFrame);
}

test("loopback: clean link delivers the file byte-exact", function () {
  var f = pattern(5000);
  assertDelivered(transfer(f), f);
});

test("loopback: sizes 0, 1, 221, 222, 223, 4096", function () {
  [0, 1, 221, 222, 223, 4096].forEach(function (n) {
    var f = pattern(n);
    assertDelivered(transfer(f), f);
  });
});

test("loopback: every Kermit packet travels as one LoRa frame", function () {
  var f = pattern(3000);
  var r = transfer(f);
  assertDelivered(r, f);
  // 3000 bytes of binary at ~222 encoded bytes per packet -> roughly 16-18 D packets;
  // plus N, S, Y... handshakes. Far fewer frames than a byte-split stream would need.
  assertTrue(r.frames < 60, "frames: " + r.frames);
});

test("loopback: lost data packets are resent", function () {
  var f = pattern(4000);
  assertDelivered(transfer(f, { drop: function (d, n) { return d === "AB" && (n === 5 || n === 9 || n === 10); } }), f);
});

test("loopback: lost ACKs make duplicates that are not stored twice", function () {
  var f = pattern(4000);
  assertDelivered(transfer(f, { drop: function (d, n) { return d === "BA" && (n === 3 || n === 6 || n === 7); } }), f);
});

test("loopback: lost ACK of the final B packet still completes on both sides", function () {
  var f = pattern(500);
  var nBA = 0; // dry run: the receiver's last frame is the ACK of B
  transfer(f, { drop: function (d, n) { if (d === "BA") nBA = n; return false; } });
  assertTrue(nBA > 3, "dry run counted " + nBA + " receiver frames");
  assertDelivered(transfer(f, { drop: function (d, n) { return d === "BA" && n === nBA; } }), f);
});

test("loopback: packets split across LoRa frames still arrive", function () {
  var f = pattern(3000);
  assertDelivered(transfer(f, { split: true }), f);
});

test("loopback: line noise between packets is ignored", function () {
  var f = pattern(3000);
  assertDelivered(transfer(f, {
    inject: function (d, n) { return n % 4 === 0 ? [0x00, 0x43, 0x06, 0x15, 0x7f, 0x0d, 0x0a] : null; },
  }), f);
});

test("loopback: first S packets lost (receiver's bridge not on yet) recovers", function () {
  var f = pattern(1000);
  assertDelivered(transfer(f, { drop: function (d, n) { return d === "AB" && n <= 2; } }), f);
});

test("loopback: a dead link fails both sides instead of hanging", function () {
  var r = transfer(pattern(3000), { drop: function (d, n) { return d === "AB" && n >= 4; } });
  assertTrue(/^fail/.test(r.sender), "sender should fail, got " + r.sender);
  assertTrue(/^fail/.test(r.receiver), "receiver should fail, got " + r.receiver);
});

test("loopback: receiver cancel stops the sender", function () {
  __now = 0; __timers = [];
  var toA = [], toB = [];
  var rs = null;
  function deliver(list, b) { setTimeout(function () { list.slice().forEach(function (cb) { cb(b); }); }, 50); }
  var sender = K.createSender({ file: { name: "x", data: pattern(20000) }, timeout: 2000,
    write: function (b) { deliver(toB, b); }, onData: function (cb) { toA.push(cb); },
    onComplete: function (ok, err) { rs = ok ? "ok" : err; } });
  var receiver = K.createReceiver({ timeout: 2000, write: function (b) { deliver(toA, b); },
    onData: function (cb) { toB.push(cb); }, onComplete: function () {} });
  receiver.start(); sender.start();
  setTimeout(function () { receiver.cancel(); }, 3000);
  runClock(10 * 60 * 1000);
  assertTrue(/aborted/.test(rs), "sender result: " + rs);
});

done();
