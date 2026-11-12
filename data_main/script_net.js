/*
 * Project Voyager web UI - WebSocket, server-sent events and LoRa TX.
 * Function declarations only: the shared constants and `state` live in
 * script.js, which index.html loads last (see the loader there).
 */
"use strict";

/* ------------------------------------------------------------------ */
/* WebSocket (Contract 3)                                             */
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
    ws_send({ "request-type": "get_sensor_config" }, true);
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
    case "sensor_config":
      on_sensor_config(data);
      break;
    case "sensor_data":
      state.sensorLocal = { readings: L.parseSensorData(data.data), time: new Date() };
      render_sensor_local();
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
  if (msg.packType === "sensor") {
    on_sensor_rx(msg);
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
/* Server-sent events (Contract 4)                                    */
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
  }, SSE_RECONNECT_DELAY_MS);
}

/* ------------------------------------------------------------------ */
/* LoRa TX (Contract 2: 202 {id}; Contract 3: tx_result)              */
/* ------------------------------------------------------------------ */

function tx_http_error(xhr) {
  switch (xhr.status) {
    case 400:
      return { ok: false, fatal: true, reason: "the device rejected the request (400)" };
    case 413:
      return { ok: false, fatal: true, reason: "message too long" };
    case 503:
      return { ok: false, fatal: false, reason: "device busy, retry" };
    case 409:
      return { ok: false, fatal: true, reason: "sensor broadcast is on, file transfer is off" };
    case 401:
      return { ok: false, fatal: true, reason: "not signed in" };
    case 403:
      return { ok: false, fatal: true, reason: "forbidden (403)" };
    default:
      return { ok: false, fatal: false, reason: describe_xhr(xhr) };
  }
}

// cb({ok, fatal, reason, akn, noack, username}); called exactly once.
// noAck (only /lora_transmit): broadcast once, don't wait for an acknowledgement.
function lora_tx(url, payload, cb, noAck) {
  const form = { data: payload };
  if (noAck) form.ack = "0";
  $.ajax({
    url: url,
    method: "POST",
    data: form,
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
          noack: !!r.noack,
          username: r.username,
          reason: r.timeout
            ? "no result from the device within " + L.TX_RESULT_TIMEOUT_MS / 1000 + " s"
            : r.akn === 1
            ? ""
            : noAck
            ? "the radio was busy"
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
      }, RETRY_DELAY_MS);
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
  const noAck = !state.msgAck;
  box.prop("readonly", true);
  $("#lora_send_status").text("Sending...");
  lora_tx(
    "/lora_transmit",
    inner,
    function (r) {
      box.prop("readonly", false);
      if (r.ok) {
        box.val("");
        $("#lora_send_status").text(noAck ? "Sent (no acknowledgement requested)." : "Delivered.");
        add_message(r.username || state.username || "You", msg, true);
      } else {
        $("#lora_send_status").text("");
        alertText("Message not " + (noAck ? "sent" : "delivered") + ": " + r.reason + ".");
      }
    },
    noAck
  );
}

function set_msg_ack(val) {
  state.msgAck = !!val;
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
