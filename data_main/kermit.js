/*
 * Project Voyager - Kermit file transfer for the LoRa serial bridge.
 *
 * Why Kermit: every Kermit packet is short, self-delimited (SOH ... CR) and free of
 * raw control characters, so one packet travels as one LoRa frame and a lost frame
 * costs one small retransmission. (YMODEM's 1 KB blocks span five unacknowledged
 * LoRa frames; validated on hardware: only Kermit survived the link.)
 *
 * Profile (negotiated in the S packet; falls back to what the peer supports, so it
 * interoperates with Tera Term / C-Kermit / G-Kermit):
 *   - long packets, capped at MAX_LONG so a whole packet fits one 251-byte LoRa payload
 *     (basic 94-byte packets when the peer has no long-packet support);
 *   - stop-and-wait (window 1): LoRa is half-duplex without carrier sense, so ACKs of
 *     a sliding window would collide with the data;
 *   - block check 3 (CRC-16/KERMIT) when the peer agrees, else type 1;
 *   - control-prefix '#', repeat-count '~', 8th-bit prefix only if the peer asks;
 *   - A (attribute) packet with the file size when the peer supports attributes.
 *
 * API mirrors the old YMODEM module so the page code stays the same:
 *   KERMIT.createSender({file:{name,size,data}, write, onData, timeout, onProgress, onComplete})
 *   KERMIT.createReceiver({write, onData, timeout, onFileStart, onProgress, onComplete})
 *   -> {start(), cancel()}. onData(cb) may return an unsubscribe function.
 * No DOM. Exposes one global: KERMIT.
 */
var KERMIT = (function () {
  "use strict";

  var MARK = 0x01; // SOH
  var EOL = 0x0d; // CR
  var QCTL = 0x23; // '#'
  var REPT = 0x7e; // '~'
  var MAX_BASIC = 94; // LEN field limit
  var MAX_LONG = 230; // our cap for extended packets (LoRa payload is 251)
  var MAX_RETRIES = 10;
  var DEFAULT_TIMEOUT_MS = 10000;
  var CAPAS_LONG = 2, CAPAS_SWS = 4, CAPAS_ATTR = 8;

  function tochar(x) { return (x + 32) & 0xff; }
  function unchar(c) { return (c - 32) & 0xff; }
  function ctl(c) { return c ^ 64; }

  /* ---------- block checks ---------- */

  function check1(bytes, from, to) {
    var s = 0;
    for (var i = from; i < to; i++) s += bytes[i];
    return [tochar((s + ((s & 192) >> 6)) & 63)];
  }

  // CRC-16/KERMIT: poly 0x1021 reflected (0x8408), init 0, no final xor.
  function crc16(bytes, from, to) {
    var crc = 0;
    for (var i = from; i < to; i++) {
      crc ^= bytes[i];
      for (var j = 0; j < 8; j++) crc = crc & 1 ? (crc >>> 1) ^ 0x8408 : crc >>> 1;
    }
    return crc & 0xffff;
  }

  function check3(bytes, from, to) {
    var c = crc16(bytes, from, to);
    return [tochar((c >> 12) & 0x0f), tochar((c >> 6) & 0x3f), tochar(c & 0x3f)];
  }

  function blockCheck(type, bytes, from, to) {
    return type === 3 ? check3(bytes, from, to) : check1(bytes, from, to);
  }

  function ckLen(type) { return type === 3 ? 3 : 1; }

  /* ---------- packet build ---------- */

  // data: array of already-encoded bytes. Long form when it does not fit in basic.
  function buildPacket(seq, type, data, ckType) {
    var cl = ckLen(ckType);
    var p = [MARK];
    var bodyLen = 2 + data.length + cl; // SEQ TYPE DATA CHECK
    if (bodyLen <= MAX_BASIC) {
      p.push(tochar(bodyLen), tochar(seq & 63), type.charCodeAt(0));
    } else {
      var lenx = data.length + cl;
      p.push(tochar(0), tochar(seq & 63), type.charCodeAt(0), tochar(Math.floor(lenx / 95)), tochar(lenx % 95));
      p.push(check1(p, 1, 6)[0]); // header check
    }
    for (var i = 0; i < data.length; i++) p.push(data[i]);
    var ck = blockCheck(ckType, p, 1, p.length);
    for (var k = 0; k < ck.length; k++) p.push(ck[k]);
    p.push(EOL);
    return new Uint8Array(p);
  }

  /* ---------- packet parse (stream) ---------- */

  // Feeds raw bytes, calls onPacket(pkt) for every complete, well-formed frame.
  // pkt.data(ckType) -> data byte array if the block check is valid, else null.
  // Resyncs on MARK: a frame cut short by a lost LoRa packet is dropped when the
  // next MARK (or any control character) shows up inside it.
  function createParser(onPacket) {
    var buf = [];

    function drop(n) { buf.splice(0, n); }

    function parse() {
      for (;;) {
        var m = buf.indexOf(MARK);
        if (m < 0) { buf.length = 0; return; }
        if (m > 0) drop(m);
        if (buf.length < 2) return;
        var len = unchar(buf[1]);
        var hdr, total;
        if (len === 0) {
          hdr = 7;
          if (buf.length < hdr) { if (badRange(1, buf.length)) continue; return; }
          if (badRange(1, hdr)) continue;
          if (check1(buf, 1, 6)[0] !== buf[6]) { drop(1); continue; }
          total = hdr + unchar(buf[4]) * 95 + unchar(buf[5]);
        } else if (len < 3 || len > MAX_BASIC) {
          drop(1);
          continue;
        } else {
          hdr = 4;
          total = 2 + len;
        }
        if (buf.length < total) { if (badRange(1, buf.length)) continue; return; }
        if (badRange(1, total)) continue;
        var raw = buf.slice(0, total);
        drop(total);
        emit(raw, hdr);
      }
    }

    // A control char inside a frame means it was cut: drop up to it and rescan.
    function badRange(from, to) {
      for (var i = from; i < to; i++) {
        if (buf[i] < 32 || buf[i] === 127) {
          drop(i);
          return true;
        }
      }
      return false;
    }

    function emit(raw, hdr) {
      onPacket({
        seq: unchar(raw[2]),
        type: String.fromCharCode(raw[3]),
        data: function (ckType) {
          var cl = ckLen(ckType);
          var end = raw.length - cl;
          if (end < hdr) return null;
          var ck = blockCheck(ckType, raw, 1, end);
          for (var i = 0; i < cl; i++) if (raw[end + i] !== ck[i]) return null;
          return raw.slice(hdr, end);
        },
      });
    }

    return {
      feed: function (bytes) {
        for (var i = 0; i < bytes.length; i++) buf.push(bytes[i]);
        parse();
      },
      clear: function () { buf.length = 0; },
    };
  }

  /* ---------- data encoding ---------- */

  // q: {qbin: 0 | prefix char, rept: 0 | prefix char}
  function encodeByte(b, q) {
    var out = [];
    if (q.qbin && b & 0x80) {
      out.push(q.qbin);
      b &= 0x7f;
    }
    var a7 = b & 0x7f;
    if (a7 < 32 || a7 === 127) out.push(QCTL, ctl(b));
    else if (a7 === QCTL || (q.qbin && a7 === q.qbin) || (q.rept && a7 === q.rept)) out.push(QCTL, b);
    else out.push(b);
    return out;
  }

  // Encodes from src[pos] until maxData bytes would be exceeded.
  // Returns {bytes, next}.
  function encodeChunk(src, pos, maxData, q) {
    var out = [];
    var i = pos;
    while (i < src.length) {
      var b = src[i];
      var run = 1;
      if (q.rept) while (i + run < src.length && src[i + run] === b && run < 94) run++;
      var unit = encodeByte(b, q);
      var piece;
      if (q.rept && run >= 3) piece = [q.rept, tochar(run)].concat(unit);
      else { run = 1; piece = unit; }
      if (out.length + piece.length > maxData) break;
      for (var k = 0; k < piece.length; k++) out.push(piece[k]);
      i += run;
    }
    return { bytes: out, next: i };
  }

  function decode(data, q) {
    var out = [];
    var i = 0;
    while (i < data.length) {
      var n = 1;
      var c = data[i++];
      if (q.rept && c === q.rept && i < data.length) {
        n = unchar(data[i++]);
        c = data[i++];
      }
      var high = 0;
      if (q.qbin && c === q.qbin && i < data.length) {
        high = 0x80;
        c = data[i++];
      }
      if (c === (q.qctl || QCTL) && i < data.length) {
        c = data[i++];
        var a7 = c & 0x7f;
        if ((a7 >= 64 && a7 <= 95) || a7 === 63) c = ctl(c);
      }
      c = (c | high) & 0xff;
      for (var k = 0; k < n; k++) out.push(c);
    }
    return out;
  }

  function strBytes(s) {
    var a = [];
    for (var i = 0; i < s.length; i++) a.push(s.charCodeAt(i) & 0xff);
    return a;
  }
  function bytesStr(a) {
    var s = "";
    for (var i = 0; i < a.length; i++) s += String.fromCharCode(a[i]);
    return s;
  }

  /* ---------- send-init negotiation ---------- */

  function initParams(timeoutMs) {
    var maxl = MAX_LONG;
    return [
      tochar(MAX_BASIC), // MAXL
      tochar(Math.min(94, Math.max(1, Math.ceil(timeoutMs / 1000)))), // TIME (s)
      tochar(0), // NPAD
      ctl(0), // PADC
      tochar(EOL), // EOL
      QCTL, // QCTL
      0x59, // QBIN 'Y': will 8th-bit prefix if asked
      0x33, // CHKT '3'
      REPT, // REPT
      tochar(CAPAS_LONG | CAPAS_ATTR), // CAPAS (no sliding windows)
      tochar(1), // WINDO
      tochar(Math.floor(maxl / 95)), // MAXLX1
      tochar(maxl % 95), // MAXLX2
    ];
  }

  // Combines our init with the peer's: what *we* may send and how both sides encode.
  function negotiate(peer) {
    function at(i) { return i < peer.length ? peer[i] : 32; }
    var p = {
      maxl: unchar(at(0)) || 80,
      time: unchar(at(1)) || 10,
      eol: unchar(at(4)) || EOL,
      qctl: at(5) === 32 ? QCTL : at(5),
      ckType: 1,
      qbin: 0,
      rept: 0,
      long: false,
      attr: false,
    };
    var qb = at(6);
    if ((qb >= 33 && qb <= 62) || (qb >= 96 && qb <= 126)) p.qbin = qb;
    var ck = at(7);
    if (ck === 0x33) p.ckType = 3;
    if (at(8) === REPT) p.rept = REPT;
    var capas = peer.length > 9 ? unchar(at(9)) : 0;
    // CAPAS can span several bytes (continuation bit 1); MAXLX follows WINDO after them.
    var ci = 9;
    while (ci < peer.length && unchar(peer[ci]) & 1) ci++;
    var after = ci + 1; // WINDO
    if (capas & CAPAS_LONG) {
      p.long = true;
      var x1 = peer.length > after + 1 ? unchar(peer[after + 1]) : -1;
      var x2 = peer.length > after + 2 ? unchar(peer[after + 2]) : -1;
      p.maxl = x1 >= 0 && x2 >= 0 ? x1 * 95 + x2 : 500;
    }
    if (capas & CAPAS_ATTR) p.attr = true;
    // QCTL is per direction: we always encode with '#', and decode with the peer's.
    if (p.qctl < 33 || p.qctl > 126) p.qctl = QCTL;
    return p;
  }

  // Max encoded data bytes per packet we send, given the peer's limit and our LoRa cap.
  function dataRoom(neg) {
    var cl = ckLen(neg.ckType);
    if (neg.long) {
      var lim = Math.min(neg.maxl, MAX_LONG);
      return Math.max(10, lim - 5 - cl); // LENX counts data + check; keep header slack
    }
    return Math.min(neg.maxl, MAX_BASIC) - 2 - cl;
  }

  /* ---------- common session plumbing ---------- */

  function session(opts) {
    var s = {
      write: opts.write,
      timeout: opts.timeout || DEFAULT_TIMEOUT_MS,
      done: false,
      timer: null,
      unsub: null,
      waiting: null, // function(pkt) or null
      onTimeout: null,
    };
    var parser = createParser(function (pkt) {
      if (!s.done && s.waiting) s.waiting(pkt);
    });
    s.parser = parser;
    s.listen = function () {
      var r = opts.onData(function (bytes) {
        if (!s.done) parser.feed(bytes);
      });
      s.unsub = typeof r === "function" ? r : null;
    };
    s.arm = function (onPkt, onTimeout) {
      s.waiting = onPkt;
      s.onTimeout = onTimeout;
      if (s.timer) clearTimeout(s.timer);
      s.timer = setTimeout(function () {
        s.timer = null;
        if (!s.done && s.onTimeout) s.onTimeout();
      }, s.timeout);
    };
    s.finish = function () {
      s.done = true;
      s.waiting = null;
      if (s.timer) clearTimeout(s.timer);
      s.timer = null;
      if (s.unsub) s.unsub();
      s.unsub = null;
    };
    return s;
  }

  /* ---------- sender ---------- */

  function createSender(options) {
    var file = options.file;
    var onProgress = options.onProgress || function () {};
    var onComplete = options.onComplete || function () {};
    var s = session(options);
    var data = file.data instanceof Uint8Array ? file.data : new Uint8Array(file.data || []);
    var name = String(file.name || "file").replace(/^.*[\\/]/, "") || "file";
    var neg = null;
    var seq = 0;

    function fail(msg, notifyPeer) {
      if (s.done) return;
      if (notifyPeer) s.write(buildPacket(seq, "E", strBytes(msg).slice(0, 80), neg ? neg.ckType : 1));
      s.finish();
      onComplete(false, msg);
    }

    // Sends one packet and waits for its ACK. onAck(ackData) runs on success.
    function exchange(type, payload, ckType, onAck) {
      var tries = 0;
      var pkt = buildPacket(seq, type, payload, ckType);
      function send() {
        if (s.done) return;
        if (++tries > MAX_RETRIES) {
          fail("No response from receiver (" + type + " packet)", true);
          return;
        }
        s.write(pkt);
        s.arm(onPacket, send);
      }
      function onPacket(p) {
        // Before negotiation (and for the ACK of S) checks are type 1.
        var ck = type === "S" ? 1 : ckType;
        var d = p.data(ck);
        if (d === null && p.type === "E") d = p.data(1);
        if (d === null) return; // corrupted: let the timeout resend
        if (p.type === "E") {
          fail("Receiver aborted: " + bytesStr(decode(d, neg || { qbin: 0, rept: 0 })), false);
          return;
        }
        if (p.type === "N") {
          // NAK of the next packet = ACK of this one.
          if (p.seq === ((seq + 1) & 63)) return ack([]);
          if (p.seq === seq) send();
          return;
        }
        if (p.type === "Y" && p.seq === seq) ack(d);
      }
      function ack(d) {
        if (s.timer) clearTimeout(s.timer);
        s.waiting = null;
        seq = (seq + 1) & 63;
        onAck(d);
      }
      send();
    }

    function sendInit() {
      exchange("S", initParams(s.timeout), 1, function (peer) {
        neg = negotiate(peer);
        sendFile();
      });
    }

    function sendFile() {
      exchange("F", encodeChunk(strBytes(name), 0, dataRoom(neg), neg).bytes, neg.ckType, function () {
        if (neg.attr) {
          var size = String(data.length);
          var attr = [0x31, tochar(size.length)].concat(strBytes(size)); // '1' = length in bytes
          exchange("A", attr, neg.ckType, function (d) {
            if (d.length && d[0] === 0x4e) fail("Receiver refused the file", true); // 'N'
            else sendData(0);
          });
        } else sendData(0);
      });
    }

    function sendData(pos) {
      if (pos >= data.length) return sendEof();
      var chunk = encodeChunk(data, pos, dataRoom(neg), neg);
      exchange("D", chunk.bytes, neg.ckType, function (d) {
        if (d.length && (d[0] === 0x58 || d[0] === 0x5a)) {
          fail("Receiver cancelled the transfer", false); // 'X' / 'Z'
          return;
        }
        onProgress(chunk.next, data.length);
        sendData(chunk.next);
      });
    }

    function sendEof() {
      exchange("Z", [], neg.ckType, function () {
        exchange("B", [], neg.ckType, function () {
          s.finish();
          onComplete(true, null);
        });
      });
    }

    return {
      start: function () {
        s.listen();
        sendInit();
      },
      cancel: function () {
        fail("Cancelled by sender", true);
      },
    };
  }

  /* ---------- receiver ---------- */

  function createReceiver(options) {
    var onFileStart = options.onFileStart || function () {};
    var onProgress = options.onProgress || function () {};
    var onComplete = options.onComplete || function () {};
    var s = session(options);
    var neg = null;
    var expected = 0;
    var lastAck = null; // packet bytes, re-sent for duplicates
    var tries = 0;
    var fileName = "";
    var fileSize = -1; // for progress (may be approximate, from '!' = K)
    var exactSize = -1; // from '1' (bytes): verified at the end
    var chunks = [];
    var received = 0;
    var gotEof = false;

    function ckType() { return neg ? neg.ckType : 1; }

    function fail(msg, notifyPeer) {
      if (s.done) return;
      if (notifyPeer) s.write(buildPacket(expected, "E", strBytes(msg).slice(0, 80), ckType()));
      s.finish();
      onComplete(false, null, msg);
    }

    function sendAck(seq, payload, ck) {
      lastAck = buildPacket(seq, "Y", payload || [], ck || ckType());
      s.write(lastAck);
    }

    function wait() {
      s.arm(onPacket, function () {
        if (++tries > MAX_RETRIES) {
          fail("No data from sender", true);
          return;
        }
        // Timeout. Before any ACK: NAK to prompt the S packet. After one: repeat our
        // last ACK, since the sender most likely never got it. (A NAK of the next
        // packet means the same per spec, but e.g. Tera Term ignores it and waits for
        // the ACK forever. Also the cheaper recovery when a LoRa frame was lost.)
        s.write(lastAck || buildPacket(expected, "N", [], ckType()));
        wait();
      });
    }

    function onPacket(p) {
      var isInit = p.type === "S";
      var d = p.data(isInit ? 1 : ckType());
      if (d === null && (p.type === "E" || isInit)) d = p.data(1);
      if (d === null) {
        s.write(buildPacket(expected, "N", [], ckType()));
        return;
      }
      if (p.type === "E") {
        fail("Sender aborted: " + bytesStr(decode(d, neg || { qbin: 0, rept: 0 })), false);
        return;
      }
      var prev = (expected - 1) & 63;
      if (p.seq === prev && lastAck && p.seq !== expected) {
        s.write(lastAck); // our ACK was lost: repeat it
        return;
      }
      if (p.seq !== expected) {
        s.write(buildPacket(expected, "N", [], ckType()));
        return;
      }
      tries = 0;
      switch (p.type) {
        case "S": {
          var ours = initParams(s.timeout);
          neg = negotiate(d);
          // ACK carries our parameters; check type 1 like the S itself.
          sendAck(p.seq, ours, 1);
          break;
        }
        case "F":
          if (!neg) return;
          fileName = bytesStr(decode(d, neg)) || "file";
          onFileStart(fileName, fileSize);
          sendAck(p.seq, encodeChunk(strBytes(fileName), 0, 60, neg).bytes);
          break;
        case "A":
          parseAttrs(d);
          onFileStart(fileName, fileSize);
          sendAck(p.seq, [0x59]); // 'Y': accept
          break;
        case "D": {
          var bytes = decode(d, neg);
          chunks.push(bytes);
          received += bytes.length;
          onProgress(received, fileSize);
          sendAck(p.seq, []);
          break;
        }
        case "Z":
          gotEof = true;
          sendAck(p.seq, []);
          if (d.length && d[0] === 0x44) { // 'D': sender discarded the file
            fail("Sender discarded the file", false);
            return;
          }
          break;
        case "B":
          sendAck(p.seq, []);
          finish();
          return;
        default:
          // Unknown packet type: refuse it so the sender stops.
          fail("Unsupported Kermit packet '" + p.type + "'", true);
          return;
      }
      expected = (expected + 1) & 63;
      wait();
    }

    function parseAttrs(d) {
      var i = 0;
      while (i + 1 < d.length) {
        var tag = d[i];
        var len = unchar(d[i + 1]);
        var val = bytesStr(d.slice(i + 2, i + 2 + len));
        if (tag === 0x31) exactSize = fileSize = parseInt(val, 10); // '1': bytes
        else if (tag === 0x21 && exactSize < 0) fileSize = parseInt(val, 10) * 1024; // '!': K
        i += 2 + len;
      }
      if (!isFinite(fileSize)) fileSize = -1;
      if (!isFinite(exactSize)) exactSize = -1;
    }

    function finish() {
      if (!gotEof) {
        fail("Transfer ended before end of file", false);
        return;
      }
      var out = new Uint8Array(received);
      var o = 0;
      for (var i = 0; i < chunks.length; i++) {
        out.set(chunks[i], o);
        o += chunks[i].length;
      }
      linger();
      if (exactSize >= 0 && out.length !== exactSize) {
        onComplete(false, null, "Size mismatch: got " + out.length + " of " + exactSize + " bytes");
        return;
      }
      onComplete(true, out, null, fileName);
    }

    // The transfer is over, but our ACK of B (or Z) may be lost on the radio link.
    // Keep answering repeats with the last ACK for a while, then let go of the port.
    function linger() {
      var prev = (expected - 1) & 63;
      s.waiting = null;
      if (s.timer) clearTimeout(s.timer);
      s.timer = null;
      var parser = createParser(function (p) {
        if (p.seq === prev || p.type === "B" || p.type === "Z") s.write(lastAck);
      });
      s.done = true;
      var unsub = s.unsub;
      s.unsub = null;
      var stop = options.onData(function (b) { parser.feed(b); });
      setTimeout(function () {
        if (typeof stop === "function") stop();
        if (unsub) unsub();
      }, 2 * s.timeout);
    }

    return {
      start: function () {
        s.listen();
        // Prompt the sender right away (Kermit receivers may NAK while waiting for S).
        s.write(buildPacket(0, "N", [], 1));
        wait();
      },
      cancel: function () {
        fail("Cancelled by receiver", true);
      },
    };
  }

  return {
    createSender: createSender,
    createReceiver: createReceiver,
    DEFAULT_TIMEOUT_MS: DEFAULT_TIMEOUT_MS,
    MAX_LONG: MAX_LONG,
    // exposed for tests
    buildPacket: buildPacket,
    createParser: createParser,
    encodeChunk: encodeChunk,
    decode: decode,
    crc16: crc16,
    check1: check1,
    negotiate: negotiate,
    initParams: initParams,
    tochar: tochar,
    unchar: unchar,
  };
})();
