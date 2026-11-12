/*
 * Project Voyager web UI - DOM, network and socket wiring.
 * Pure logic lives in lib.js (VoyagerLib), which index.html loads first.
 *
 * The UI is split into several small files so the device never has to serve
 * one large script: script_net.js, script_file.js, script_sensors.js,
 * script_wifi.js and script_lora.js hold function declarations only. This
 * file holds the shared constants, `state`, boot, auth, helpers and page
 * loading, and index.html loads it LAST so everything exists before boot.
 *
 * Rule: anything that came from the device or over the radio (names, messages,
 * SSIDs, file names, alerts, debug lines) goes into the DOM with .text(), .val()
 * or DOM nodes. Never concatenate it into HTML.
 */
"use strict";

const L = VoyagerLib;

// UI timing constants (milliseconds)
const AJAX_TIMEOUT_MS = 10000;
const DEBUG_MAX_CHARS = 20000;
const WS_RECONNECT_DELAY_MS = 1000;
const SSE_RECONNECT_DELAY_MS = 2000;
const SCAN_TIMEOUT_MS = 20000;
const REDIRECT_DELAY_MS = 10000;
const MODAL_HIDE_DELAY_MS = 600;
const RETRY_DELAY_MS = 1000;
const URL_REVOKE_DELAY_MS = 1000;
const SENSOR_RX_MAX_ROWS = 40;

const state = {
  socket: null,
  events: null,
  hostnameUrl: "",
  username: "",
  serialMode: false,
  msgAck: true, // dashboard "Wait for acknowledgement" switch
  sensorMode: false, // sensor broadcast is on: no serial bridge, no file transfer
  sensorCfg: null, // last WS sensor_config
  sensorPins: { io: [], in: [], adc: [] },
  sensorLocal: null, // { readings, time } from WS sensor_data
  sensorRx: [], // newest first: { key, sender, name, typeLabel, text, time }
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
/* Boot                                                               */
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
  $("#loadingModal").on("shown.bs.modal", function () {
    if (!loadingWanted) loadingModal.hide();
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

function schedule_redirect() {
  setTimeout(redirect_home, REDIRECT_DELAY_MS);
}

/* ------------------------------------------------------------------ */
/* First-boot password setup / admin password change (Contract 2)     */
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
/* Small UI helpers                                                   */
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

let loadingWanted = false;

function set_loading(msg) {
  loadingWanted = true;
  $("#loading_model_body").text(msg);
  loadingModal.show();
}

function hide_loading() {
  loadingWanted = false;
  // Bootstrap ignores hide() while the show transition is still running (and
  // that transition can take arbitrarily long in a background tab), so the
  // "shown" handler registered at boot hides it again once it has finished.
  loadingModal.hide();
  setTimeout(function () {
    if (!loadingWanted) loadingModal.hide();
  }, MODAL_HIDE_DELAY_MS);
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
//  .file_tx_controls  (file transfer form):  the above, receiving a file, or sensor mode
//  .busy_lock         (WiFi / LoRa settings): sending or receiving a file
//  .sensor_controls   (sensor form):          sending or receiving a file
//  #lora_to_serial:                           sensor mode
function refresh_locks() {
  const receiving = receiver.isActive();
  const lockMsg = state.serialMode || state.sending;
  const lockFile = lockMsg || receiving || state.sensorMode;
  $(".sensor_controls").prop("disabled", state.sending || receiving);
  $("#lora_to_serial").prop("disabled", state.sensorMode);
  $("#sensor_mode_note").prop("hidden", !state.sensorMode);
  $(".lora_transmission").prop("disabled", lockMsg);
  $(".file_tx_controls").prop("disabled", lockFile);
  $(".busy_lock").prop("disabled", state.sending || receiving);
  $("#fileBroadcastButton").prop("disabled", lockFile);
  $("#stopBroadcastButton").prop("disabled", !state.sending);
  $("#cancel_receive_btn").prop("hidden", !receiving);
  $("#download_btn").prop("disabled", !receiver.canDownload());
}

/* ------------------------------------------------------------------ */
/* Pages                                                              */
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
    $("#lora_msg_ack").prop("checked", state.msgAck);
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

function sensors() {
  load_page("sensors.html", "#navbar-sensors", function () {
    render_sensor_form();
    render_sensor_local();
    render_sensor_rx();
    ws_send({ "request-type": "get_sensor_config" }, true);
  });
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
      if (ws_send({ "request-type": "reset_device" })) schedule_redirect();
    }
  );
}
