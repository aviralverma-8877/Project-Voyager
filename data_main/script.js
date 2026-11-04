/*
 * Project Voyager web UI - DOM, network and socket wiring.
 * Pure logic lives in lib.js (VoyagerLib), which index.html loads first.
 *
 * Rule: anything that came from the device or over the radio (names, messages,
 * SSIDs, file names, alerts, debug lines) goes into the DOM with .text(), .val()
 * or DOM nodes. Never concatenate it into HTML.
 */
"use strict";

const L = VoyagerLib;
const AJAX_TIMEOUT_MS = 10000;
const DEBUG_MAX_CHARS = 20000;

const state = {
  socket: null,
  events: null,
  hostnameUrl: "",
  username: "",
  serialMode: false,
  sending: false,
  cancelSend: false,
  setupMode: false,
  signInRequired: false,
  sessionRequest: null,
  connectedOnce: false,
  lastRawMillis: null,
  lastDebugMillis: null,
  scanTimer: null,
};

let promptModal = null;
let alertModal = null;
let loadingModal = null;

const receiver = L.createFileReceiver();
const txTracker = L.createTxTracker({
  setTimeout: (fn, ms) => window.setTimeout(fn, ms),
  clearTimeout: (t) => window.clearTimeout(t),
  timeoutMs: L.TX_RESULT_TIMEOUT_MS,
});

$.ajaxSetup({ timeout: AJAX_TIMEOUT_MS });

/* ------------------------------------------------------------------ */
/* Boot                                                                */
/* ------------------------------------------------------------------ */

$(document).ready(function () {
  promptModal = new bootstrap.Modal($("#promptModal")[0], {});
  alertModal = new bootstrap.Modal($("#alertModal")[0], {});
  loadingModal = new bootstrap.Modal($("#loadingModal")[0], {
    backdrop: "static",
    keyboard: false,
  });
  // A dismissed prompt must not leave a stale Proceed handler behind.
  $("#promptModal").on("hidden.bs.modal", function () {
    $("#promptModelProceed").off("click");
  });
  $("#setup_form").on("submit", submit_setup);
  $("#download_btn").on("click", downloadFile);
  $("#cancel_receive_btn").on("click", cancel_receive);

  // 403 {"error":"setup_required"} from any endpoint -> set-password screen.
  $(document).ajaxError(function (_event, xhr) {
    if (xhr && xhr.status === 403) {
      const body = L.safeJsonParse(xhr.responseText);
      if (body && body.error === "setup_required") show_setup_screen();
    }
  });

  get_hostname();
  check_auth();
});

function check_auth() {
  set_loading("Connecting...");
  $.ajax({ url: "/api/auth/status", dataType: "json", timeout: AJAX_TIMEOUT_MS })
    .done(function (s) {
      if (s && s.set === false) show_setup_screen();
      else if (s && s.set === true) start_session();
      else boot_error("The device sent an unexpected auth status.");
    })
    .fail(function (xhr) {
      boot_error(
        "Could not reach the device (" + describe_xhr(xhr) + ").\nReload the page to retry."
      );
    });
}

// GET /api/session (Contract 2) brings up the browser's Digest prompt and makes
// the device set the HttpOnly vsid cookie that authenticates WS and SSE.
// WS/SSE are opened only after it succeeds.
function start_session() {
  refresh_session()
    .done(function () {
      init_socket();
      init_events();
    })
    .fail(function (xhr) {
      if (state.setupMode || xhr.status === 401) return; // handled in refresh_session
      boot_error(
        "Could not start a session with the device (" + describe_xhr(xhr) + ").\nReload the page to retry."
      );
    });
}

// One /api/session request at a time; concurrent callers share it.
function refresh_session() {
  if (state.sessionRequest) return state.sessionRequest;
  const req = $.ajax({
    url: "/api/session",
    dataType: "json",
    timeout: AJAX_TIMEOUT_MS,
    xhrFields: { withCredentials: true },
  });
  state.sessionRequest = req;
  req
    .always(function () {
      state.sessionRequest = null;
    })
    .fail(function (xhr) {
      if (xhr.status === 401 && !state.setupMode) {
        state.signInRequired = true;
        boot_error("Sign-in is required. Reload the page and enter the admin password.");
      }
    });
  return req;
}

function boot_error(msg) {
  hide_loading();
  $("#main_content").empty().append($("<p>").addClass("text-danger pre-line").text(msg));
}

function get_hostname() {
  $.ajax({ url: "/hostname", dataType: "text", timeout: AJAX_TIMEOUT_MS }).done(function (name) {
    name = String(name || "").trim();
    if (/^[A-Za-z0-9-]{1,63}$/.test(name)) {
      state.hostnameUrl = "http://" + name + ".local/";
      $("#project_title").attr("href", state.hostnameUrl);
    }
  });
}

function redirect_home() {
  window.location.replace(state.hostnameUrl || "/");
}

/* ------------------------------------------------------------------ */
/* First-boot password setup / admin password change (Contract 2)      */
/* ------------------------------------------------------------------ */

function show_setup_screen() {
  if (state.setupMode) return;
  state.setupMode = true;
  if (state.socket) state.socket.close();
  if (state.events) {
    state.events.close();
    state.events = null;
  }
  hide_loading();
  $("#main_nav, #main_content, #status_panel").prop("hidden", true);
  $("#setup_screen").prop("hidden", false);
  $("#setup_pass1").trigger("focus");
}

function submit_setup(ev) {
  ev.preventDefault();
  const p1 = $("#setup_pass1").val() || "";
  const p2 = $("#setup_pass2").val() || "";
  const err = $("#setup_error");
  if (!L.validatePassword(p1)) {
    err.text("The password must be 8-63 characters.");
    return;
  }
  if (p1 !== p2) {
    err.text("The passwords don't match.");
    return;
  }
  err.text("");
  $("#setup_submit").prop("disabled", true);
  $.ajax({
    url: "/api/auth/setup",
    method: "POST",
    data: { password: p1 },
    dataType: "text",
    timeout: AJAX_TIMEOUT_MS,
  })
    .done(function () {
      window.location.reload();
    })
    .fail(function (xhr) {
      $("#setup_submit").prop("disabled", false);
      if (xhr.status === 409) window.location.reload(); // already set elsewhere
      else if (xhr.status === 400) err.text("The device rejected the password (8-63 characters).");
      else err.text("Could not set the password (" + describe_xhr(xhr) + ").");
    });
}

function change_admin_password() {
  const p1 = $("#admin_pass1").val() || "";
  const p2 = $("#admin_pass2").val() || "";
  if (!L.validatePassword(p1)) {
    alertText("The admin password must be 8-63 characters.");
    return;
  }
  if (p1 !== p2) {
    alertText("The passwords don't match.");
    return;
  }
  $.ajax({
    url: "/api/auth/change",
    method: "POST",
    data: { password: p1 },
    dataType: "text",
    timeout: AJAX_TIMEOUT_MS,
  })
    .done(function () {
      $("#admin_pass1, #admin_pass2").val("");
      alertText(
        "Admin password changed.\nYour browser will ask you to sign in again with the new password."
      );
    })
    .fail(function (xhr) {
      if (xhr.status === 400) alertText("The device rejected the password (8-63 characters).");
      else if (xhr.status === 401) alertText("Not signed in. Reload the page and sign in first.");
      else alertText("Could not change the password (" + describe_xhr(xhr) + ").");
    });
}

/* ------------------------------------------------------------------ */
/* Small UI helpers                                                    */
/* ------------------------------------------------------------------ */

// Plain text only: safe for anything that came from the device or the radio.
function alertText(msg) {
  $("#alert_body").text(L.toText(msg));
  alertModal.show();
}

// Fixed, trusted strings that need markup such as <br>. Never pass remote text.
function alertHtml(html) {
  $("#alert_body").html(html);
  alertModal.show();
}

function confirm_prompt(title, body, onProceed) {
  $("#promptModalLabel").text(title);
  $("#prompt_body").text(body);
  $("#promptModelProceed")
    .off("click")
    .one("click", function () {
      promptModal.hide();
      onProceed();
    });
  promptModal.show();
}

function set_loading(msg) {
  $("#loading_model_body").text(msg);
  loadingModal.show();
}

function hide_loading() {
  // Bootstrap ignores hide() while the show transition is still running.
  loadingModal.hide();
  setTimeout(function () {
    loadingModal.hide();
  }, 600);
}

function render_kv(selector, rows) {
  const box = $(selector).empty();
  rows.forEach(function (row, i) {
    if (i > 0) box.append($("<br>"));
    box.append(document.createTextNode(row[0] + ": "));
    box.append($("<b>").text(L.toText(row[1])));
  });
}

function set_progress(percent, cls) {
  const bar = $("#file_upload_progress_bar");
  const p = Math.max(0, Math.min(100, Number(percent) || 0));
  bar.css("width", p + "%").attr("aria-valuenow", Math.round(p));
  if (cls) bar.removeClass("bg-success bg-info bg-danger").addClass(cls);
}

function reset_progress_bar() {
  if (!state.sending && !receiver.isActive()) set_progress(0);
}

function describe_xhr(xhr) {
  if (!xhr) return "unknown error";
  if (xhr.statusText === "timeout") return "timed out";
  if (xhr.status === 0) return "device unreachable";
  return "HTTP " + xhr.status;
}

function get_config(name) {
  if (!L.isAllowedConfigName(name)) return $.Deferred().reject({ status: 0 }).promise();
  return $.ajax({
    url: "/api/config",
    data: { name: name },
    dataType: "json",
    timeout: AJAX_TIMEOUT_MS,
  });
}

// Locks:
//  .lora_transmission (dashboard messaging): serial mode or sending a file
//  .file_tx_controls  (file transfer form):  the above, or receiving a file
//  .busy_lock         (WiFi / LoRa settings): sending or receiving a file
function refresh_locks() {
  const receiving = receiver.isActive();
  const lockMsg = state.serialMode || state.sending;
  const lockFile = lockMsg || receiving;
  $(".lora_transmission").prop("disabled", lockMsg);
  $(".file_tx_controls").prop("disabled", lockFile);
  $(".busy_lock").prop("disabled", state.sending || receiving);
  $("#fileBroadcastButton").prop("disabled", lockFile);
  $("#stopBroadcastButton").prop("disabled", !state.sending);
  $("#cancel_receive_btn").prop("hidden", !receiving);
  $("#download_btn").prop("disabled", !receiver.canDownload());
}

/* ------------------------------------------------------------------ */
/* Pages                                                               */
/* ------------------------------------------------------------------ */

function load_page(url, navId, after) {
  $(".nav-link").removeClass("active");
  if (navId) $(navId).addClass("active");
  $.ajax({ url: url, dataType: "html", timeout: AJAX_TIMEOUT_MS })
    .done(function (html) {
      // Static page fragments shipped in LittleFS (or the device's OTA form).
      $("#main_content").html(html);
      refresh_locks();
      if (after) after();
    })
    .fail(function (xhr) {
      if (state.setupMode) return;
      $("#main_content")
        .empty()
        .append($("<p>").addClass("text-danger").text("Could not load the page (" + describe_xhr(xhr) + ")."));
    });
}

function dashboard() {
  load_page("dashboard.html", "#navbar-dashboard", function () {
    $("#username").val(state.username);
    $("#lora_to_serial").prop("checked", state.serialMode);
    get_config("lora_serial.json").done(function (cfg) {
      state.serialMode = !!(cfg && cfg.lora_serial);
      $("#lora_to_serial").prop("checked", state.serialMode);
      refresh_locks();
    });
    ws_send({ "request-type": "get_username" }, true);
  });
}

function file_transfer() {
  load_page("file_transfer.html", "#navbar-file-transfer", null);
}

function wifi() {
  load_page("wifi.html", "#navbar-wifi", function () {
    get_config("wifi_config.json").done(function (cfg) {
      const ssid = L.toText(cfg && cfg.wifi_ssid);
      const mode = L.toText(cfg && cfg.wifi_function);
      $("#wifi_ssid_name").text(ssid + (mode ? " (" + mode + ")" : ""));
    });
  });
}

function lora() {
  load_page("lora.html", "#navbar-lora", function () {
    generate_sync_word();
    get_config("lora_config.json").done(set_lora_config);
    get_config("config_list.json").done(function (list) {
      if (!Array.isArray(list)) return;
      const sel = $("#lora_config");
      list.forEach(function (el) {
        const value = L.toText(el && el.value);
        if (L.isLoraPreset(value)) sel.append(new Option(L.toText(el.name), value));
      });
    });
  });
}

function update() {
  load_page("/update", "#navbar-update", null);
}

function restart() {
  confirm_prompt("Device Restart", "Are you sure you want to restart the device?", function () {
    ws_send({ "request-type": "restart_device" });
  });
}

function reset() {
  confirm_prompt(
    "Device Reset",
    "Are you sure you want to reset the device? WiFi, LoRa settings, the username and the admin password go back to factory defaults.",
    function () {
      if (ws_send({ "request-type": "reset_device" })) setTimeout(redirect_home, 10000);
    }
  );
}

/* ------------------------------------------------------------------ */
/* WebSocket (Contract 3)                                              */
/* ------------------------------------------------------------------ */

function init_socket() {
  if (state.setupMode) return;
  const proto = window.location.protocol === "https:" ? "wss://" : "ws://";
  const ws = new WebSocket(proto + window.location.host + "/ws");
  let opened = false;
  state.socket = ws;

  ws.onmessage = function (event) {
    const data = L.safeJsonParse(event.data);
    if (!data) return;
    try {
      handle_ws_message(data);
    } catch (e) {
      console.log(e);
    }
  };
  ws.onopen = function () {
    opened = true;
    console.log("Connected to web sockets...");
    hide_loading();
    ws_send({ "request-type": "get_username" }, true);
    ws_send({ "request-type": "get_serial_mode" }, true);
    if (!state.connectedOnce) {
      state.connectedOnce = true;
      dashboard();
    }
  };
  ws.onclose = function () {
    if (state.socket !== ws) return;
    state.socket = null;
    if (state.setupMode || state.signInRequired) return;
    set_loading("Connecting...");
    // A handshake that never opened is most likely an auth failure (e.g. the
    // device rebooted and forgot the vsid session): get a new session first.
    reconnect_socket(!opened);
  };
  ws.onerror = function () {
    console.log("Error in websockets");
  };
}

function reconnect_socket(needSession) {
  setTimeout(function () {
    if (state.setupMode || state.signInRequired || state.socket) return;
    if (!needSession) {
      init_socket();
      return;
    }
    refresh_session()
      .done(init_socket)
      .fail(function (xhr) {
        if (state.setupMode || xhr.status === 401) return;
        reconnect_socket(true); // device still down: keep trying
      });
  }, 1000);
}

function ws_send(obj, quiet) {
  if (!state.socket || state.socket.readyState !== WebSocket.OPEN) {
    if (!quiet) alertText("Not connected to the device. Wait for the connection and retry.");
    return false;
  }
  state.socket.send(JSON.stringify(obj));
  return true;
}

function handle_ws_message(data) {
  switch (data.response_type) {
    case "tx_result":
      txTracker.resolve(data);
      break;
    case "lora_rx":
      handle_lora_rx(L.parseLoraRx(data.lora_msg));
      break;
    case "wifi_scan":
      render_wifi_scan(L.parseWifiScan(data));
      break;
    case "alert":
      alertText(data.alert_msg);
      break;
    case "set_uname":
      state.username = L.toText(data.uname);
      $("#username").val(state.username);
      break;
    case "serial_mode":
      state.serialMode = data.value === true || data.value === "true" || data.value === 1;
      $("#lora_to_serial").prop("checked", state.serialMode);
      refresh_locks();
      break;
    case "stop_transmission":
      if (state.sending) state.cancelSend = true;
      break;
    default:
      break;
  }
}

function handle_lora_rx(msg) {
  if (!msg) return;
  if (msg.packType === "msg") {
    add_message(msg.sender, msg.text, false);
    return;
  }
  if (msg.action === "enable_file_tx_mode") on_file_rx_start(msg);
  else if (msg.action === "disable_file_tx_mode") on_file_rx_end(msg);
  else if (msg.action === "sos") alertText("User " + msg.sender + " has sent a SOS request.");
}

function add_message(sender, text, own) {
  const li = $("<li>").addClass("list-group-item pre-wrap");
  if (own) li.addClass("list-group-item-light");
  li.append($("<b>").text(sender));
  li.append(document.createTextNode(" : " + text));
  $("#lora_rx_msg").prepend(li);
}

/* ------------------------------------------------------------------ */
/* Server-sent events (Contract 4)                                     */
/* ------------------------------------------------------------------ */

function init_events() {
  if (state.setupMode || state.events) return;
  const source = new EventSource("/rawEvents");
  state.events = source;
  source.addEventListener("open", function () {
    console.log("Events Connected");
  });
  source.addEventListener("error", function () {
    // CONNECTING: the browser is retrying by itself. CLOSED: the server refused
    // the stream (e.g. 401, session gone), so get a new session and reopen.
    if (source.readyState !== EventSource.CLOSED) {
      console.log("Events Disconnected");
      return;
    }
    source.close();
    if (state.events === source) state.events = null;
    reconnect_events();
  });

  source.addEventListener("RAW_DATA", function (e) {
    const ev = L.parseSseData(e.data);
    if (!ev) return;
    if (ev.millis !== undefined && ev.millis === state.lastRawMillis) return;
    state.lastRawMillis = ev.millis;
    if (!receiver.addChunk(ev.data)) return; // not receiving: ignore stray chunks
    const p = receiver.progress();
    $("#chunk_ratio").text("(" + p.received + " / " + p.total + ") " + p.sizeText + " received");
    set_progress(p.percent);
  });

  source.addEventListener("WIFI_DATA", function (e) {
    const d = L.safeJsonParse(e.data);
    if (d && d.rssi !== undefined) $("#wifi_quality").text(L.wifiQuality(d.rssi) + " %");
  });

  source.addEventListener("RAM_DATA", function (e) {
    const d = L.safeJsonParse(e.data);
    const h = d ? L.heapStatus(d.free_heap, d.heap_size) : null;
    if (!h) return;
    $("#ram_ratio")
      .empty()
      .append($("<b>").text(h.percent + "%"))
      .append(document.createTextNode(" (" + h.free + "/" + h.size + ")"));
    $("#heap_progress_bar")
      .removeClass("bg-success bg-warning bg-danger")
      .addClass(h.cls)
      .css("width", h.percent + "%");
  });

  source.addEventListener("DEBUG", function (e) {
    const ev = L.parseSseData(e.data);
    if (!ev || ev.data === "") return;
    if (ev.millis !== undefined && ev.millis === state.lastDebugMillis) return;
    state.lastDebugMillis = ev.millis;
    const box = $("#debug_textarea");
    const text = Date.now() + " > " + ev.data + "\n" + (box.val() || "");
    box.val(text.length > DEBUG_MAX_CHARS ? text.slice(0, DEBUG_MAX_CHARS) : text);
  });
}

function reconnect_events() {
  setTimeout(function () {
    if (state.setupMode || state.signInRequired || state.events) return;
    refresh_session()
      .done(init_events)
      .fail(function (xhr) {
        if (state.setupMode || xhr.status === 401) return;
        reconnect_events();
      });
  }, 2000);
}

/* ------------------------------------------------------------------ */
/* LoRa TX (Contract 2: 202 {id}; Contract 3: tx_result)               */
/* ------------------------------------------------------------------ */

function tx_http_error(xhr) {
  switch (xhr.status) {
    case 400:
      return { ok: false, fatal: true, reason: "the device rejected the request (400)" };
    case 413:
      return { ok: false, fatal: true, reason: "message too long" };
    case 503:
      return { ok: false, fatal: false, reason: "device busy, retry" };
    case 401:
      return { ok: false, fatal: true, reason: "not signed in" };
    case 403:
      return { ok: false, fatal: true, reason: "forbidden (403)" };
    default:
      return { ok: false, fatal: false, reason: describe_xhr(xhr) };
  }
}

// cb({ok, fatal, reason, akn, username}); called exactly once.
function lora_tx(url, payload, cb) {
  $.ajax({
    url: url,
    method: "POST",
    data: { data: payload },
    dataType: "json",
    timeout: AJAX_TIMEOUT_MS,
  })
    .done(function (body, _status, xhr) {
      if (xhr.status !== 202 || !body || body.id === undefined || body.id === null) {
        cb({ ok: false, fatal: false, reason: "unexpected response from the device" });
        return;
      }
      txTracker.register(body.id, function (r) {
        cb({
          ok: r.akn === 1,
          fatal: false,
          akn: r.akn,
          username: r.username,
          reason: r.timeout
            ? "no result from the device within " + L.TX_RESULT_TIMEOUT_MS / 1000 + " s"
            : r.akn === 1
            ? ""
            : "not acknowledged by the receiver",
        });
      });
    })
    .fail(function (xhr) {
      cb(tx_http_error(xhr));
    });
}

// Control messages (enable/disable file mode) get the same retry budget as chunks.
function lora_tx_retry(url, payload, retries, cb) {
  lora_tx(url, payload, function (r) {
    if (r.ok || r.fatal || retries <= 0) cb(r);
    else
      setTimeout(function () {
        lora_tx_retry(url, payload, retries - 1, cb);
      }, 1000);
  });
}

function send_lora() {
  const box = $("#lora_msg");
  const msg = box.val() || "";
  if (msg.trim() === "") return;
  const inner = L.buildTextMessage(msg);
  const chk = L.checkPayload(state.username, inner);
  if (!chk.ok) {
    alertText(
      "Message too long: " + chk.size + " bytes once wrapped for the radio, the limit is " +
        chk.max + " bytes.\nShorten it by at least " + (chk.size - chk.max) + " bytes."
    );
    return;
  }
  box.prop("readonly", true);
  $("#lora_send_status").text("Sending...");
  lora_tx("/lora_transmit", inner, function (r) {
    box.prop("readonly", false);
    if (r.ok) {
      box.val("");
      $("#lora_send_status").text("Delivered.");
      add_message(r.username || state.username || "You", msg, true);
    } else {
      $("#lora_send_status").text("");
      alertText("Message not delivered: " + r.reason + ".");
    }
  });
}

function set_username(val) {
  if (!L.validateUsername(val)) {
    alertText("The name must be 1-32 characters.");
    return;
  }
  ws_send({ "request-type": "set_username", val: val });
}

function set_lora_to_serial(val) {
  state.serialMode = !!val;
  refresh_locks();
  ws_send({ "request-type": "set_serial_mode", value: !!val });
}

/* ------------------------------------------------------------------ */
/* File transfer                                                       */
/* ------------------------------------------------------------------ */

function file_broadcast() {
  if (state.sending) {
    alertText("A file transfer is already in progress.");
    return;
  }
  if (receiver.isActive()) {
    alertText("A file is being received. Cancel receiving first.");
    return;
  }
  const files = $("#broadcastFile").prop("files");
  const file = files && files[0];
  if (!file) {
    alertText("Choose a file first.");
    return;
  }
  const chunkRaw = $("#chunk_size").val();
  if (!L.validateChunkSize(chunkRaw)) {
    alertText("Packet size must be a whole number from " + L.CHUNK_MIN + " to " + L.CHUNK_MAX + ".");
    return;
  }
  const chunkSize = Number(chunkRaw);
  const waitTime = Number($("#wait_time").val());
  if (!(isFinite(waitTime) && waitTime >= 10)) {
    alertText("Wait time must be at least 10 ms.");
    return;
  }

  state.sending = true;
  state.cancelSend = false;
  refresh_locks();

  const reader = new FileReader();
  reader.onerror = function () {
    end_send();
    alertText("Could not read the file.");
  };
  reader.onload = function () {
    const dataURL = String(reader.result || "");
    const chunks = L.chunkString(dataURL, chunkSize);
    const total = chunks.length;
    const sha = L.sha256Hex(dataURL);
    const sizeText = L.get_string_size(dataURL);
    const time = L.formatDuration(L.estimateSeconds(total, waitTime));
    const enable = L.buildEnableFileTx(
      { total_packets: total, size: sizeText, name: file.name, time: time },
      state.username
    );
    if (!enable) {
      end_send();
      alertText("Could not fit the file details into one radio packet.");
      return;
    }

    render_kv("#rec_info", [
      ["Sending", file.name],
      ["Total size", sizeText],
      ["Chunks", String(total)],
      ["Estimated min time", time],
    ]);
    $("#rec_verify").text("").removeClass("text-success text-danger");
    $("#chunk_ratio").text("(0 / " + total + ") chunks sent");
    set_progress(0, "bg-info");

    const sender = L.createChunkSender({
      chunks: chunks,
      waitMs: waitTime,
      retryDelayMs: 1000,
      maxRetries: L.CHUNK_MAX_RETRIES,
      schedule: function (fn, ms) {
        setTimeout(fn, ms);
      },
      isCancelled: function () {
        return state.cancelSend;
      },
      send: function (chunk, _i, done) {
        lora_tx("/send_raw", chunk, done);
      },
      onProgress: function (sent, tot) {
        $("#chunk_ratio").text("(" + sent + " / " + tot + ") chunks sent");
        set_progress((sent / tot) * 100);
      },
      onFinish: function (res) {
        finish_send(res, sha);
      },
    });

    lora_tx_retry("/lora_transmit", enable, L.CHUNK_MAX_RETRIES, function (r) {
      if (!r.ok) {
        end_send();
        alertText("Could not start the transfer: " + r.reason + ".");
        return;
      }
      // Give the receiving browser time to process enable_file_tx_mode (WS)
      // before the first chunk arrives on SSE.
      setTimeout(sender.start, 1000);
    });
  };
  reader.readAsDataURL(file);
}

function finish_send(res, sha) {
  // Always tell the receiver we're done, with what it should have received.
  lora_tx_retry(
    "/lora_transmit",
    L.buildDisableFileTx(res.total, sha),
    L.CHUNK_MAX_RETRIES,
    function (r) {
      end_send();
      let msg;
      if (res.ok) {
        msg = "File sent: " + res.total + " chunks.";
        if (!r.ok)
          msg += "\nWarning: the end-of-transfer message failed (" + r.reason +
            "), so the receiver can't verify the file.";
        set_progress(100, "bg-success");
      } else {
        msg = "Transfer aborted after " + res.sent + " of " + res.total + " chunks: " + res.reason + ".";
        set_progress((res.sent / Math.max(1, res.total)) * 100, "bg-danger");
      }
      $("#rec_verify").text(msg).toggleClass("text-success", res.ok).toggleClass("text-danger", !res.ok);
      alertText(msg);
    }
  );
}

function end_send() {
  state.sending = false;
  state.cancelSend = false;
  refresh_locks();
}

function stop_broadcast() {
  if (!state.sending) return;
  state.cancelSend = true;
  $("#chunk_ratio").text("Stopping after the current chunk...");
}

function on_file_rx_start(msg) {
  if (state.sending) {
    console.log("Ignoring an incoming file while sending one.");
    return;
  }
  if (msg.totalPackets < 0) return;
  receiver.start({
    totalPackets: msg.totalPackets,
    fileName: msg.fileName,
    size: msg.size,
    time: msg.time,
    sender: msg.sender,
  });
  state.lastRawMillis = null;
  render_kv("#rec_info", [
    ["Receiving from", msg.sender],
    ["File name", msg.fileName],
    ["Total size", msg.size],
    ["Chunks", String(msg.totalPackets)],
    ["Estimated min time", msg.time],
  ]);
  $("#rec_verify").text("").removeClass("text-success text-danger");
  $("#chunk_ratio").text("(0 / " + msg.totalPackets + ") 0 Bytes received");
  set_progress(0, "bg-success");
  refresh_locks();
}

function on_file_rx_end(msg) {
  if (!receiver.isActive()) return;
  const r = receiver.finish({ totalPackets: msg.totalPackets, sha256: msg.sha256 });
  if (r.verified) {
    $("#rec_verify")
      .text("Verified: " + r.received + " packets, SHA-256 matches. Ready to download.")
      .removeClass("text-danger")
      .addClass("text-success");
  } else {
    $("#rec_verify")
      .text("Corrupted: " + r.reason + ". Download is disabled.")
      .removeClass("text-success")
      .addClass("text-danger");
    set_progress(receiver.progress().percent, "bg-danger");
  }
  refresh_locks();
}

function cancel_receive() {
  if (!receiver.cancel()) return;
  $("#rec_verify").text("Receiving cancelled.").removeClass("text-success").addClass("text-danger");
  refresh_locks();
}

function downloadFile() {
  if (receiver.isActive()) {
    alertText("A file transfer is in progress.");
    return;
  }
  if (!receiver.canDownload()) {
    alertText("There is no verified file to download.");
    return;
  }
  let decoded;
  try {
    decoded = L.dataURIToBytes(receiver.data());
  } catch (e) {
    alertText("The received data isn't a valid file.");
    return;
  }
  const link = document.createElement("a");
  link.href = URL.createObjectURL(new Blob([decoded.bytes], { type: decoded.mime }));
  link.download = receiver.fileName() || "Voyager_Download";
  link.click();
  setTimeout(function () {
    URL.revokeObjectURL(link.href);
  }, 1000);
}

/* ------------------------------------------------------------------ */
/* WiFi page                                                           */
/* ------------------------------------------------------------------ */

function scan_ssid() {
  $("#wifi_ssid_list").empty();
  if (!ws_send({ "request-type": "wifi_ssid_scan" })) return;
  $("#wifi_scan_btn").text("Scanning...").prop("disabled", true);
  clearTimeout(state.scanTimer);
  state.scanTimer = setTimeout(function () {
    $("#wifi_scan_btn").text("Scan SSID").prop("disabled", false);
  }, 20000);
}

function render_wifi_scan(list) {
  clearTimeout(state.scanTimer);
  $("#wifi_scan_btn").text("Scan SSID").prop("disabled", false);
  const ul = $("#wifi_ssid_list").empty();
  const visible = list.filter(function (n) {
    return n.ssid !== "";
  });
  if (visible.length === 0) {
    ul.append($("<li>").addClass("list-group-item").text("No networks found."));
    return;
  }
  visible.forEach(function (n) {
    const a = $("<a>")
      .attr("href", "#wifi_password")
      .text(n.ssid)
      .on("click", function (ev) {
        ev.preventDefault();
        $("#wifi_ssid").val(n.ssid);
        $("#wifi_password").trigger("focus");
      });
    const badge = $("<span>").addClass("badge bg-secondary rounded-pill").text(n.quality + " %");
    $("<li>")
      .addClass("list-group-item d-flex justify-content-between align-items-center")
      .addClass(n.cls)
      .append(a, badge)
      .appendTo(ul);
  });
}

function wifi_connect() {
  const ssid = $("#wifi_ssid").val() || "";
  const psk = $("#wifi_password").val() || "";
  if (ssid === "") {
    alertText("Choose a network from the scan list first.");
    return;
  }
  if (!L.validateWifiPassword(psk)) {
    alertText("The WiFi password must be empty (open network) or 8-63 printable characters.");
    return;
  }
  const go = function () {
    if (!ws_send({ "request-type": "connect_wifi", wifi_ssid: ssid, wifi_pass: psk })) return;
    set_loading("Connecting to WiFi...");
    setTimeout(redirect_home, 10000);
  };
  if (psk === "")
    confirm_prompt("Open network", 'Connect to "' + ssid + '" without a password?', go);
  else go();
}

function wifi_ap_mode() {
  confirm_prompt("Access Point", "Are you sure you want to switch to access point mode?", function () {
    $.ajax({ url: "/username", dataType: "text", timeout: AJAX_TIMEOUT_MS })
      .always(function (name) {
        if (!ws_send({ "request-type": "wifi_ap_mode" })) return;
        const ap = typeof name === "string" && name.trim() !== "" ? name.trim() : "Voyager";
        alertText(
          'Switched to AP mode.\nConnect to the "' + ap +
            '" access point. Its password is shown on the device OLED Network page.'
        );
        setTimeout(redirect_home, 10000);
      });
  });
}

function set_ap_password() {
  const p = $("#ap_password").val() || "";
  if (p !== "" && !L.validateApPassword(p)) {
    alertText("The access point password must be 8-63 printable ASCII characters, or empty for the device default.");
    return;
  }
  const body =
    p === ""
      ? "Reset the access point password to the device default (shown on the OLED Network page)?"
      : "Change the access point password? Devices on the access point will need the new password to reconnect.";
  confirm_prompt("Access point password", body, function () {
    if (ws_send({ "request-type": "set_ap_pass", val: p })) {
      $("#ap_password").val("");
      $("#ap_pass_status").text("Request sent to the device.");
    }
  });
}

/* ------------------------------------------------------------------ */
/* LoRa page                                                           */
/* ------------------------------------------------------------------ */

function generate_sync_word() {
  const sel = $("#sync_word").empty();
  for (let i = 0; i <= 255; i++) {
    const hex = ("0" + i.toString(16).toUpperCase()).slice(-2);
    sel.append(new Option(i + " (0x" + hex + ")", String(i)));
  }
}

function set_lora_config(cfg) {
  if (!cfg) return;
  const mhz = Number(cfg.freq) / 1000000;
  $("#freq_range").val(mhz);
  $("#selected_lora_freq").text(mhz);
  $("#tx_power_range").val(cfg.TxPower);
  $("#selected_lora_tx_power").text(L.toText(cfg.TxPower));
  $("#spreading_factor").val(cfg.SpreadingFactor);
  $("#selected_lora_spreading_factor").text(L.toText(cfg.SpreadingFactor));
  $("#bandwidth").val(String(cfg.SignalBandwidth));
  $("#coding_rate").val(cfg.CodingRate4);
  $("#selected_lora_coding_rate").text(L.toText(cfg.CodingRate4));
  $("#sync_word").val(String(cfg.SyncWord));
}

function read_lora_form() {
  return L.validateLoraConfig(
    L.loraConfigFromForm({
      freqMHz: $("#freq_range").val(),
      txPower: $("#tx_power_range").val(),
      spreadingFactor: $("#spreading_factor").val(),
      bandwidth: $("#bandwidth").val(),
      codingRate: $("#coding_rate").val(),
      syncWord: $("#sync_word").val(),
    })
  );
}

function save_lora_config() {
  const v = read_lora_form();
  if (!v.ok) {
    alertText("Invalid LoRa settings:\n" + v.errors.join("\n"));
    return;
  }
  ws_send({ "request-type": "set_lora_config", val: JSON.stringify(v.config) });
}

function download_lora_config() {
  const v = read_lora_form();
  if (!v.ok) {
    alertText("Invalid LoRa settings:\n" + v.errors.join("\n"));
    return;
  }
  JSONToFile(v.config, "project_voyager_lora_config.json");
}

function update_lora_config_from_file() {
  const files = $("#LoraConfigFile").prop("files");
  const file = files && files[0];
  if (!file) return;
  const reader = new FileReader();
  reader.onload = function () {
    $("#LoraConfigFile").val("");
    const v = L.validateLoraConfig(L.safeJsonParse(String(reader.result || "")));
    if (!v.ok) {
      alertText("Invalid settings file:\n" + v.errors.join("\n"));
      return;
    }
    set_lora_config(v.config);
    alertHtml("Settings loaded.<br>Press <b>Save</b> to apply them.");
  };
  reader.onerror = function () {
    alertText("Could not read the file.");
  };
  reader.readAsText(file);
}

function load_lora_config(name) {
  if (L.isLoraPreset(name)) get_config(name).done(set_lora_config);
}

function JSONToFile(obj, filename) {
  const blob = new Blob([JSON.stringify(obj, null, 2)], { type: "application/json" });
  const a = document.createElement("a");
  a.href = URL.createObjectURL(blob);
  a.download = filename;
  a.click();
  setTimeout(function () {
    URL.revokeObjectURL(a.href);
  }, 1000);
}

function set_freq_range(val) {
  $("#selected_lora_freq").text(val);
}
function set_tx_power(val) {
  $("#selected_lora_tx_power").text(val);
}
function set_spreading_factor(val) {
  $("#selected_lora_spreading_factor").text(val);
}
function set_coding_rate(val) {
  $("#selected_lora_coding_rate").text(val);
}
