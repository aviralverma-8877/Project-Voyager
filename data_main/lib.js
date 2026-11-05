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
  var MAC_PLACEHOLDER = "XX:XX:XX:XX:XX:XX";
  var TX_RESULT_TIMEOUT_MS = 30000; // fallback send window when tx_started has no usable expected_ms
  var TX_QUEUE_TIMEOUT_MS = 600000; // safety timer while an item waits in the device queue (10 min)
  var TX_START_SLACK_MS = 5000; // added to tx_started.expected_ms
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

  // Serial (Kermit) transfer handshake, sent as LORA_MSG actions through /lora_transmit:
  //   sender   -> all : file_tx_invite {tx_id, name, size}
  //   receiver -> all : file_tx_join   {tx_id, nonce}     (nonce: receiver's random id)
  //   sender   -> all : file_tx_go     {tx_id, nonce}     (only that receiver starts)
  // The transfer is point-to-point, so exactly one receiver is picked. The name
  // here is for display only (the Kermit F packet carries the real one) and is clipped.
  var FILE_TX_NAME_MAX = 64;
  function buildFileTxInvite(txId, name, size) {
    return buildAction("file_tx_invite", {
      tx_id: toText(txId),
      name: toText(name).slice(0, FILE_TX_NAME_MAX),
      size: toText(size),
    });
  }
  function buildFileTxJoin(txId, nonce) {
    return buildAction("file_tx_join", { tx_id: toText(txId), nonce: toText(nonce) });
  }
  function buildFileTxGo(txId, nonce) {
    return buildAction("file_tx_go", { tx_id: toText(txId), nonce: toText(nonce) });
  }

  // SX127x time on air (Semtech AN1200.13): explicit header, CRC on, 8-symbol preamble,
  // low-data-rate optimisation when a symbol lasts 16 ms or more.
  function loraAirtimeMs(sf, bw, cr4, payloadLen) {
    sf = toNumber(sf); bw = toNumber(bw); cr4 = toNumber(cr4);
    if (!(sf >= 6 && sf <= 12) || !(bw > 0) || !(cr4 >= 5 && cr4 <= 8)) return NaN;
    var tSym = Math.pow(2, sf) / bw * 1000;
    var de = tSym >= 16 ? 1 : 0;
    var num = 8 * payloadLen - 4 * sf + 28 + 16;
    var nPayload = 8 + Math.max(Math.ceil(num / (4 * (sf - 2 * de))) * cr4, 0); // cr4 = CR+4
    return (8 + 4.25) * tSym + nPayload * tSym;
  }

  // Kermit wait for one packet's answer over the LoRa serial bridge: a packet is one
  // full LORA_SERIAL frame, its ACK one short frame back. Three times that plus slack
  // for the USB/bridge hops, never under 5 s. Unknown config -> 10 s.
  function kermitTimeoutMs(cfg) {
    if (!isObject(cfg)) return 10000;
    var full = loraAirtimeMs(cfg.SpreadingFactor, cfg.SignalBandwidth, cfg.CodingRate4, 255);
    var ack = loraAirtimeMs(cfg.SpreadingFactor, cfg.SignalBandwidth, cfg.CodingRate4, 20);
    if (!isFinite(full) || !isFinite(ack)) return 10000;
    return Math.max(5000, Math.ceil(3 * (full + ack) + 2000));
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
      size: "",
      fileName: "",
      txId: "",
      nonce: "",
    };
    r.sender = r.name || r.mac || "unknown";
    if (r.packType === "msg") {
      r.text = toText(inner.data);
    } else if (r.packType === "action") {
      r.action = toText(inner.data);
      r.size = toText(inner.size);
      r.fileName = toText(inner.name);
      r.txId = toText(inner.tx_id);
      r.nonce = toText(inner.nonce);
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

  // net_status (get_net_status): {"active","node","ip","tx","rx","drop","peers":[{"ip","rssi","age_s"}]}
  // Strings are passed through as text, numbers clamped; null if unusable.
  function parseNetStatus(d) {
    if (!isObject(d) || typeof d.ip !== "string") return null;
    function n(v) { var x = toNumber(v); return isFinite(x) && x >= 0 ? Math.floor(x) : 0; }
    var peers = Array.isArray(d.peers) ? d.peers : [];
    return {
      active: d.active === true,
      ip: toText(d.ip),
      tx: n(d.tx), rx: n(d.rx), drop: n(d.drop),
      peers: peers.filter(isObject).slice(0, 16).map(function (p) {
        var r = toNumber(p.rssi);
        return { ip: toText(p.ip), rssi: isFinite(r) ? Math.round(r) : 0, age: n(p.age_s) };
      }),
    };
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

  return {
    LORA_MAX_PAYLOAD: LORA_MAX_PAYLOAD,
    MAC_PLACEHOLDER: MAC_PLACEHOLDER,
    TX_RESULT_TIMEOUT_MS: TX_RESULT_TIMEOUT_MS,
    TX_QUEUE_TIMEOUT_MS: TX_QUEUE_TIMEOUT_MS,
    TX_START_SLACK_MS: TX_START_SLACK_MS,
    LORA_BANDWIDTHS: LORA_BANDWIDTHS,
    LORA_LIMITS: LORA_LIMITS,
    toText: toText,
    safeJsonParse: safeJsonParse,
    escapeHtml: escapeHtml,
    utf8Encode: utf8Encode,
    utf8ByteLength: utf8ByteLength,
    formatSize: formatSize,
    wrappedPayloadSize: wrappedPayloadSize,
    checkPayload: checkPayload,
    buildTextMessage: buildTextMessage,
    buildAction: buildAction,
    buildFileTxInvite: buildFileTxInvite,
    buildFileTxJoin: buildFileTxJoin,
    buildFileTxGo: buildFileTxGo,
    loraAirtimeMs: loraAirtimeMs,
    kermitTimeoutMs: kermitTimeoutMs,
    parseLoraRx: parseLoraRx,
    parseNetStatus: parseNetStatus,
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
    createTxTracker: createTxTracker,
  };
})();
