/*
 * Project Voyager web UI - Sensors page (sensor broadcast mode).
 *
 * Function declarations only: `state`, `L` and the DOM helpers (byId, el,
 * setText, ws_send, alertText, confirm_prompt, load_page, refresh_locks) live
 * in script.js, which index.html loads after this file.
 *
 * Rule (same as script.js): sensor names, sender names and readings come from
 * the radio, so they only ever go into the DOM as text nodes.
 */
"use strict";

var SENSOR_RX_MAX_ROWS = 40;

function sensors() {
  load_page("sensors.html", "navbar-sensors", function () {
    render_sensor_form();
    render_sensor_local();
    render_sensor_rx();
    ws_send({ "request-type": "get_sensor_config" }, true);
    // Fresh readings every time the page is opened.
    if (ws_send({ "request-type": "read_sensors" }, true)) setText("sensor_read_status", "Reading...");
  });
}

// WS sensor_config: {config:{enabled,interval,ack,sensors}, pins:{io,in,adc}}
function on_sensor_config(data) {
  const cfg = data && data.config;
  if (!cfg || typeof cfg !== "object") return;
  const pins = (data && data.pins) || {};
  const list = function (v) {
    return Array.isArray(v) ? v.filter(Number.isInteger) : [];
  };
  state.sensorPins = { io: list(pins.io), in: list(pins.in), adc: list(pins.adc) };
  state.sensorCfg = cfg;
  state.sensorMode = cfg.enabled === true;
  render_sensor_form();
  if (!state.sensorLocal) render_sensor_local(); // "No sensors saved" vs "Press Read now"
  refresh_locks();
}

function fill_pin_select(sel, pins, selected) {
  sel.replaceChildren(new Option("GPIO...", ""));
  pins.forEach(function (pin) {
    sel.append(new Option("GPIO " + pin, String(pin)));
  });
  sel.value = pins.indexOf(Number(selected)) >= 0 && selected !== "" ? String(selected) : "";
}

function sensor_field(labelText, control) {
  const wrap = el("div", "field");
  const label = el("label", "label", labelText);
  wrap.append(label, control);
  return { wrap: wrap, label: label };
}

// One editable sensor row. `s` may be null for a new, empty row.
function sensor_row(s) {
  const row = el("div", "section sensor_row");
  const grid = el("div", "grid-2");

  const type = el("select", "select sensor_type");
  L.SENSOR_TYPES.forEach(function (t) {
    type.append(new Option(t.label, t.key));
  });
  const name = el("input", "input sm sensor_name");
  name.type = "text";
  name.maxLength = 12;
  name.placeholder = "e.g. room";
  name.autocomplete = "off";
  const pin = el("select", "select sensor_pin");
  const pin2 = el("select", "select sensor_pin2");

  const pinField = sensor_field("Data pin", pin);
  const pin2Field = sensor_field("Echo pin", pin2);

  const apply_type = function (selPin, selPin2) {
    const t = L.sensorType(type.value);
    pinField.label.textContent = t.pin2 ? "Trigger pin" : t.pins === "adc" ? "Analog pin" : "Data pin";
    fill_pin_select(pin, state.sensorPins[t.pins] || [], selPin);
    pin2Field.wrap.hidden = !t.pin2;
    fill_pin_select(pin2, t.pin2 ? state.sensorPins[t.pin2] || [] : [], selPin2);
  };
  type.addEventListener("change", function () {
    apply_type("", "");
  });

  if (s && L.sensorType(s.type)) type.value = s.type;
  name.value = s ? L.toText(s.name) : "";
  apply_type(s ? s.pin : "", s ? s.pin2 : "");

  const remove = el("button", "btn danger", "Remove");
  remove.type = "button";
  remove.addEventListener("click", function () {
    row.remove();
    sensor_rows_changed();
  });
  const actions = el("div", "actions");
  actions.append(remove);

  grid.append(sensor_field("Sensor", type).wrap, sensor_field("Name", name).wrap, pinField.wrap, pin2Field.wrap);
  row.append(grid, actions);
  return row;
}

function sensor_rows_changed() {
  const n = document.querySelectorAll("#sensor_rows .sensor_row").length;
  const empty = byId("sensor_rows_empty");
  if (empty) empty.hidden = n > 0;
  setDisabled("sensor_add_btn", n >= L.SENSOR_MAX);
}

function sensor_add_row() {
  const box = byId("sensor_rows");
  if (!box || box.querySelectorAll(".sensor_row").length >= L.SENSOR_MAX) return;
  box.append(sensor_row(null));
  sensor_rows_changed();
}

function render_sensor_form() {
  const box = byId("sensor_rows");
  if (!box) return; // another page is showing
  const cfg = state.sensorCfg || { enabled: false, interval: 30, ack: false, sensors: [] };
  byId("sensor_enabled").checked = cfg.enabled === true;
  byId("sensor_ack").checked = cfg.ack === true;
  byId("sensor_interval").value = Number.isInteger(cfg.interval) ? cfg.interval : 30;
  box.replaceChildren();
  (Array.isArray(cfg.sensors) ? cfg.sensors : []).slice(0, L.SENSOR_MAX).forEach(function (s) {
    box.append(sensor_row(s));
  });
  sensor_rows_changed();
}

function save_sensor_config() {
  const list = [];
  document.querySelectorAll("#sensor_rows .sensor_row").forEach(function (r) {
    list.push({
      type: r.querySelector(".sensor_type").value,
      name: (r.querySelector(".sensor_name").value || "").trim(),
      pin: r.querySelector(".sensor_pin").value,
      pin2: r.querySelector(".sensor_pin2").value,
    });
  });
  const v = L.validateSensorConfig(
    {
      enabled: byId("sensor_enabled").checked,
      interval: byId("sensor_interval").value,
      ack: byId("sensor_ack").checked,
      sensors: list,
    },
    state.sensorPins
  );
  if (!v.ok) {
    alertText("Invalid sensor settings:\n" + v.errors.join("\n"));
    return;
  }
  const send = function () {
    ws_send({ "request-type": "set_sensor_config", val: JSON.stringify(v.config) });
  };
  if (v.config.enabled && state.serialMode)
    confirm_prompt(
      "Sensor broadcast",
      "Turning sensor broadcast on switches LoRa to Serial off, which also ends a file transfer in progress. Continue?",
      send
    );
  else send();
}

var SENSOR_READ_TIMEOUT_MS = 15000; // 4 slow sensors (DS18B20, DHT retry) fit well inside

// "Read now": the device reads every saved sensor once and answers with WS
// sensor_data (nothing is transmitted). Works with the broadcast switched off.
function sensor_read_now() {
  if (!ws_send({ "request-type": "read_sensors" })) return;
  setText("sensor_read_status", "Reading...");
  setDisabled("sensor_read_btn", true);
  clearTimeout(state.sensorReadTimer);
  state.sensorReadTimer = setTimeout(function () {
    setText("sensor_read_status", "No answer from the device.");
    setDisabled("sensor_read_btn", false);
  }, SENSOR_READ_TIMEOUT_MS);
}

// One line per sensor: name, type, then either the reading with "detected", or
// a clear "no response" so a wiring / pin mistake is visible.
function sensor_status_item(r) {
  const li = el("li", "spread");
  const left = el("span");
  left.append(el("b", "", r.name), document.createTextNode(" (" + r.typeLabel + ") : "));
  if (r.ok) left.append(el("span", "", r.text));
  else left.append(el("span", "text-bad", "no response - check the wiring and the selected pin"));
  li.append(left, el("span", r.ok ? "text-ok" : "text-bad", r.ok ? "Detected" : "Not detected"));
  return li;
}

function render_sensor_local() {
  const ul = byId("sensor_local");
  if (!ul) return;
  clearTimeout(state.sensorReadTimer);
  setDisabled("sensor_read_btn", false);
  ul.replaceChildren();
  const local = state.sensorLocal;
  const saved = state.sensorCfg && Array.isArray(state.sensorCfg.sensors) ? state.sensorCfg.sensors.length : 0;
  if (!local) {
    ul.append(el("li", "muted", saved ? "Not read yet. Press Read now." : "No sensors saved yet."));
    return;
  }
  if (local.readings.length === 0) {
    setText("sensor_read_status", "");
    ul.append(el("li", "muted", "No sensors saved on the device. Add one above and press Save."));
    return;
  }
  local.readings.forEach(function (r) {
    ul.append(sensor_status_item(r));
  });
  setText("sensor_read_status", "Read at " + local.time.toLocaleTimeString());
}

// Keeps the latest reading per device + sensor, newest first.
function on_sensor_rx(msg) {
  const now = new Date();
  (msg.sensors || []).forEach(function (r) {
    const key = (msg.mac || msg.sender) + "|" + r.name;
    state.sensorRx = state.sensorRx.filter(function (e) {
      return e.key !== key;
    });
    state.sensorRx.unshift({
      key: key,
      sender: msg.sender,
      name: r.name,
      typeLabel: r.typeLabel,
      text: r.text,
      ok: r.ok,
      time: now,
    });
  });
  if (state.sensorRx.length > SENSOR_RX_MAX_ROWS) state.sensorRx.length = SENSOR_RX_MAX_ROWS;
  render_sensor_rx();
}

function render_sensor_rx() {
  const ul = byId("sensor_rx_list");
  if (!ul) return;
  ul.replaceChildren();
  if (state.sensorRx.length === 0) {
    ul.append(el("li", "muted", "Nothing received yet."));
    return;
  }
  state.sensorRx.forEach(function (e) {
    const li = el("li", "spread");
    const left = el("span");
    left.append(
      el("b", "", e.sender),
      document.createTextNode(" / " + e.name + " (" + e.typeLabel + ") : "),
      el("span", e.ok ? "" : "text-bad", e.text)
    );
    li.append(left, el("span", "muted", e.time.toLocaleTimeString()));
    ul.append(li);
  });
}
