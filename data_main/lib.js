/*
 * Project Voyager web UI - pure logic.
 *
 * No DOM, no jQuery, no network. Everything here runs in a browser and in the
 * JavaScriptCore shell used by the L3 tests (test/web/test_*.js).
 * Loaded by index.html before script.js; exposes a single global: VoyagerLib.
 */
var VoyagerLib = (function () {
  "use strict";

  /* ---------- constants (Contracts 1, 2, 3, 5) ---------- */

  var LORA_MAX_PAYLOAD = 251; // Contract 1: 255 FIFO bytes - 4-byte header
  var CHUNK_MIN = 1; // Contract 1: RAW_DATA is 1-200 chars
  var CHUNK_MAX = 200;
  var MAC_PLACEHOLDER = "XX:XX:XX:XX:XX:XX";
  var TX_RESULT_TIMEOUT_MS = 30000; // fallback send window when tx_started has no usable expected_ms
  var TX_QUEUE_TIMEOUT_MS = 600000; // safety timer while an item waits in the device queue (10 min)
  var TX_START_SLACK_MS = 5000; // added to tx_started.expected_ms
  var CHUNK_MAX_RETRIES = 2; // same chunk is retried up to 2 more times
  var USERNAME_MAX = 32;

  // Files GET /api/config?name= accepts (Contract 2).
  var CONFIG_FILES = [
    "lora_config.json",
    "long_lora_config.json",
    "short_lora_config.json",
    "config_list.json",
    "lora_serial.json",
    "wifi_config.json",
  ];
  var LORA_PRESET_FILES = [
    "lora_config.json",
    "long_lora_config.json",
    "short_lora_config.json",
  ];

  // SX127x bandwidth values (Hz).
  var LORA_BANDWIDTHS = [
    7800, 10400, 15600, 20800, 31250, 41700, 62500, 125000, 250000, 500000,
  ];
  // Contract 5 ranges.
  var LORA_LIMITS = {
    freq: [410000000, 525000000],
    TxPower: [2, 20],
    SpreadingFactor: [6, 12],
    CodingRate4: [5, 8],
    SyncWord: [0, 255],
  };

  /* ---------- small helpers ---------- */

  function toText(v) {
    if (v === null || v === undefined) return "";
    if (typeof v === "string") return v;
    if (typeof v === "number" || typeof v === "boolean") return String(v);
    try {
      return JSON.stringify(v);
    } catch (e) {
      return "";
    }
  }

  function isObject(v) {
    return v !== null && typeof v === "object" && !Array.isArray(v);
  }

  function safeJsonParse(text) {
    if (typeof text !== "string") return null;
    try {
      return JSON.parse(text);
    } catch (e) {
      return null;
    }
  }

  function isInt(n) {
    return typeof n === "number" && isFinite(n) && Math.floor(n) === n;
  }

  // Strict number parsing: "12" -> 12, "" / "1x" / null -> NaN.
  function toNumber(v) {
    if (typeof v === "number") return v;
    if (typeof v === "string" && v.trim() !== "") return Number(v);
    return NaN;
  }

  function clamp(n, lo, hi) {
    return Math.min(hi, Math.max(lo, n));
  }

  /* ---------- escaping ---------- */

  // For the rare case a value has to go into an HTML string. The UI itself uses
  // .text()/DOM nodes for remote text; this exists for defence in depth and tests.
  function escapeHtml(s) {
    return toText(s)
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;")
      .replace(/'/g, "&#39;");
  }

  /* ---------- UTF-8 ---------- */

  function utf8Encode(str) {
    str = toText(str);
    var out = [];
    for (var i = 0; i < str.length; i++) {
      var c = str.charCodeAt(i);
      if (c >= 0xd800 && c <= 0xdbff && i + 1 < str.length) {
        var d = str.charCodeAt(i + 1);
        if (d >= 0xdc00 && d <= 0xdfff) {
          c = 0x10000 + ((c - 0xd800) << 10) + (d - 0xdc00);
          i++;
        } else {
          c = 0xfffd;
        }
      } else if (c >= 0xd800 && c <= 0xdfff) {
        c = 0xfffd; // lone surrogate
      }
      if (c < 0x80) out.push(c);
      else if (c < 0x800) out.push(0xc0 | (c >> 6), 0x80 | (c & 63));
      else if (c < 0x10000)
        out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
      else
        out.push(
          0xf0 | (c >> 18),
          0x80 | ((c >> 12) & 63),
          0x80 | ((c >> 6) & 63),
          0x80 | (c & 63)
        );
    }
    return out;
  }

  function utf8ByteLength(str) {
    str = toText(str);
    var n = 0;
    for (var i = 0; i < str.length; i++) {
      var c = str.charCodeAt(i);
      if (c < 0x80) n += 1;
      else if (c < 0x800) n += 2;
      else if (c >= 0xd800 && c <= 0xdbff && i + 1 < str.length) {
        var d = str.charCodeAt(i + 1);
        if (d >= 0xdc00 && d <= 0xdfff) {
          n += 4;
          i++;
        } else n += 3;
      } else n += 3;
    }
    return n;
  }

  /* ---------- size formatting ---------- */

  function formatSize(bytes) {
    bytes = Number(bytes) || 0;
    if (bytes < 1024) return bytes + " Bytes";
    if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(2) + " KB";
    return (bytes / 1024 / 1024).toFixed(2) + " MB";
  }

  function get_string_size(str) {
    return formatSize(utf8ByteLength(str));
  }

  function formatDuration(totalSeconds) {
    var t = Math.max(0, Math.ceil(Number(totalSeconds) || 0));
    var h = Math.floor(t / 3600);
    var m = Math.floor((t % 3600) / 60);
    var s = t % 60;
    return h + ":" + (m < 10 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s;
  }

  // Rough lower bound: each chunk needs at least ~500 ms of air time + ACK.
  function estimateSeconds(totalPackets, waitMs) {
    var per = Math.max(Number(waitMs) || 0, 500) / 1000;
    return Math.max(0, totalPackets) * per;
  }

  /* ---------- chunking ---------- */

  function validateChunkSize(v) {
    var n = toNumber(v);
    return isInt(n) && n >= CHUNK_MIN && n <= CHUNK_MAX;
  }

  function totalPackets(length, chunkSize) {
    if (!(length > 0) || !(chunkSize > 0)) return 0;
    return Math.ceil(length / chunkSize);
  }

  function chunkString(str, chunkSize) {
    str = toText(str);
    var size = toNumber(chunkSize);
    if (!validateChunkSize(size)) throw new Error("chunk size must be 1-200");
    var out = [];
    for (var i = 0; i < str.length; i += size) out.push(str.slice(i, i + size));
    return out;
  }

  /* ---------- LoRa message building (Contract 1) ---------- */

  function wrapperName(username) {
    var u = toText(username);
    return u === "" ? MAC_PLACEHOLDER : u;
  }

  // Byte size of the LORA_MSG payload the device will build around `innerJson`.
  function wrappedPayloadSize(username, innerJson) {
    var wrapped = JSON.stringify({
      name: wrapperName(username),
      mac: MAC_PLACEHOLDER,
      data: toText(innerJson),
    });
    return utf8ByteLength(wrapped);
  }

  function checkPayload(username, innerJson) {
    var size = wrappedPayloadSize(username, innerJson);
    return { ok: size <= LORA_MAX_PAYLOAD, size: size, max: LORA_MAX_PAYLOAD };
  }

  function buildTextMessage(text) {
    return JSON.stringify({ pack_type: "msg", data: toText(text) });
  }

  function buildAction(action, extra) {
    var o = { pack_type: "action", data: action };
    if (extra) {
      for (var k in extra) {
        if (Object.prototype.hasOwnProperty.call(extra, k)) o[k] = extra[k];
      }
    }
    return JSON.stringify(o);
  }

  // enable_file_tx_mode; the file name is shortened (extension kept where
  // possible) until the wrapped payload fits. Returns null if nothing fits.
  function buildEnableFileTx(meta, username) {
    var name = toText(meta.name);
    var dot = name.lastIndexOf(".");
    var ext = dot > 0 && name.length - dot <= 10 ? name.slice(dot) : "";
    var base = ext ? name.slice(0, dot) : name;
    function build(n) {
      return buildAction("enable_file_tx_mode", {
        total_packets: meta.total_packets,
        size: toText(meta.size),
        name: n,
        time: toText(meta.time),
      });
    }
    var inner = build(name);
    if (checkPayload(username, inner).ok) return inner;
    for (var keep = base.length - 1; keep >= 0; keep--) {
      inner = build(base.slice(0, keep) + "~" + ext);
      if (checkPayload(username, inner).ok) return inner;
    }
    inner = build("~");
    return checkPayload(username, inner).ok ? inner : null;
  }

  function buildDisableFileTx(total, sha256) {
    return buildAction("disable_file_tx_mode", {
      total_packets: total,
      sha256: toText(sha256),
    });
  }

  /* ---------- message parsing (Contracts 1, 3, 4) ---------- */

  // Parse the raw LORA_MSG payload from WS lora_rx. Every field comes back as a
  // plain string/number; callers must still render with .text().
  function parseLoraRx(raw) {
    var outer = typeof raw === "string" ? safeJsonParse(raw) : raw;
    if (!isObject(outer)) return null;
    var inner =
      typeof outer.data === "string" ? safeJsonParse(outer.data) : outer.data;
    if (!isObject(inner)) return null;
    var r = {
      name: toText(outer.name),
      mac: toText(outer.mac),
      packType: toText(inner.pack_type),
      text: "",
      action: "",
      totalPackets: -1,
      size: "",
      fileName: "",
      time: "",
      sha256: "",
    };
    r.sender = r.name || r.mac || "unknown";
    if (r.packType === "msg") {
      r.text = toText(inner.data);
    } else if (r.packType === "action") {
      r.action = toText(inner.data);
      var tp = toNumber(inner.total_packets);
      r.totalPackets = isInt(tp) && tp >= 0 ? tp : -1;
      r.size = toText(inner.size);
      r.fileName = toText(inner.name);
      r.time = toText(inner.time);
      r.sha256 = toText(inner.sha256).toLowerCase();
    } else {
      return null;
    }
    return r;
  }

  function wifiQuality(rssi) {
    var n = toNumber(rssi);
    if (!isFinite(n)) return 0;
    return clamp(Math.round(2 * (n + 100)), 0, 100);
  }

  // Navbar label for a WIFI_DATA event (Contract 4):
  //   {"mode":"STA","rssi":-55} -> "90 %"      {"mode":"AP","clients":2} -> "AP, 2 clients"
  //   {"mode":"OFF"}            -> "off"       {"rssi":-55} (older firmware) -> "90 %"
  // Returns null for anything unusable, so the caller keeps the previous label.
  function wifiLabel(d) {
    if (!isObject(d)) return null;
    var mode = typeof d.mode === "string" ? d.mode.toUpperCase() : "";
    if (mode === "OFF") return "off";
    if (mode === "AP") {
      var c = toNumber(d.clients);
      if (!isFinite(c) || c < 0) return "AP";
      c = Math.floor(c);
      return "AP, " + c + (c === 1 ? " client" : " clients");
    }
    if (mode === "STA" || mode === "") {
      if (d.rssi === undefined || d.rssi === null || !isFinite(toNumber(d.rssi))) return null;
      return wifiQuality(d.rssi) + " %";
    }
    return null;
  }

  // Signal-bar indicator for a WIFI_DATA event: {mode: "sta"|"ap"|"off", level: 0-4},
  // or null for anything unusable (same rules as wifiLabel).
  function wifiBars(d) {
    if (wifiLabel(d) === null) return null;
    var mode = typeof d.mode === "string" ? d.mode.toUpperCase() : "";
    if (mode === "OFF") return { mode: "off", level: 0 };
    if (mode === "AP") return { mode: "ap", level: 4 };
    var q = wifiQuality(d.rssi);
    var level = q >= 75 ? 4 : q >= 50 ? 3 : q >= 25 ? 2 : q > 0 ? 1 : 0;
    return { mode: "sta", level: level };
  }

  function wifiQualityClass(q) {
    if (q >= 80) return "list-group-item-success";
    if (q >= 60) return "list-group-item-primary";
    if (q >= 40) return "list-group-item-warning";
    return "list-group-item-danger";
  }

  function parseWifiScan(msg) {
    var list = msg && Array.isArray(msg.SSID) ? msg.SSID : [];
    var out = [];
    for (var i = 0; i < list.length; i++) {
      if (!isObject(list[i])) continue;
      var q = wifiQuality(list[i].rssi);
      out.push({
        ssid: toText(list[i].ssid),
        rssi: toNumber(list[i].rssi),
        quality: q,
        cls: wifiQualityClass(q),
      });
    }
    return out;
  }

  function heapStatus(freeHeap, heapSize) {
    var free = toNumber(freeHeap);
    var size = toNumber(heapSize);
    if (!(size > 0) || !isFinite(free)) return null;
    var pct = clamp(Math.round(((size - free) / size) * 100), 0, 100);
    var cls = pct <= 30 ? "bg-success" : pct <= 70 ? "bg-warning" : "bg-danger";
    return { percent: pct, cls: cls, free: free, size: size };
  }

  // SSE DEBUG / RAW_DATA payload {"millis","data"}; data comes back as text.
  function parseSseData(text) {
    var o = safeJsonParse(text);
    if (!isObject(o)) return null;
    return { millis: o.millis, data: toText(o.data) };
  }

  /* ---------- validation ---------- */

  function validatePassword(p) {
    return typeof p === "string" && p.length >= 8 && p.length <= 63;
  }

  // WPA2 passphrase: 8-63 printable ASCII.
  function validateApPassword(p) {
    return typeof p === "string" && /^[\x20-\x7e]{8,63}$/.test(p);
  }

  // Empty means an open network.
  function validateWifiPassword(p) {
    return typeof p === "string" && (p.length === 0 || /^[\x20-\x7e]{8,63}$/.test(p));
  }

  function validateUsername(u) {
    return typeof u === "string" && u.length >= 1 && u.length <= USERNAME_MAX;
  }

  function isAllowedConfigName(name) {
    return CONFIG_FILES.indexOf(name) >= 0;
  }

  function isLoraPreset(name) {
    return LORA_PRESET_FILES.indexOf(name) >= 0;
  }

  function validateLoraConfig(cfg) {
    var errors = [];
    if (!isObject(cfg)) return { ok: false, errors: ["not an object"], config: null };
    var c = {
      freq: toNumber(cfg.freq),
      TxPower: toNumber(cfg.TxPower),
      SpreadingFactor: toNumber(cfg.SpreadingFactor),
      SignalBandwidth: toNumber(cfg.SignalBandwidth),
      CodingRate4: toNumber(cfg.CodingRate4),
      SyncWord: toNumber(cfg.SyncWord),
    };
    Object.keys(LORA_LIMITS).forEach(function (k) {
      var lim = LORA_LIMITS[k];
      if (!isInt(c[k]) || c[k] < lim[0] || c[k] > lim[1])
        errors.push(k + " must be an integer " + lim[0] + "-" + lim[1]);
    });
    if (LORA_BANDWIDTHS.indexOf(c.SignalBandwidth) < 0)
      errors.push("SignalBandwidth must be one of " + LORA_BANDWIDTHS.join(", "));
    return { ok: errors.length === 0, errors: errors, config: c };
  }

  // Form values (strings, freq in MHz) -> config object (freq in Hz).
  function loraConfigFromForm(v) {
    var mhz = parseFloat(v.freqMHz);
    return {
      freq: isFinite(mhz) ? Math.round(mhz * 1000000) : NaN,
      TxPower: toNumber(v.txPower),
      SpreadingFactor: toNumber(v.spreadingFactor),
      SignalBandwidth: toNumber(v.bandwidth),
      CodingRate4: toNumber(v.codingRate),
      SyncWord: toNumber(v.syncWord),
    };
  }

  /* ---------- SHA-256 (crypto.subtle is unavailable over plain http) ---------- */

  var K = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
  ];

  function ror(x, n) {
    return (x >>> n) | (x << (32 - n));
  }

  function sha256Bytes(bytes) {
    var H = [
      0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c,
      0x1f83d9ab, 0x5be0cd19,
    ];
    var len = bytes.length;
    var msg = Array.prototype.slice.call(bytes);
    msg.push(0x80);
    while (msg.length % 64 !== 56) msg.push(0);
    var hi = Math.floor(len / 0x20000000);
    var lo = (len * 8) >>> 0;
    msg.push(
      (hi >>> 24) & 255, (hi >>> 16) & 255, (hi >>> 8) & 255, hi & 255,
      (lo >>> 24) & 255, (lo >>> 16) & 255, (lo >>> 8) & 255, lo & 255
    );
    var W = new Array(64);
    for (var off = 0; off < msg.length; off += 64) {
      var t;
      for (t = 0; t < 16; t++) {
        var p = off + 4 * t;
        W[t] = (msg[p] << 24) | (msg[p + 1] << 16) | (msg[p + 2] << 8) | msg[p + 3];
      }
      for (t = 16; t < 64; t++) {
        var x = W[t - 15];
        var y = W[t - 2];
        var s0 = ror(x, 7) ^ ror(x, 18) ^ (x >>> 3);
        var s1 = ror(y, 17) ^ ror(y, 19) ^ (y >>> 10);
        W[t] = (W[t - 16] + s0 + W[t - 7] + s1) | 0;
      }
      var a = H[0], b = H[1], c = H[2], d = H[3];
      var e = H[4], f = H[5], g = H[6], h = H[7];
      for (t = 0; t < 64; t++) {
        var S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
        var ch = (e & f) ^ (~e & g);
        var t1 = (h + S1 + ch + K[t] + W[t]) | 0;
        var S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
        var maj = (a & b) ^ (a & c) ^ (b & c);
        var t2 = (S0 + maj) | 0;
        h = g;
        g = f;
        f = e;
        e = (d + t1) | 0;
        d = c;
        c = b;
        b = a;
        a = (t1 + t2) | 0;
      }
      H[0] = (H[0] + a) | 0;
      H[1] = (H[1] + b) | 0;
      H[2] = (H[2] + c) | 0;
      H[3] = (H[3] + d) | 0;
      H[4] = (H[4] + e) | 0;
      H[5] = (H[5] + f) | 0;
      H[6] = (H[6] + g) | 0;
      H[7] = (H[7] + h) | 0;
    }
    var hex = "";
    for (var i = 0; i < 8; i++) hex += ("00000000" + (H[i] >>> 0).toString(16)).slice(-8);
    return hex;
  }

  function sha256Hex(str) {
    return sha256Bytes(utf8Encode(str));
  }

  /* ---------- data URL decoding ---------- */

  var B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  function base64Decode(s) {
    s = toText(s).replace(/\s+/g, "");
    if (!/^[A-Za-z0-9+/]*={0,2}$/.test(s) || s.length % 4 === 1)
      throw new Error("invalid base64");
    s = s.replace(/=+$/, "");
    var out = new Uint8Array(Math.floor((s.length * 3) / 4));
    var buf = 0, bits = 0, j = 0;
    for (var i = 0; i < s.length; i++) {
      buf = (buf << 6) | B64.indexOf(s.charAt(i));
      bits += 6;
      if (bits >= 8) {
        bits -= 8;
        out[j++] = (buf >> bits) & 255;
      }
    }
    return out;
  }

  // "data:<mime>[;base64],<body>" -> { mime, bytes: Uint8Array }. Throws if malformed.
  function dataURIToBytes(uri) {
    uri = toText(uri);
    var comma = uri.indexOf(",");
    if (uri.slice(0, 5) !== "data:" || comma < 0) throw new Error("not a data URL");
    var header = uri.slice(5, comma);
    var body = uri.slice(comma + 1);
    var mime = header.split(";")[0] || "application/octet-stream";
    var bytes = /;base64$/i.test(header)
      ? base64Decode(body)
      : new Uint8Array(utf8Encode(decodeURIComponent(body)));
    return { mime: mime, bytes: bytes };
  }

  /* ---------- TX result tracking (Contract 2: 202 {id}, Contract 3: tx_result) ---------- */

  function normalizeTxResult(msg) {
    return {
      id: msg.id,
      akn: toNumber(msg.akn) === 1 ? 1 : 0,
      username: toText(msg.username),
      timeout: false,
    };
  }

  // How long to wait for tx_result after tx_started: expected_ms + slack, or the
  // 30 s fallback when expected_ms is missing, non-numeric or negative.
  // (expected_ms === 0 is handled by the caller: it keeps the queue safety timer.)
  function txSendingWindowMs(expectedMs, slackMs) {
    var e = toNumber(expectedMs);
    if (!isFinite(e) || e < 0) e = TX_RESULT_TIMEOUT_MS;
    return Math.min(e, TX_QUEUE_TIMEOUT_MS) + slackMs;
  }

  // Bounded id -> message map for events that arrive before the 202 is processed.
  function createEarlyMap(max) {
    var map = {};
    var order = [];
    return {
      put: function (key, msg) {
        if (!Object.prototype.hasOwnProperty.call(map, key)) order.push(key);
        map[key] = msg;
        while (order.length > max) delete map[order.shift()];
      },
      take: function (key) {
        if (!Object.prototype.hasOwnProperty.call(map, key)) return null;
        var m = map[key];
        delete map[key];
        var i = order.indexOf(key);
        if (i >= 0) order.splice(i, 1);
        return m;
      },
    };
  }

  /*
   * Tracks queued radio transmissions by id (Contract 2: 202 {id[,queued]};
   * Contract 3: tx_started {id, expected_ms, attempts}, then tx_result {id, akn, username}).
   *
   * opts: setTimeout, clearTimeout,
   *   timeoutMs   safety timer while "queued" (default TX_QUEUE_TIMEOUT_MS, 10 min)
   *   slackMs     added to expected_ms once "sending" (default TX_START_SLACK_MS)
   *
   * register(id, cb, onState):
   *   cb(result) exactly once: {id, akn, username, timeout, phase}
   *     (phase = "queued" | "sending": the state the item was in when it timed out)
   *   onState(state, info) (optional): "queued" right away, then "sending" with
   *     {expectedMs, attempts} when tx_started arrives.
   * started(msg) / resolve(msg): feed tx_started / tx_result; either may arrive
   *   before register() and is kept until then. Return true when matched.
   */
  function createTxTracker(opts) {
    var setT = opts.setTimeout;
    var clearT = opts.clearTimeout;
    var queuedMs = opts.timeoutMs || TX_QUEUE_TIMEOUT_MS;
    var slackMs = opts.slackMs === undefined ? TX_START_SLACK_MS : opts.slackMs;
    var pending = {};
    var earlyResult = createEarlyMap(50); // tx_result that arrived before the 202 was processed
    var earlyStarted = createEarlyMap(50); // tx_started that arrived before the 202 was processed

    function validId(msg) {
      return isObject(msg) && msg.id !== undefined && msg.id !== null;
    }

    function arm(key, p, ms) {
      if (p.timer !== null) clearT(p.timer);
      p.timer = setT(function () {
        if (pending[key] === p) {
          delete pending[key];
          p.cb({ id: p.id, akn: 0, username: "", timeout: true, phase: p.state });
        }
      }, ms);
    }

    function markStarted(key, p, msg) {
      p.state = "sending";
      // expected_ms === 0 means "time unknown" (Contract 3): keep the queue safety
      // timer that is already running instead of the 30 s fallback.
      if (toNumber(msg.expected_ms) === 0) {
        if (p.timer === null) arm(key, p, queuedMs);
      } else {
        arm(key, p, txSendingWindowMs(msg.expected_ms, slackMs));
      }
      if (p.onState) {
        var a = toNumber(msg.attempts);
        var e = toNumber(msg.expected_ms);
        p.onState("sending", {
          expectedMs: isFinite(e) && e >= 0 ? e : null,
          attempts: isFinite(a) && a > 0 ? Math.floor(a) : null,
        });
      }
    }

    return {
      register: function (id, cb, onState) {
        var key = String(id);
        var r = earlyResult.take(key);
        if (r) {
          earlyStarted.take(key);
          cb(normalizeTxResult(r));
          return;
        }
        var p = { id: id, cb: cb, onState: onState || null, state: "queued", timer: null };
        pending[key] = p;
        if (p.onState) p.onState("queued", {});
        var s = earlyStarted.take(key);
        if (s) markStarted(key, p, s);
        else arm(key, p, queuedMs);
      },
      started: function (msg) {
        if (!validId(msg)) return false;
        var key = String(msg.id);
        var p = pending[key];
        if (!p) {
          earlyStarted.put(key, msg);
          return false;
        }
        markStarted(key, p, msg);
        return true;
      },
      resolve: function (msg) {
        if (!validId(msg)) return false;
        var key = String(msg.id);
        var p = pending[key];
        if (!p) {
          earlyResult.put(key, msg);
          return false;
        }
        clearT(p.timer);
        delete pending[key];
        p.cb(normalizeTxResult(msg));
        return true;
      },
      state: function (id) {
        var p = pending[String(id)];
        return p ? p.state : null;
      },
      pendingCount: function () {
        return Object.keys(pending).length;
      },
    };
  }

  /* ---------- file sender: one chunk at a time, retry the same chunk ---------- */

  /*
   * opts:
   *   chunks      array of strings
   *   send(chunk, index, done)  done({ok:bool, fatal:bool, reason:string})
   *   schedule(fn, ms)
   *   waitMs      delay between chunks
   *   retryDelayMs delay before a retry
   *   maxRetries  extra attempts per chunk (default 2)
   *   isCancelled() -> bool
   *   onProgress(sent, total)
   *   onFinish({ok, sent, total, reason})
   */
  function createChunkSender(opts) {
    var chunks = opts.chunks;
    var total = chunks.length;
    var maxRetries = opts.maxRetries === undefined ? CHUNK_MAX_RETRIES : opts.maxRetries;
    var finished = false;
    var attempts = 0;

    function finish(ok, sent, reason) {
      if (finished) return;
      finished = true;
      opts.onFinish({ ok: ok, sent: sent, total: total, reason: reason || "" });
    }

    function attempt(index, tries) {
      if (opts.isCancelled && opts.isCancelled()) return finish(false, index, "cancelled");
      attempts++;
      opts.send(chunks[index], index, function (res) {
        if (finished) return;
        if (res && res.ok) {
          var sent = index + 1;
          if (opts.onProgress) opts.onProgress(sent, total);
          if (sent >= total) return finish(true, sent, "");
          opts.schedule(function () {
            attempt(sent, 0);
          }, opts.waitMs || 0);
        } else if (res && res.fatal) {
          finish(false, index, res.reason || "failed");
        } else if (tries < maxRetries) {
          opts.schedule(function () {
            attempt(index, tries + 1);
          }, opts.retryDelayMs || 0);
        } else {
          finish(false, index, (res && res.reason) || "no acknowledgement");
        }
      });
    }

    return {
      start: function () {
        if (total === 0) return finish(true, 0, "");
        attempt(0, 0);
      },
      attempts: function () {
        return attempts;
      },
    };
  }

  /* ---------- file receiver: count + SHA-256 verification ---------- */

  function createFileReceiver() {
    var st = { active: false, meta: null, data: "", count: 0, result: null };
    return {
      start: function (meta) {
        st.active = true;
        st.meta = meta || {};
        st.data = "";
        st.count = 0;
        st.result = null;
      },
      isActive: function () {
        return st.active;
      },
      addChunk: function (chunk) {
        if (!st.active || typeof chunk !== "string" || chunk === "") return false;
        st.data += chunk;
        st.count++;
        return true;
      },
      progress: function () {
        var total = st.meta && st.meta.totalPackets > 0 ? st.meta.totalPackets : 0;
        return {
          received: st.count,
          total: total,
          percent: total > 0 ? clamp((st.count / total) * 100, 0, 100) : 0,
          sizeText: get_string_size(st.data),
        };
      },
      finish: function (info) {
        if (!st.active) return null;
        st.active = false;
        var expected = info ? info.totalPackets : -1;
        var expectedHash = info ? toText(info.sha256).toLowerCase() : "";
        var countOk = expected >= 0 && st.count === expected;
        var hash = sha256Hex(st.data);
        var hashOk = /^[0-9a-f]{64}$/.test(expectedHash) && hash === expectedHash;
        st.result = {
          verified: countOk && hashOk,
          countOk: countOk,
          hashOk: hashOk,
          received: st.count,
          expected: expected,
          sha256: hash,
          reason: !countOk
            ? "received " + st.count + " of " + expected + " packets"
            : !hashOk
            ? "SHA-256 mismatch"
            : "",
        };
        return st.result;
      },
      cancel: function () {
        if (!st.active) return false;
        st.active = false;
        st.result = { verified: false, reason: "cancelled" };
        return true;
      },
      result: function () {
        return st.result;
      },
      canDownload: function () {
        return !!(st.result && st.result.verified && st.data.length > 0);
      },
      data: function () {
        return st.data;
      },
      fileName: function () {
        return st.meta && st.meta.fileName ? st.meta.fileName : "";
      },
    };
  }

  return {
    LORA_MAX_PAYLOAD: LORA_MAX_PAYLOAD,
    CHUNK_MIN: CHUNK_MIN,
    CHUNK_MAX: CHUNK_MAX,
    MAC_PLACEHOLDER: MAC_PLACEHOLDER,
    TX_RESULT_TIMEOUT_MS: TX_RESULT_TIMEOUT_MS,
    TX_QUEUE_TIMEOUT_MS: TX_QUEUE_TIMEOUT_MS,
    TX_START_SLACK_MS: TX_START_SLACK_MS,
    CHUNK_MAX_RETRIES: CHUNK_MAX_RETRIES,
    LORA_BANDWIDTHS: LORA_BANDWIDTHS,
    LORA_LIMITS: LORA_LIMITS,
    toText: toText,
    safeJsonParse: safeJsonParse,
    escapeHtml: escapeHtml,
    utf8Encode: utf8Encode,
    utf8ByteLength: utf8ByteLength,
    formatSize: formatSize,
    get_string_size: get_string_size,
    formatDuration: formatDuration,
    estimateSeconds: estimateSeconds,
    validateChunkSize: validateChunkSize,
    totalPackets: totalPackets,
    chunkString: chunkString,
    wrappedPayloadSize: wrappedPayloadSize,
    checkPayload: checkPayload,
    buildTextMessage: buildTextMessage,
    buildAction: buildAction,
    buildEnableFileTx: buildEnableFileTx,
    buildDisableFileTx: buildDisableFileTx,
    parseLoraRx: parseLoraRx,
    parseWifiScan: parseWifiScan,
    wifiQuality: wifiQuality,
    wifiLabel: wifiLabel,
    wifiBars: wifiBars,
    wifiQualityClass: wifiQualityClass,
    heapStatus: heapStatus,
    parseSseData: parseSseData,
    validatePassword: validatePassword,
    validateApPassword: validateApPassword,
    validateWifiPassword: validateWifiPassword,
    validateUsername: validateUsername,
    isAllowedConfigName: isAllowedConfigName,
    isLoraPreset: isLoraPreset,
    validateLoraConfig: validateLoraConfig,
    loraConfigFromForm: loraConfigFromForm,
    sha256Bytes: sha256Bytes,
    sha256Hex: sha256Hex,
    base64Decode: base64Decode,
    dataURIToBytes: dataURIToBytes,
    createTxTracker: createTxTracker,
    createChunkSender: createChunkSender,
    createFileReceiver: createFileReceiver,
  };
})();
