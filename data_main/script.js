/*
 * Project Voyager web UI - DOM, network and socket wiring.
 * Pure logic lives in lib.js (VoyagerLib), which index.html loads first.
 * Plain browser APIs only (fetch, DOM, <dialog>): no libraries.
 *
 * Rule: anything that came from the device or over the radio (names, messages,
 * SSIDs, file names, alerts, debug lines) goes into the DOM with textContent,
 * .value or DOM nodes. Never concatenate it into HTML.
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
const MODAL_FADE_MS = 300;
const RETRY_DELAY_MS = 1000;
const URL_REVOKE_DELAY_MS = 1000;

// lib.js returns these class names; the stylesheet uses shorter ones.
const BAR_CLASS = { "bg-success": "ok", "bg-warning": "warn", "bg-danger": "bad", "bg-info": "info" };
const BAR_CLASSES = ["ok", "warn", "bad", "info"];
const WIFI_CLASS = {
  "list-group-item-success": "good",
  "list-group-item-primary": "fair",
  "list-group-item-warning": "weak",
  "list-group-item-danger": "poor",
};

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
  lastDebugKey: null,
  scanTimer: null,
  txActive: 0,
  scanning: false,
};

let promptModal = null;
let alertModal = null;
let loadingModal = null;

const receiver = L.createFileReceiver();
const txTracker = L.createTxTracker({
  setTimeout: (fn, ms) => window.setTimeout(fn, ms),
  clearTimeout: (t) => window.clearTimeout(t),
});

/* ------------------------------------------------------------------ */
/* DOM and HTTP helpers                                                */
/* ------------------------------------------------------------------ */

function byId(id) {
  return document.getElementById(id);
}

function setText(id, text) {
  const el = byId(id);
  if (el) el.textContent = text;
}

function setValue(id, value) {
  const el = byId(id);
  if (el) el.value = value;
}

function getValue(id) {
  const el = byId(id);
  return el ? el.value || "" : "";
}

function setDisabled(id, disabled) {
  const el = byId(id);
  if (el) el.disabled = disabled;
}

function el(tag, cls, text) {
  const node = document.createElement(tag);
  if (cls) node.className = cls;
  if (text !== undefined) node.textContent = text;
  return node;
}

// fetch() wrapper. Resolves {status, body} on 2xx; rejects with
// {status, statusText, responseText} otherwise (status 0 = unreachable).
// opts: {method, data (object -> query string or form body), as: "json"|"text"}.
function request(url, opts) {
  opts = opts || {};
  const method = opts.method || "GET";
  const init = { method: method, credentials: "same-origin" };
  if (opts.data) {
    const params = new URLSearchParams(opts.data);
    if (method === "GET") url += (url.indexOf("?") < 0 ? "?" : "&") + params.toString();
    else init.body = params;
  }
  const ctrl = new AbortController();
  init.signal = ctrl.signal;
  const timer = setTimeout(function () {
    ctrl.abort();
  }, AJAX_TIMEOUT_MS);

  return fetch(url, init)
    .then(
      function (res) {
        return res.text().then(function (text) {
          return { status: res.status, text: text };
        });
      },
      function (e) {
        throw { status: 0, statusText: e && e.name === "AbortError" ? "timeout" : "error", responseText: "" };
      }
    )
    .then(function (r) {
      clearTimeout(timer);
      if (r.status < 200 || r.status >= 300) {
        // 403 {"error":"setup_required"} from any endpoint -> set-password screen.
        if (r.status === 403) {
          const body = L.safeJsonParse(r.text);
          if (body && body.error === "setup_required") show_setup_screen();
        }
        throw { status: r.status, statusText: "error", responseText: r.text };
      }
      if (opts.as !== "json") return { status: r.status, body: r.text };
      const body = L.safeJsonParse(r.text);
      if (body === null && r.text.trim() !== "null")
        throw { status: r.status, statusText: "parsererror", responseText: r.text };
      return { status: r.status, body: body };
    })
    .catch(function (err) {
      clearTimeout(timer);
      throw err;
    });
}

// Native <dialog> with the fade/slide animation. Static dialogs ignore Esc
// and backdrop clicks. onHidden runs after every close.
function createModal(dialog, onHidden) {
  const isStatic = dialog.hasAttribute("data-static");
  let hideTimer = null;

  dialog.tabIndex = -1; // focus the dialog itself, not its first button

  function show() {
    clearTimeout(hideTimer);
    if (!dialog.open) {
      dialog.showModal();
      dialog.focus();
    }
    requestAnimationFrame(function () {
      dialog.classList.add("in");
    });
  }
  function hide() {
    if (!dialog.open) return;
    dialog.classList.remove("in");
    clearTimeout(hideTimer);
    hideTimer = setTimeout(function () {
      dialog.close();
    }, MODAL_FADE_MS);
  }

  dialog.addEventListener("cancel", function (ev) {
    ev.preventDefault(); // Esc: animate out ourselves (or not at all when static)
    if (!isStatic) hide();
  });
  dialog.addEventListener("click", function (ev) {
    // A click on the dialog itself (not its content) is a backdrop click.
    if (!isStatic && ev.target === dialog) hide();
  });
  dialog.addEventListener("close", function () {
    dialog.classList.remove("in");
    if (onHidden) onHidden();
  });
  dialog.querySelectorAll("[data-dismiss]").forEach(function (b) {
    b.addEventListener("click", hide);
  });
  return { show: show, hide: hide };
}

/* ------------------------------------------------------------------ */
/* Boot                                                                */
/* ------------------------------------------------------------------ */

document.addEventListener("DOMContentLoaded", function () {
  // A dismissed prompt must not leave a stale Proceed handler behind.
  promptModal = createModal(byId("promptModal"), function () {
    byId("promptModelProceed").onclick = null;
  });
  alertModal = createModal(byId("alertModal"));
  loadingModal = createModal(byId("loadingModal"));

  byId("setup_form").addEventListener("submit", submit_setup);
  byId("download_btn").addEventListener("click", downloadFile);
  byId("cancel_receive_btn").addEventListener("click", cancel_receive);

  const toggler = byId("nav_toggler");
  toggler.addEventListener("click", function () {
    const open = byId("nav_menu").classList.toggle("open");
    toggler.setAttribute("aria-expanded", String(open));
  });
  document.querySelectorAll("#nav_menu [data-page]").forEach(function (a) {
    a.addEventListener("click", function (ev) {
      ev.preventDefault();
      PAGES[a.getAttribute("data-page")]();
    });
  });

  get_hostname();
  check_auth();
});

function check_auth() {
  set_loading("Connecting...");
  request("/api/auth/status", { as: "json" }).then(
    function (r) {
      const s = r.body;
      if (s && s.set === false) show_setup_screen();
      else if (s && s.set === true) start_session();
      else boot_error("The device sent an unexpected auth status.");
    },
    function (err) {
      boot_error("Could not reach the device (" + describe_err(err) + ").\nReload the page to retry.");
    }
  );
}

// GET /api/session (Contract 2) brings up the browser's Digest prompt and makes
// the device set the HttpOnly vsid cookie that authenticates WS and SSE.
// WS/SSE are opened only after it succeeds.
function start_session() {
  refresh_session().then(
    function () {
      init_socket();
      init_events();
    },
    function (err) {
      if (state.setupMode || err.status === 401) return; // handled in refresh_session
      boot_error(
        "Could not start a session with the device (" + describe_err(err) + ").\nReload the page to retry."
      );
    }
  );
}

// One /api/session request at a time; concurrent callers share it.
function refresh_session() {
  if (state.sessionRequest) return state.sessionRequest;
  const req = request("/api/session", { as: "json" });
  state.sessionRequest = req;
  req.then(
    function () {
      state.sessionRequest = null;
    },
    function (err) {
      state.sessionRequest = null;
      if (err.status === 401 && !state.setupMode) {
        state.signInRequired = true;
        boot_error("Sign-in is required. Reload the page and enter the admin password.");
      }
    }
  );
  return req;
}

function boot_error(msg) {
  hide_loading();
  byId("main_content").replaceChildren(el("p", "text-bad pre-line", msg));
}

function get_hostname() {
  request("/hostname").then(
    function (r) {
      const name = String(r.body || "").trim();
      if (/^[A-Za-z0-9-]{1,63}$/.test(name)) {
        state.hostnameUrl = "http://" + name + ".local/";
        byId("project_title").setAttribute("href", state.hostnameUrl);
      }
    },
    function () {}
  );
}

function redirect_home() {
  window.location.replace(state.hostnameUrl || "/");
}

function schedule_redirect() {
  setTimeout(redirect_home, REDIRECT_DELAY_MS);
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
  ["main_nav", "main_content", "status_panel"].forEach(function (id) {
    byId(id).hidden = true;
  });
  byId("setup_screen").hidden = false;
  byId("setup_pass1").focus();
}

function submit_setup(ev) {
  ev.preventDefault();
  const p1 = getValue("setup_pass1");
  const p2 = getValue("setup_pass2");
  if (!L.validatePassword(p1)) {
    setText("setup_error", "The password must be 8-63 characters.");
    return;
  }
  if (p1 !== p2) {
    setText("setup_error", "The passwords don't match.");
    return;
  }
  setText("setup_error", "");
  setDisabled("setup_submit", true);
  request("/api/auth/setup", { method: "POST", data: { password: p1 } }).then(
    function () {
      window.location.reload();
    },
    function (err) {
      setDisabled("setup_submit", false);
      if (err.status === 409) window.location.reload(); // already set elsewhere
      else if (err.status === 400) setText("setup_error", "The device rejected the password (8-63 characters).");
      else setText("setup_error", "Could not set the password (" + describe_err(err) + ").");
    }
  );
}

function change_admin_password() {
  const p1 = getValue("admin_pass1");
  const p2 = getValue("admin_pass2");
  if (!L.validatePassword(p1)) {
    alertText("The admin password must be 8-63 characters.");
    return;
  }
  if (p1 !== p2) {
    alertText("The passwords don't match.");
    return;
  }
  request("/api/auth/change", { method: "POST", data: { password: p1 } }).then(
    function () {
      setValue("admin_pass1", "");
      setValue("admin_pass2", "");
      alertText(
        "Admin password changed.\nYour browser will ask you to sign in again with the new password."
      );
    },
    function (err) {
      if (err.status === 400) alertText("The device rejected the password (8-63 characters).");
      else if (err.status === 401) alertText("Not signed in. Reload the page and sign in first.");
      else alertText("Could not change the password (" + describe_err(err) + ").");
    }
  );
}

/* ------------------------------------------------------------------ */
/* Small UI helpers                                                    */
/* ------------------------------------------------------------------ */

// Plain text only: safe for anything that came from the device or the radio.
function alertText(msg) {
  setText("alert_body", L.toText(msg));
  alertModal.show();
}

// Fixed, trusted strings that need markup such as <br>. Never pass remote text.
function alertHtml(html) {
  byId("alert_body").innerHTML = html;
  alertModal.show();
}

function confirm_prompt(title, body, onProceed) {
  setText("promptModalLabel", title);
  setText("prompt_body", body);
  byId("promptModelProceed").onclick = function () {
    byId("promptModelProceed").onclick = null;
    promptModal.hide();
    onProceed();
  };
  promptModal.show();
}

function set_loading(msg) {
  setText("loading_model_body", msg);
  loadingModal.show();
}

function hide_loading() {
  loadingModal.hide();
}

function render_kv(id, rows) {
  const box = byId(id);
  box.replaceChildren();
  rows.forEach(function (row, i) {
    if (i > 0) box.append(el("br"));
    box.append(document.createTextNode(row[0] + ": "));
    box.append(el("b", "", L.toText(row[1])));
  });
}

function set_bar_class(bar, cls) {
  BAR_CLASSES.forEach(function (c) {
    bar.classList.remove(c);
  });
  if (cls) bar.classList.add(cls);
}

// Status LEDs in the top bar. cls: "on" | "warn" | "bad" | "busy" | "" (off).
const LED_CLASSES = ["on", "warn", "bad", "busy"];
const ledTimers = {};
function set_led(id, cls) {
  const led = byId(id);
  if (!led) return;
  clearTimeout(ledTimers[id]);
  LED_CLASSES.forEach(function (c) {
    led.classList.remove(c);
  });
  if (cls) led.classList.add(cls);
}
// Light an LED for ms, then go back to restoreCls.
function flash_led(id, cls, ms, restoreCls) {
  set_led(id, cls);
  ledTimers[id] = setTimeout(function () {
    set_led(id, restoreCls || "");
  }, ms);
}

function tx_led_begin() {
  state.txActive++;
  set_led("led_tx", "busy");
}
function tx_led_end(ok) {
  state.txActive = Math.max(0, state.txActive - 1);
  flash_led("led_tx", ok ? "on" : "bad", ok ? 1500 : 3000, state.txActive > 0 ? "busy" : "");
}

function rx_led_blink() {
  flash_led("led_rx", "on", 400);
}

// cls: "ok" | "info" | "warn" | "bad" (omit to keep the current colour).
function set_progress(percent, cls) {
  const bar = byId("file_upload_progress_bar");
  const p = Math.max(0, Math.min(100, Number(percent) || 0));
  bar.style.width = p + "%";
  bar.setAttribute("aria-valuenow", String(Math.round(p)));
  if (cls) set_bar_class(bar, cls);
}

function reset_progress_bar() {
  if (!state.sending && !receiver.isActive()) set_progress(0);
}

function set_verify(text, ok) {
  const box = byId("rec_verify");
  box.textContent = text;
  box.classList.toggle("text-ok", ok === true);
  box.classList.toggle("text-bad", ok === false);
}

function describe_err(err) {
  if (!err) return "unknown error";
  if (err.statusText === "timeout") return "timed out";
  if (err.status === 0) return "device unreachable";
  return "HTTP " + err.status;
}

function get_config(name) {
  if (!L.isAllowedConfigName(name)) return Promise.reject({ status: 0 });
  return request("/api/config", { data: { name: name }, as: "json" }).then(function (r) {
    return r.body;
  });
}

function set_fieldsets_disabled(cls, disabled) {
  document.querySelectorAll("." + cls).forEach(function (f) {
    f.disabled = disabled;
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
  set_fieldsets_disabled("lora_transmission", lockMsg);
  set_fieldsets_disabled("file_tx_controls", lockFile);
  set_fieldsets_disabled("busy_lock", state.sending || receiving);
  setDisabled("fileBroadcastButton", lockFile);
  setDisabled("stopBroadcastButton", !state.sending);
  byId("cancel_receive_btn").hidden = !receiving;
  setDisabled("download_btn", !receiver.canDownload());
}

/* ------------------------------------------------------------------ */
/* Pages                                                               */
/* ------------------------------------------------------------------ */

function load_page(url, navId, after) {
  document.querySelectorAll("#nav_menu a").forEach(function (a) {
    a.classList.remove("active");
  });
  if (navId) byId(navId).classList.add("active");
  request(url).then(
    function (r) {
      // Static page fragments shipped in LittleFS (or the device's OTA form).
      byId("main_content").innerHTML = r.body;
      refresh_locks();
      if (after) after();
    },
    function (err) {
      if (state.setupMode) return;
      byId("main_content").replaceChildren(
        el("p", "text-bad", "Could not load the page (" + describe_err(err) + ").")
      );
    }
  );
}

function dashboard() {
  load_page("dashboard.html", "navbar-dashboard", function () {
    setValue("username", state.username);
    byId("lora_to_serial").checked = state.serialMode;
    get_config("lora_serial.json").then(function (cfg) {
      state.serialMode = !!(cfg && cfg.lora_serial);
      const sw = byId("lora_to_serial");
      if (sw) sw.checked = state.serialMode;
      refresh_locks();
    }, function () {});
    ws_send({ "request-type": "get_username" }, true);
  });
}

function file_transfer() {
  load_page("file_transfer.html", "navbar-file-transfer", null);
}

function wifi() {
  load_page("wifi.html", "navbar-wifi", function () {
    get_config("wifi_config.json").then(function (cfg) {
      const ssid = L.toText(cfg && cfg.wifi_ssid);
      const mode = L.toText(cfg && cfg.wifi_function);
      setText("wifi_ssid_name", ssid + (mode ? " (" + mode + ")" : ""));
    }, function () {});
  });
}

function lora() {
  load_page("lora.html", "navbar-lora", function () {
    generate_sync_word();
    get_config("lora_config.json").then(set_lora_config, function () {});
    get_config("config_list.json").then(function (list) {
      if (!Array.isArray(list)) return;
      const sel = byId("lora_config");
      if (!sel) return;
      list.forEach(function (item) {
        const value = L.toText(item && item.value);
        if (L.isLoraPreset(value)) sel.add(new Option(L.toText(item.name), value));
      });
    }, function () {});
  });
}

function update() {
  load_page("/update", "navbar-update", function () {
    const title = el("h2", "panel-title", "Firmware Update");
    title.append(el("small", "", "Over-the-air"));
    byId("main_content").prepend(title);
  });
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

// Navbar data-page -> handler.
const PAGES = {
  dashboard: dashboard,
  file_transfer: file_transfer,
  wifi: wifi,
  lora: lora,
  update: update,
  restart: restart,
  reset: reset,
};

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
    set_led("led_ws", "on");
    console.log("Connected to web sockets...");
    hide_loading();
    ws_send({ "request-type": "get_username" }, true);
    ws_send({ "request-type": "get_serial_mode" }, true);
    // A scan result broadcast while we were reconnecting would be lost: ask for
    // the device's cached one (Contract 3 get_wifi_scan; no reply if none yet).
    if (state.scanning) ws_send({ "request-type": "get_wifi_scan" }, true);
    if (!state.connectedOnce) {
      state.connectedOnce = true;
      dashboard();
    }
  };
  ws.onclose = function () {
    if (state.socket !== ws) return;
    state.socket = null;
    set_led("led_ws", "bad");
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
    refresh_session().then(init_socket, function (err) {
      if (state.setupMode || err.status === 401) return;
      reconnect_socket(true); // device still down: keep trying
    });
  }, WS_RECONNECT_DELAY_MS);
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
    case "tx_started":
      txTracker.started(data);
      break;
    case "tx_result":
      txTracker.resolve(data);
      break;
    case "lora_rx":
      rx_led_blink();
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
      setValue("username", state.username);
      break;
    case "serial_mode": {
      state.serialMode = data.value === true || data.value === "true" || data.value === 1;
      const sw = byId("lora_to_serial");
      if (sw) sw.checked = state.serialMode;
      refresh_locks();
      break;
    }
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
  const li = el("li", own ? "pre-wrap own" : "pre-wrap");
  const now = new Date();
  const ts = el("time", "ts", now.toTimeString().slice(0, 8));
  ts.dateTime = now.toISOString();
  li.append(ts, el("b", "", sender));
  li.append(document.createTextNode(" : " + text));
  byId("lora_rx_msg").prepend(li);
}

/* ------------------------------------------------------------------ */
/* Server-sent events (Contract 4)                                     */
/* ------------------------------------------------------------------ */

function init_events() {
  if (state.setupMode || state.events) return;
  const source = new EventSource("/rawEvents");
  state.events = source;
  source.addEventListener("open", function () {
    set_led("led_sse", "on");
    console.log("Events Connected");
  });
  source.addEventListener("error", function () {
    // CONNECTING: the browser is retrying by itself. CLOSED: the server refused
    // the stream (e.g. 401, session gone), so get a new session and reopen.
    if (source.readyState !== EventSource.CLOSED) {
      set_led("led_sse", "warn");
      console.log("Events Disconnected");
      return;
    }
    set_led("led_sse", "bad");
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
    rx_led_blink();
    const p = receiver.progress();
    setText("chunk_ratio", "(" + p.received + " / " + p.total + ") " + p.sizeText + " received");
    set_progress(p.percent);
  });

  source.addEventListener("WIFI_DATA", function (e) {
    const d = L.safeJsonParse(e.data);
    const label = L.wifiLabel(d);
    if (label === null) return;
    setText("wifi_quality", label);
    const bars = L.wifiBars(d);
    const sig = byId("wifi_bars");
    if (sig && bars) {
      sig.setAttribute("data-level", String(bars.level));
      sig.setAttribute("data-mode", bars.mode);
    }
  });

  source.addEventListener("RAM_DATA", function (e) {
    const d = L.safeJsonParse(e.data);
    const h = d ? L.heapStatus(d.free_heap, d.heap_size) : null;
    if (!h) return;
    byId("ram_ratio").replaceChildren(
      el("b", "", h.percent + "%"),
      document.createTextNode(" (" + h.free + "/" + h.size + ")")
    );
    const bar = byId("heap_progress_bar");
    set_bar_class(bar, BAR_CLASS[h.cls]);
    bar.style.width = h.percent + "%";
  });

  source.addEventListener("DEBUG", function (e) {
    const ev = L.parseSseData(e.data);
    if (!ev || ev.data === "") return;
    // Drop only an exact repeat: several different lines can share one millis value.
    const key = ev.millis + "|" + ev.data;
    if (ev.millis !== undefined && key === state.lastDebugKey) return;
    state.lastDebugKey = key;
    const box = byId("debug_textarea");
    const now = new Date();
    const stamp = now.toTimeString().slice(0, 8) + "." + String(now.getMilliseconds()).padStart(3, "0");
    const text = stamp + " > " + ev.data + "\n" + (box.value || "");
    box.value = text.length > DEBUG_MAX_CHARS ? text.slice(0, DEBUG_MAX_CHARS) : text;
  });
}

function reconnect_events() {
  setTimeout(function () {
    if (state.setupMode || state.signInRequired || state.events) return;
    refresh_session().then(init_events, function (err) {
      if (state.setupMode || err.status === 401) return;
      reconnect_events();
    });
  }, SSE_RECONNECT_DELAY_MS);
}

/* ------------------------------------------------------------------ */
/* LoRa TX (Contract 2: 202 {id}; Contract 3: tx_result)               */
/* ------------------------------------------------------------------ */

function tx_http_error(err) {
  switch (err.status) {
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
      return { ok: false, fatal: false, reason: describe_err(err) };
  }
}

function tx_timeout_reason(phase) {
  return phase === "sending"
    ? "no result from the device after it started sending"
    : "still waiting in the device's send queue after " + L.TX_QUEUE_TIMEOUT_MS / 60000 + " min";
}

// cb({ok, fatal, reason, akn, username}); called exactly once.
// onState(state) (optional): "queued" after the 202, "sending" on tx_started (Contract 3).
function lora_tx(url, payload, cb, onState) {
  tx_led_begin();
  const done = cb;
  cb = function (r) {
    tx_led_end(r.ok);
    done(r);
  };
  request(url, { method: "POST", data: { data: payload }, as: "json" }).then(
    function (r) {
      const body = r.body;
      if (r.status !== 202 || !body || body.id === undefined || body.id === null) {
        cb({ ok: false, fatal: false, reason: "unexpected response from the device" });
        return;
      }
      // body.queued (optional) is accepted but not needed: tx_started says when sending begins.
      txTracker.register(body.id, function (t) {
        cb({
          ok: t.akn === 1,
          fatal: false,
          akn: t.akn,
          username: t.username,
          reason: t.timeout
            ? tx_timeout_reason(t.phase)
            : t.akn === 1
            ? ""
            : "not acknowledged by the receiver",
        });
      }, onState);
    },
    function (err) {
      cb(tx_http_error(err));
    }
  );
}

// Control messages (enable/disable file mode) get the same retry budget as chunks.
// isCancelled (optional): checked before each retry; a cancel ends with reason "cancelled".
function lora_tx_retry(url, payload, retries, cb, isCancelled, onState) {
  lora_tx(url, payload, function (r) {
    if (r.ok || r.fatal || retries <= 0) cb(r);
    else if (isCancelled && isCancelled()) cb({ ok: false, fatal: true, reason: "cancelled" });
    else
      setTimeout(function () {
        lora_tx_retry(url, payload, retries - 1, cb, isCancelled, onState);
      }, RETRY_DELAY_MS);
  }, onState);
}

function tx_state_text(txState) {
  return txState === "queued" ? "Waiting in queue..." : "Sending...";
}

// File transfer status line: "(sent / total) chunks sent, <what is happening now>".
// Leaves the "Stopping..." notice alone once Stop was pressed.
function show_send_stage(sent, total, what) {
  if (state.cancelSend) return;
  setText("chunk_ratio", "(" + sent + " / " + total + ") chunks sent, " + what);
}

function send_lora() {
  const box = byId("lora_msg");
  const msg = box.value || "";
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
  box.readOnly = true;
  setText("lora_send_status", "Sending...");
  lora_tx("/lora_transmit", inner, function (r) {
    box.readOnly = false;
    if (r.ok) {
      box.value = "";
      setText("lora_send_status", "Delivered.");
      add_message(r.username || state.username || "You", msg, true);
    } else {
      setText("lora_send_status", "");
      alertText("Message not delivered: " + r.reason + ".");
    }
  }, function (txState) {
    setText("lora_send_status", tx_state_text(txState));
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
  const input = byId("broadcastFile");
  const file = input && input.files && input.files[0];
  if (!file) {
    alertText("Choose a file first.");
    return;
  }
  const chunkRaw = getValue("chunk_size");
  if (!L.validateChunkSize(chunkRaw)) {
    alertText("Packet size must be a whole number from " + L.CHUNK_MIN + " to " + L.CHUNK_MAX + ".");
    return;
  }
  const chunkSize = Number(chunkRaw);
  const waitTime = Number(getValue("wait_time"));
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

    render_kv("rec_info", [
      ["Sending", file.name],
      ["Total size", sizeText],
      ["Chunks", String(total)],
      ["Estimated min time", time],
    ]);
    set_verify("");
    setText("chunk_ratio", "(0 / " + total + ") chunks sent");
    set_progress(0, "info");

    const sender = L.createChunkSender({
      chunks: chunks,
      waitMs: waitTime,
      retryDelayMs: RETRY_DELAY_MS,
      maxRetries: L.CHUNK_MAX_RETRIES,
      schedule: function (fn, ms) {
        setTimeout(fn, ms);
      },
      isCancelled: function () {
        return state.cancelSend;
      },
      send: function (chunk, i, done) {
        lora_tx("/send_raw", chunk, done, function (txState) {
          show_send_stage(i, total, txState === "queued"
            ? "chunk " + (i + 1) + " waiting in queue..."
            : "sending chunk " + (i + 1) + "...");
        });
      },
      onProgress: function (sent, tot) {
        setText("chunk_ratio", "(" + sent + " / " + tot + ") chunks sent");
        set_progress((sent / tot) * 100);
      },
      onFinish: function (res) {
        finish_send(res, sha);
      },
    });

    const cancelled = function () {
      return state.cancelSend;
    };
    lora_tx_retry("/lora_transmit", enable, L.CHUNK_MAX_RETRIES, function (r) {
      // (If it was delivered but Stop was pressed, the sender below finishes at once
      // as "cancelled" and finish_send still tells the receiver we're done.)
      if (!r.ok) {
        end_send();
        const msg = r.reason === "cancelled"
          ? "Transfer cancelled before the first chunk."
          : "Could not start the transfer: " + r.reason + ".";
        setText("chunk_ratio", "(0 / " + total + ") chunks sent");
        set_progress(0, "bad");
        set_verify(msg, false);
        alertText(msg);
        return;
      }
      // Give the receiving browser time to process enable_file_tx_mode (WS)
      // before the first chunk arrives on SSE.
      setTimeout(sender.start, RETRY_DELAY_MS);
    }, cancelled, function (txState) {
      show_send_stage(0, total, txState === "queued"
        ? "start message waiting in queue..."
        : "sending the start message...");
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
        set_progress(100, "ok");
      } else {
        msg = "Transfer aborted after " + res.sent + " of " + res.total + " chunks: " + res.reason + ".";
        set_progress((res.sent / Math.max(1, res.total)) * 100, "bad");
      }
      set_verify(msg, res.ok);
      alertText(msg);
    },
    null,
    function (txState) {
      if (res.ok)
        show_send_stage(res.sent, res.total, txState === "queued"
          ? "end message waiting in queue..."
          : "sending the end message...");
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
  setText("chunk_ratio", "Stopping after the current packet...");
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
  render_kv("rec_info", [
    ["Receiving from", msg.sender],
    ["File name", msg.fileName],
    ["Total size", msg.size],
    ["Chunks", String(msg.totalPackets)],
    ["Estimated min time", msg.time],
  ]);
  set_verify("");
  setText("chunk_ratio", "(0 / " + msg.totalPackets + ") 0 Bytes received");
  set_progress(0, "ok");
  refresh_locks();
}

function on_file_rx_end(msg) {
  if (!receiver.isActive()) return;
  const r = receiver.finish({ totalPackets: msg.totalPackets, sha256: msg.sha256 });
  if (r.verified) {
    set_verify("Verified: " + r.received + " packets, SHA-256 matches. Ready to download.", true);
  } else {
    set_verify("Corrupted: " + r.reason + ". Download is disabled.", false);
    set_progress(receiver.progress().percent, "bad");
  }
  refresh_locks();
}

function cancel_receive() {
  if (!receiver.cancel()) return;
  set_verify("Receiving cancelled.", false);
  refresh_locks();
}

function save_blob(blob, filename) {
  const link = document.createElement("a");
  link.href = URL.createObjectURL(blob);
  link.download = filename;
  link.click();
  setTimeout(function () {
    URL.revokeObjectURL(link.href);
  }, URL_REVOKE_DELAY_MS);
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
  save_blob(new Blob([decoded.bytes], { type: decoded.mime }), receiver.fileName() || "Voyager_Download");
}

/* ------------------------------------------------------------------ */
/* WiFi page                                                           */
/* ------------------------------------------------------------------ */

function set_scan_button(scanning) {
  state.scanning = scanning;
  const btn = byId("wifi_scan_btn");
  if (!btn) return;
  btn.textContent = scanning ? "Scanning..." : "Scan SSID";
  btn.disabled = scanning;
}

function scan_ssid() {
  byId("wifi_ssid_list").replaceChildren();
  if (!ws_send({ "request-type": "wifi_ssid_scan" })) return;
  set_scan_button(true);
  clearTimeout(state.scanTimer);
  state.scanTimer = setTimeout(function () {
    set_scan_button(false);
  }, SCAN_TIMEOUT_MS);
}

function render_wifi_scan(list) {
  clearTimeout(state.scanTimer);
  set_scan_button(false);
  const ul = byId("wifi_ssid_list");
  if (!ul) return;
  ul.replaceChildren();
  const visible = list.filter(function (n) {
    return n.ssid !== "";
  });
  if (visible.length === 0) {
    ul.append(el("li", "", "No networks found."));
    return;
  }
  visible.forEach(function (n) {
    const a = el("a", "", n.ssid);
    a.href = "#wifi_password";
    a.addEventListener("click", function (ev) {
      ev.preventDefault();
      setValue("wifi_ssid", n.ssid);
      byId("wifi_password").focus();
    });
    const li = el("li", "spread " + (WIFI_CLASS[n.cls] || ""));
    li.append(a, el("span", "pill", n.quality + " %"));
    ul.append(li);
  });
}

function wifi_connect() {
  const ssid = getValue("wifi_ssid").trim();
  const psk = getValue("wifi_password");
  // 802.11 limits an SSID to 32 bytes; the device stores it as UTF-8.
  const ssidBytes = new TextEncoder().encode(ssid).length;
  if (ssidBytes < 1 || ssidBytes > 32) {
    alertText("Enter a WiFi SSID of 1-32 characters, or choose one from the scan list.");
    return;
  }
  setValue("wifi_ssid", ssid);
  if (!L.validateWifiPassword(psk)) {
    alertText("The WiFi password must be empty (open network) or 8-63 printable characters.");
    return;
  }
  const go = function () {
    if (!ws_send({ "request-type": "connect_wifi", wifi_ssid: ssid, wifi_pass: psk })) return;
    set_loading("Connecting to WiFi...");
    schedule_redirect();
  };
  if (psk === "")
    confirm_prompt("Open network", 'Connect to "' + ssid + '" without a password?', go);
  else go();
}

function wifi_ap_mode() {
  confirm_prompt("Access Point", "Are you sure you want to switch to access point mode?", function () {
    const go = function (name) {
      if (!ws_send({ "request-type": "wifi_ap_mode" })) return;
      const ap = typeof name === "string" && name.trim() !== "" ? name.trim() : "Voyager";
      alertText(
        'Switched to AP mode.\nConnect to the "' + ap +
          '" access point. Its password is shown on the device OLED Network page.'
      );
      schedule_redirect();
    };
    request("/username").then(
      function (r) {
        go(r.body);
      },
      function () {
        go("");
      }
    );
  });
}

function set_ap_password() {
  const p = getValue("ap_password");
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
      setValue("ap_password", "");
      setText("ap_pass_status", "Request sent to the device.");
    }
  });
}

/* ------------------------------------------------------------------ */
/* LoRa page                                                           */
/* ------------------------------------------------------------------ */

function generate_sync_word() {
  const sel = byId("sync_word");
  sel.replaceChildren();
  for (let i = 0; i <= 255; i++) {
    const hex = ("0" + i.toString(16).toUpperCase()).slice(-2);
    sel.add(new Option(i + " (0x" + hex + ")", String(i)));
  }
}

function set_lora_config(cfg) {
  if (!cfg || !byId("freq_range")) return;
  const mhz = Number(cfg.freq) / 1000000;
  setValue("freq_range", mhz);
  setText("selected_lora_freq", String(mhz));
  setValue("tx_power_range", cfg.TxPower);
  setText("selected_lora_tx_power", L.toText(cfg.TxPower));
  setValue("spreading_factor", cfg.SpreadingFactor);
  setText("selected_lora_spreading_factor", L.toText(cfg.SpreadingFactor));
  setValue("bandwidth", String(cfg.SignalBandwidth));
  setValue("coding_rate", cfg.CodingRate4);
  setText("selected_lora_coding_rate", L.toText(cfg.CodingRate4));
  setValue("sync_word", String(cfg.SyncWord));
}

function read_lora_form() {
  return L.validateLoraConfig(
    L.loraConfigFromForm({
      freqMHz: getValue("freq_range"),
      txPower: getValue("tx_power_range"),
      spreadingFactor: getValue("spreading_factor"),
      bandwidth: getValue("bandwidth"),
      codingRate: getValue("coding_rate"),
      syncWord: getValue("sync_word"),
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
  const input = byId("LoraConfigFile");
  const file = input && input.files && input.files[0];
  if (!file) return;
  const reader = new FileReader();
  reader.onload = function () {
    input.value = "";
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
  if (L.isLoraPreset(name)) get_config(name).then(set_lora_config, function () {});
}

function JSONToFile(obj, filename) {
  save_blob(new Blob([JSON.stringify(obj, null, 2)], { type: "application/json" }), filename);
}

function set_freq_range(val) {
  setText("selected_lora_freq", val);
}
function set_tx_power(val) {
  setText("selected_lora_tx_power", val);
}
function set_spreading_factor(val) {
  setText("selected_lora_spreading_factor", val);
}
function set_coding_rate(val) {
  setText("selected_lora_coding_rate", val);
}
