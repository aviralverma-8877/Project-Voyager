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
  setupMode: false,
  signInRequired: false,
  sessionRequest: null,
  connectedOnce: false,
  lastDebugKey: null,
  scanTimer: null,
  txActive: 0,
  scanning: false,
};

let promptModal = null;
let alertModal = null;
let loadingModal = null;

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

// Locks: .lora_transmission (dashboard messaging) while the LoRa-serial bridge owns
// the radio link (serial mode).
function refresh_locks() {
  set_fieldsets_disabled("lora_transmission", state.serialMode);
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
    request_net_status();
  });
}

// Dashboard "LoRa network" panel: refreshed every 5 s while the dashboard is shown.
let netStatusTimer = null;
function request_net_status() {
  if (netStatusTimer) clearTimeout(netStatusTimer);
  netStatusTimer = null;
  if (!byId("net_status")) return; // left the dashboard
  ws_send({ "request-type": "get_net_status" }, true);
  netStatusTimer = setTimeout(request_net_status, 5000);
}

function render_net_status(d) {
  const box = byId("net_status");
  if (!box) return;
  const s = L.parseNetStatus(d);
  if (!s) return;
  box.textContent = s.active
    ? "Joined: a PC uses this board as " + s.ip + " (" + s.tx + " packets out, " + s.rx + " in, " + s.drop + " dropped)."
    : "This board's address is " + s.ip + ". No PC has joined through it.";
  const ul = byId("net_peers");
  ul.replaceChildren();
  s.peers.forEach(function (p) {
    const li = el("li", "spread");
    li.append(el("span", "", p.ip), el("span", "hint", p.rssi + " dBm, " + p.age + " s ago"));
    ul.append(li);
  });
  if (s.peers.length === 0) ul.append(el("li", "hint", "No other network nodes heard yet."));
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

function file_transfer() {
  load_page("file_transfer.html", "navbar-file-transfer", function () {
    serialTransfer.init();
  });
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
    case "net_status":
      render_net_status(data);
      break;
    case "serial_mode": {
      state.serialMode = data.value === true || data.value === "true" || data.value === 1;
      const sw = byId("lora_to_serial");
      if (sw) sw.checked = state.serialMode;
      refresh_locks();
      break;
    }
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
  if (msg.action === "sos") alertText("User " + msg.sender + " has sent a SOS request.");
  // Serial (Kermit) transfer handshake. Invites are kept even while another page is
  // shown, so they are listed when the user opens File Transfer.
  else if (msg.action.indexOf("file_tx_") === 0) serialTransfer.handleLoraAction(msg);
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

// lora_tx with up to `retries` more attempts (serial-transfer handshake messages).
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

function save_blob(blob, filename) {
  const link = document.createElement("a");
  link.href = URL.createObjectURL(blob);
  link.download = filename;
  link.click();
  setTimeout(function () {
    URL.revokeObjectURL(link.href);
  }, URL_REVOKE_DELAY_MS);
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

/* ------------------------------------------------------------------ */
/* File Transfer (Kermit over USB serial, relayed over LoRa)           */
/* ------------------------------------------------------------------ */
//
// Each browser runs Kermit against its own device's USB serial port; the devices
// relay the bytes as LORA_SERIAL packets while the bridge is on (file_tx_start).
// The serial port is reached through one of two transports with the same shape:
//   - Web Serial, when the page is a secure context (https / localhost);
//   - Voyager Link (tools/voyager-link), a tray helper on ws://127.0.0.1, for the
//     normal case of this page being served over plain http by the device.
// Transport: {kind, label, needsPortList, listPorts(), open(name), write(u8),
//             onData(cb) -> unsubscribe, onClose(cb), close()}
//
// Handshake (LORA_MSG actions via /lora_transmit, see lib.js buildFileTx*):
//   sender: file_tx_invite -> receivers: file_tx_join{nonce} -> sender picks ONE:
//   file_tx_go{nonce}. The transfer is point-to-point, so only that receiver turns its
//   bridge on; the others go back to waiting.

var HELPER_PORTS = [47321, 47322, 47323, 47324, 47325]; // = helperPorts in tools/voyager-link/server.go
var HELPER_DOWNLOAD_URL =
  "https://aviralverma-8877.github.io/Project-Voyager/webflash/voyager-link.exe";
var SERIAL_BAUD = 115200;
// Time for file_tx_start (WS) to reach the device before Kermit bytes do: while the
// bridge is off the device discards serial input, and debug lines stop only once it is on.
var BRIDGE_SETTLE_MS = 400;

function webSerialAvailable() {
  return !!(window.isSecureContext && navigator.serial);
}

function makeListeners() {
  var list = [];
  return {
    add: function (cb) {
      list.push(cb);
      return function () {
        var i = list.indexOf(cb);
        if (i >= 0) list.splice(i, 1);
      };
    },
    emit: function (data) {
      list.slice().forEach(function (cb) {
        try {
          cb(data);
        } catch (e) {
          console.error("Serial data callback error:", e);
        }
      });
    },
    clear: function () {
      list = [];
    },
  };
}

function createWebSerialTransport() {
  var port = null, reader = null, writer = null, closing = false, closeCb = null;
  var listeners = makeListeners();

  async function readLoop() {
    try {
      while (reader) {
        var r = await reader.read();
        if (r.done) break;
        if (r.value) listeners.emit(r.value);
      }
    } catch (e) {
      if (!closing) console.error("Serial read error:", e);
    }
    if (!closing && closeCb) closeCb("serial port lost");
  }

  return {
    kind: "webserial",
    label: "Web Serial",
    needsPortList: false,
    listPorts: function () {
      return Promise.resolve([]);
    },
    open: async function () {
      port = await navigator.serial.requestPort();
      await port.open({ baudRate: SERIAL_BAUD });
      // Chrome raises DTR/RTS on open; on ESP32 boards they drive the auto-reset circuit.
      try {
        await port.setSignals({ dataTerminalReady: false, requestToSend: false });
      } catch (e) {}
      closing = false;
      reader = port.readable.getReader();
      writer = port.writable.getWriter();
      readLoop();
      return "serial port";
    },
    write: function (bytes) {
      if (!writer) return;
      writer.write(bytes).catch(function (e) {
        console.error("Serial write error:", e);
      });
    },
    onData: listeners.add,
    onClose: function (cb) {
      closeCb = cb;
    },
    close: async function () {
      closing = true;
      listeners.clear();
      try {
        if (reader) {
          await reader.cancel().catch(function () {});
          reader.releaseLock();
        }
        if (writer) writer.releaseLock();
        if (port) await port.close();
      } catch (e) {
        console.error("Serial close error:", e);
      }
      reader = writer = port = null;
    },
  };
}

function fetchWithTimeout(url, ms) {
  var ctl = typeof AbortController !== "undefined" ? new AbortController() : null;
  return new Promise(function (resolve, reject) {
    var t = setTimeout(function () {
      if (ctl) ctl.abort();
      reject(new Error("timeout"));
    }, ms);
    fetch(url, { cache: "no-store", signal: ctl ? ctl.signal : undefined }).then(
      function (r) {
        clearTimeout(t);
        resolve(r);
      },
      function (e) {
        clearTimeout(t);
        reject(e);
      }
    );
  });
}

// Resolves {port, version} for a running Voyager Link, or null. A closed port fails
// at once; the long timeout only covers Chrome's "access apps on this device" prompt.
function findHelper() {
  var i = 0;
  return new Promise(function (resolve) {
    (function next() {
      if (i >= HELPER_PORTS.length) {
        resolve(null);
        return;
      }
      var p = HELPER_PORTS[i++];
      fetchWithTimeout("http://127.0.0.1:" + p + "/info", 20000)
        .then(function (r) {
          return r.json();
        })
        .then(function (info) {
          if (info && info.app === "voyager-link") resolve({ port: p, version: L.toText(info.version) });
          else next();
        }, next);
    })();
  });
}

function createHelperTransport(helper) {
  var ws = null, pending = null, closing = false, closeCb = null;
  var listeners = makeListeners();

  function lost(reason) {
    if (!closing && closeCb) closeCb(reason);
  }

  function connectWs() {
    if (ws && ws.readyState === 1) return Promise.resolve();
    return new Promise(function (resolve, reject) {
      var sock = new WebSocket("ws://127.0.0.1:" + helper.port + "/serial");
      var opened = false;
      sock.binaryType = "arraybuffer";
      sock.onopen = function () {
        opened = true;
        ws = sock;
        resolve();
      };
      sock.onerror = function () {
        if (!opened) reject(new Error("could not connect to Voyager Link"));
      };
      sock.onclose = function (ev) {
        if (ws === sock) ws = null;
        if (pending) {
          pending.reject(new Error("Voyager Link closed the connection"));
          pending = null;
        }
        if (opened) lost(ev.code === 1013 ? "too many pages are using Voyager Link" : "Voyager Link stopped");
      };
      sock.onmessage = function (ev) {
        if (typeof ev.data !== "string") {
          listeners.emit(new Uint8Array(ev.data));
          return;
        }
        var m = L.safeJsonParse(ev.data);
        if (!m) return;
        if (m.op === "closed") {
          lost(L.toText(m.reason) || "serial port closed");
          return;
        }
        if (pending && (m.op === pending.expect || m.op === "error")) {
          var p = pending;
          pending = null;
          if (m.op === "error") p.reject(new Error(L.toText(m.msg)));
          else p.resolve(m);
        } else if (m.op === "error") {
          console.warn("Voyager Link:", m.msg);
        }
      };
    });
  }

  function call(obj, expect) {
    return connectWs().then(function () {
      return new Promise(function (resolve, reject) {
        pending = { resolve: resolve, reject: reject, expect: expect };
        ws.send(JSON.stringify(obj));
      });
    });
  }

  return {
    kind: "helper",
    label: "Voyager Link " + helper.version,
    needsPortList: true,
    listPorts: function () {
      return call({ op: "list" }, "ports").then(function (m) {
        return Array.isArray(m.ports) ? m.ports : [];
      });
    },
    open: function (name) {
      closing = false;
      return call({ op: "open", port: name, baud: SERIAL_BAUD }, "opened").then(function () {
        return name;
      });
    },
    write: function (bytes) {
      if (ws && ws.readyState === 1) ws.send(bytes);
    },
    onData: listeners.add,
    onClose: function (cb) {
      closeCb = cb;
    },
    close: function () {
      closing = true;
      listeners.clear();
      if (ws) {
        try {
          ws.send(JSON.stringify({ op: "close" }));
          ws.close();
        } catch (e) {}
        ws = null;
      }
      return Promise.resolve();
    },
  };
}

function randomId() {
  var b = new Uint8Array(4);
  if (window.crypto && crypto.getRandomValues) crypto.getRandomValues(b);
  else for (var i = 0; i < 4; i++) b[i] = Math.floor(Math.random() * 256);
  return Array.prototype.map
    .call(b, function (x) {
      return ("0" + x.toString(16)).slice(-2);
    })
    .join("");
}

var serialTransfer = (function () {
  var transport = null;
  var connected = false;
  var transferActive = false;
  var role = "sender";
  var kermitTimeout = 30000;
  var kermitInstance = null;

  // sender
  var selectedFile = null;
  var txId = "";
  var participants = []; // {name, mac, nonce}
  var chosenNonce = "";

  // receiver
  var invitations = []; // {txId, sender, mac, fileName, size}
  var joined = null; // {txId, nonce, inv}
  var receivedData = null;
  var receivedFileName = "";

  function init() {
    // Page re-entered: drop whatever the previous visit left open.
    if (kermitInstance) kermitInstance.cancel();
    kermitInstance = null;
    if (transport) transport.close();
    transport = null;
    var keepInvitations = invitations; // heard while another page was shown
    resetState();
    invitations = keepInvitations;

    byId("helperDownloadLink").href = HELPER_DOWNLOAD_URL;
    byId("helperRetryBtn").addEventListener("click", detectTransport);
    byId("connectSerialBtn").addEventListener("click", connect);
    byId("refreshPortsBtn").addEventListener("click", refreshPorts);
    byId("disconnectSerialBtn").addEventListener("click", disconnect);
    byId("inviteBtn").addEventListener("click", sendInvitation);
    byId("startSerialTransferBtn").addEventListener("click", startTransfer);
    byId("cancelSerialTransferBtn").addEventListener("click", function () {
      endTransfer(false, "Transfer cancelled.", true);
    });
    byId("serialDownloadBtn").addEventListener("click", downloadReceivedFile);
    byId("serialResetBtn").addEventListener("click", resetTransfer);
    document.querySelectorAll('input[name="serial_role"]').forEach(function (radio) {
      radio.addEventListener("change", function () {
        role = radio.value === "receiver" ? "receiver" : "sender";
        updateUI();
      });
    });
    var fileInput = byId("serialSendFile");
    fileInput.addEventListener("change", function () {
      selectedFile = (fileInput.files && fileInput.files[0]) || null;
      txId = "";
      participants = [];
      chosenNonce = "";
      byId("participantsSection").hidden = true;
      setText("serialSenderStatus", "");
      updateUI();
    });

    // Block timeout follows the radio settings (SF12 needs minutes, not 30 s).
    get_config("lora_config.json").then(
      function (cfg) {
        kermitTimeout = L.kermitTimeoutMs(cfg);
      },
      function () {}
    );

    renderInvitations();
    updateUI();
    detectTransport();
  }

  function resetState() {
    connected = false;
    transferActive = false;
    role = "sender";
    selectedFile = null;
    txId = "";
    participants = [];
    chosenNonce = "";
    invitations = [];
    joined = null;
    receivedData = null;
    receivedFileName = "";
  }

  /* ---------- connection ---------- */

  function detectTransport() {
    byId("helperMissing").hidden = true;
    setDisabled("connectSerialBtn", true);
    if (webSerialAvailable()) {
      useTransport(createWebSerialTransport(), "Using the browser's Web Serial.");
      return;
    }
    setText("serialLinkStatus", "Looking for Voyager Link on this computer...");
    findHelper().then(function (helper) {
      if (!byId("serialLinkStatus")) return; // page changed meanwhile
      if (!helper) {
        setText("serialLinkStatus", "Voyager Link is not running.");
        byId("helperMissing").hidden = false;
        return;
      }
      useTransport(createHelperTransport(helper), "Connected to Voyager Link " + helper.version + ".");
      refreshPorts();
    });
  }

  function useTransport(t, statusText) {
    transport = t;
    transport.onClose(function (reason) {
      if (!connected) return;
      connected = false;
      setText("serialStatus", "Disconnected: " + reason);
      if (transferActive) endTransfer(false, "Serial link lost: " + reason, true);
      updateUI();
    });
    setText("serialLinkStatus", statusText);
    byId("serialPortField").hidden = !t.needsPortList;
    byId("refreshPortsBtn").hidden = !t.needsPortList;
    setDisabled("connectSerialBtn", t.needsPortList); // enabled once ports are listed
  }

  function refreshPorts() {
    if (!transport || !transport.needsPortList) return;
    transport.listPorts().then(
      function (ports) {
        var sel = byId("serialPortSelect");
        if (!sel) return;
        sel.replaceChildren();
        // Likely ESP32 bridges (CP210x, CH340, ...) first.
        ports.sort(function (a, b) {
          return (b.likely ? 1 : 0) - (a.likely ? 1 : 0);
        });
        ports.forEach(function (p) {
          var label = L.toText(p.name) + (p.desc ? " - " + L.toText(p.desc) : "");
          if (p.likely) label += " (Voyager?)";
          var opt = el("option", "", label);
          opt.value = L.toText(p.name);
          sel.append(opt);
        });
        if (ports.length === 0) {
          var none = el("option", "", "No serial ports found - plug in the device");
          none.value = "";
          sel.append(none);
        }
        setDisabled("connectSerialBtn", ports.length === 0 || connected);
      },
      function (e) {
        setText("serialStatus", "Could not list ports: " + e.message);
      }
    );
  }

  async function connect() {
    if (!transport) return;
    var name = transport.needsPortList ? byId("serialPortSelect").value : "";
    if (transport.needsPortList && !name) return;
    setDisabled("connectSerialBtn", true);
    setText("serialStatus", "Connecting...");
    try {
      var what = await transport.open(name);
      connected = true;
      setText("serialStatus", "Connected to " + what + " (" + transport.label + ")");
    } catch (e) {
      connected = false;
      setText(
        "serialStatus",
        e && e.name === "NotFoundError" ? "No port selected" : "Connection failed: " + ((e && e.message) || "unknown error")
      );
    }
    updateUI();
  }

  async function disconnect() {
    if (transferActive) endTransfer(false, "Transfer cancelled.", true);
    connected = false;
    if (transport) await transport.close();
    setText("serialStatus", "Disconnected");
    updateUI();
    refreshPorts();
  }

  /* ---------- handshake ---------- */

  function sendInvitation() {
    if (!selectedFile || !connected) return;
    txId = randomId();
    participants = [];
    chosenNonce = "";
    renderParticipants();
    var inner = L.buildFileTxInvite(txId, selectedFile.name, selectedFile.size);
    var chk = L.checkPayload(state.username, inner);
    if (!chk.ok) {
      alertText("File name too long to announce over LoRa. Rename the file and try again.");
      return;
    }
    byId("participantsSection").hidden = false;
    setDisabled("inviteBtn", true);
    setText("serialSenderStatus", "Broadcasting invitation...");
    lora_tx("/lora_transmit", inner, function (r) {
      setDisabled("inviteBtn", false);
      setText(
        "serialSenderStatus",
        r.ok
          ? "Invitation delivered. Waiting for a receiver to accept..."
          : "No device acknowledged the invitation (" + r.reason + "). Receivers may still see it; you can broadcast again."
      );
      updateUI();
    });
  }

  // lora_rx actions file_tx_* (called from handle_lora_rx).
  function handleLoraAction(msg) {
    if (msg.action === "file_tx_invite") onInvite(msg);
    else if (msg.action === "file_tx_join") onJoin(msg);
    else if (msg.action === "file_tx_go") onGo(msg);
  }

  function onInvite(msg) {
    if (!msg.txId) return;
    for (var i = 0; i < invitations.length; i++) if (invitations[i].txId === msg.txId) return;
    invitations.push({
      txId: msg.txId,
      sender: msg.sender,
      mac: msg.mac,
      fileName: msg.fileName || "file",
      size: Number(msg.size) || 0,
    });
    if (byId("invitationList")) {
      renderInvitations();
      updateUI();
    }
  }

  function onJoin(msg) {
    if (!txId || msg.txId !== txId || !msg.nonce || transferActive) return;
    for (var i = 0; i < participants.length; i++) if (participants[i].nonce === msg.nonce) return;
    participants.push({ name: msg.sender, mac: msg.mac, nonce: msg.nonce });
    if (!chosenNonce) chosenNonce = msg.nonce;
    renderParticipants();
    updateUI();
  }

  function onGo(msg) {
    if (!joined || msg.txId !== joined.txId || transferActive) return;
    if (msg.nonce !== joined.nonce) {
      setText("serialReceiverStatus", joined.inv.sender + " chose another receiver for " + joined.inv.fileName + ".");
      joined = null;
      renderInvitations();
      return;
    }
    startReceiving(joined.inv);
  }

  function acceptInvitation(inv) {
    if (!connected) {
      alertText("Connect the serial port first.");
      return;
    }
    var nonce = randomId();
    joined = { txId: inv.txId, nonce: nonce, inv: inv };
    renderInvitations();
    setText("serialReceiverStatus", "Joining " + inv.sender + "'s transfer...");
    lora_tx_retry("/lora_transmit", L.buildFileTxJoin(inv.txId, nonce), 2, function (r) {
      if (!joined || joined.nonce !== nonce || transferActive) return;
      if (r.ok) {
        setText("serialReceiverStatus", "Joined. Waiting for " + inv.sender + " to start the transfer...");
      } else {
        joined = null;
        renderInvitations();
        setText("serialReceiverStatus", "Could not reach " + inv.sender + ": " + r.reason + ".");
      }
    });
  }

  /* ---------- transfer ---------- */

  function startTransfer() {
    var p = chosenParticipant();
    if (!selectedFile || !p || !connected || transferActive) return;
    transferActive = true;
    receivedData = null;
    showProgressStep();
    setSerialProgress(0);
    setText("serialTransferStatus", "Preparing " + selectedFile.name + "...");

    var fr = new FileReader();
    fr.onerror = function () {
      endTransfer(false, "Could not read the file.", false);
    };
    fr.onload = function () {
      if (!transferActive) return;
      var data = new Uint8Array(fr.result);
      ws_send({ "request-type": "file_tx_start" });
      setTimeout(function () {
        if (!transferActive) return;
        setText("serialTransferStatus", "Asking " + p.name + " to start...");
        lora_tx_retry(
          "/lora_transmit",
          L.buildFileTxGo(txId, p.nonce),
          2,
          function (r) {
            if (!transferActive) return;
            if (!r.ok) {
              endTransfer(false, "Could not reach " + p.name + ": " + r.reason + ".", false);
              return;
            }
            // Kermit's sender speaks first (S packet), so start once the receiver has
            // the "go" and is turning its bridge on; a lost first S is simply resent.
            setText("serialTransferStatus", "Connecting to " + p.name + "...");
            kermitInstance = KERMIT.createSender({
              file: { name: selectedFile.name, size: data.length, data: data },
              write: transport.write,
              onData: transport.onData,
              timeout: kermitTimeout,
              onProgress: function (sent, total) {
                setSerialProgress(total > 0 ? (sent / total) * 100 : 0);
                setText("serialTransferStatus", "Sending: " + L.formatSize(sent) + " / " + L.formatSize(total));
              },
              onComplete: function (ok, err) {
                kermitInstance = null;
                endTransfer(ok, ok ? "File sent to " + p.name + "." : "Transfer failed: " + (err || "unknown error"), false);
              },
            });
            kermitInstance.start();
          },
          function () {
            return !transferActive;
          }
        );
      }, BRIDGE_SETTLE_MS);
    };
    fr.readAsArrayBuffer(selectedFile);
  }

  function startReceiving(inv) {
    transferActive = true;
    receivedData = null;
    receivedFileName = inv.fileName;
    showProgressStep();
    setSerialProgress(0);
    setText("serialTransferStatus", "Starting to receive " + inv.fileName + " from " + inv.sender + "...");
    ws_send({ "request-type": "file_tx_start" });
    setTimeout(function () {
      if (!transferActive) return;
      kermitInstance = KERMIT.createReceiver({
        write: transport.write,
        onData: transport.onData,
        timeout: kermitTimeout,
        onFileStart: function (name) {
          receivedFileName = name || inv.fileName;
          setText("serialTransferStatus", "Receiving " + receivedFileName + "...");
        },
        onProgress: function (got, total) {
          var t = total > 0 ? total : inv.size;
          setSerialProgress(t > 0 ? (got / t) * 100 : 0);
          setText("serialTransferStatus", "Receiving: " + L.formatSize(got) + " / " + L.formatSize(t));
        },
        onComplete: function (ok, data, err) {
          kermitInstance = null;
          if (ok && data) receivedData = data;
          endTransfer(
            ok && !!data,
            ok ? "Received " + receivedFileName + " (" + L.formatSize(data ? data.length : 0) + ")." : "Receive failed: " + (err || "unknown error"),
            false
          );
        },
      });
      kermitInstance.start();
    }, BRIDGE_SETTLE_MS);
  }

  // cancel=true aborts a running Kermit session (sends an E packet to the peer).
  function endTransfer(success, message, cancel) {
    if (cancel && kermitInstance) kermitInstance.cancel();
    kermitInstance = null;
    var wasActive = transferActive;
    transferActive = false;
    joined = null;
    if (wasActive) ws_send({ "request-type": "file_tx_end" });
    if (!byId("serial_step_result")) return; // page changed meanwhile
    byId("serial_step_progress").hidden = true;
    byId("serial_step_result").hidden = false;
    var resultEl = byId("serialResultMessage");
    resultEl.textContent = message;
    resultEl.classList.toggle("text-ok", success);
    resultEl.classList.toggle("text-bad", !success);
    byId("serialDownloadBtn").hidden = !(success && receivedData);
  }

  function downloadReceivedFile() {
    if (!receivedData) return;
    save_blob(new Blob([receivedData]), receivedFileName || "Voyager_Serial_Download");
  }

  function resetTransfer() {
    txId = "";
    participants = [];
    chosenNonce = "";
    invitations = [];
    joined = null;
    receivedData = null;
    byId("serial_step_result").hidden = true;
    byId("serial_step_progress").hidden = true;
    byId("serialDownloadBtn").hidden = true;
    byId("participantsSection").hidden = true;
    setText("serialSenderStatus", "");
    setText("serialReceiverStatus", "");
    renderInvitations();
    updateUI();
  }

  /* ---------- rendering ---------- */

  function chosenParticipant() {
    for (var i = 0; i < participants.length; i++) if (participants[i].nonce === chosenNonce) return participants[i];
    return null;
  }

  function updateUI() {
    if (!byId("serial_step_role")) return;
    var busy = transferActive || !byId("serial_step_result").hidden;
    byId("serial_step_role").disabled = !connected || transferActive;
    byId("serial_step_sender").hidden = busy || !connected || role !== "sender";
    byId("serial_step_receiver").hidden = busy || !connected || role !== "receiver";
    byId("disconnectSerialBtn").hidden = !connected;
    setDisabled("connectSerialBtn", connected || !transport || (transport.needsPortList && !byId("serialPortSelect").value));
    setDisabled("serialPortSelect", connected);
    setDisabled("inviteBtn", !selectedFile || !connected);
    setDisabled("startSerialTransferBtn", !selectedFile || !connected || !chosenParticipant());
    byId("participantsEmpty").hidden = participants.length > 0;
    byId("receiverWaiting").hidden = invitations.length > 0;
    byId("invitationsSection").hidden = invitations.length === 0;
  }

  function showProgressStep() {
    byId("serial_step_sender").hidden = true;
    byId("serial_step_receiver").hidden = true;
    byId("serial_step_result").hidden = true;
    byId("serial_step_progress").hidden = false;
    byId("serial_step_role").disabled = true;
  }

  function setSerialProgress(percent) {
    var bar = byId("serialTransferProgress");
    var p = Math.max(0, Math.min(100, Number(percent) || 0));
    bar.style.width = p + "%";
    bar.setAttribute("aria-valuenow", String(Math.round(p)));
  }

  function renderParticipants() {
    var ul = byId("participantList");
    if (!ul) return;
    ul.replaceChildren();
    participants.forEach(function (p) {
      var li = el("li", "spread");
      var label = el("label", "switch");
      var radio = el("input");
      radio.type = "radio";
      radio.name = "serial_participant";
      radio.checked = p.nonce === chosenNonce;
      radio.addEventListener("change", function () {
        chosenNonce = p.nonce;
        updateUI();
      });
      label.append(radio, el("span", "", p.name || p.mac));
      li.append(label);
      ul.append(li);
    });
  }

  function renderInvitations() {
    var ul = byId("invitationList");
    if (!ul) return;
    ul.replaceChildren();
    invitations.forEach(function (inv) {
      var li = el("li", "spread");
      var info = el("span", "");
      info.append(el("b", "", inv.fileName));
      info.append(document.createTextNode(" from " + inv.sender + " (" + L.formatSize(inv.size) + ")"));
      li.append(info);
      var mine = joined && joined.txId === inv.txId;
      var btn = el("button", "btn primary", mine ? "Joined" : "Accept");
      btn.type = "button";
      btn.disabled = !!joined;
      btn.addEventListener("click", function () {
        acceptInvitation(inv);
      });
      li.append(btn);
      ul.append(li);
    });
    updateUI();
  }

  return {
    init: init,
    handleLoraAction: handleLoraAction,
  };
})();
