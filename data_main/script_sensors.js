/*
 * Project Voyager web UI - Sensors page.
 * Function declarations only: the shared constants and `state` live in
 * script.js, which index.html loads last (see the loader there).
 */
"use strict";

/* ------------------------------------------------------------------ */
/* Sensors page                                                       */
/* ------------------------------------------------------------------ */

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
  refresh_locks();
}

function fill_pin_select(sel, pins, selected) {
  sel.empty();
  sel.append(new Option("GPIO...", ""));
  pins.forEach(function (pin) {
    sel.append(new Option("GPIO " + pin, String(pin)));
  });
  sel.val(pins.indexOf(Number(selected)) >= 0 ? String(selected) : "");
}

// One editable sensor row. `s` may be null for a new, empty row.
function sensor_row(s) {
  const row = $("<div>").addClass("row g-2 mb-2 align-items-end sensor_row");
  const col = function (cls, label, el) {
    return $("<div>").addClass(cls).append($("<label>").addClass("form-label small mb-0").text(label), el);
  };
  const type = $("<select>").addClass("form-select form-select-sm sensor_type");
  L.SENSOR_TYPES.forEach(function (t) {
    type.append(new Option(t.label, t.key));
  });
  const name = $("<input>")
    .addClass("form-control form-control-sm sensor_name")
    .attr({ type: "text", maxlength: 12, placeholder: "e.g. room" });
  const pin = $("<select>").addClass("form-select form-select-sm sensor_pin");
  const pin2 = $("<select>").addClass("form-select form-select-sm sensor_pin2");
  const pinLabel = $("<label>").addClass("form-label small mb-0");
  const pin2Col = col("col-6 col-md-2", "Echo pin", pin2);
  const remove = $("<button>")
    .addClass("btn btn-sm btn-outline-danger")
    .attr("type", "button")
    .text("Remove")
    .on("click", function () {
      row.remove();
      sensor_rows_changed();
    });

  const apply_type = function (selPin, selPin2) {
    const t = L.sensorType(type.val());
    pinLabel.text(t.pin2 ? "Trigger pin" : t.pins === "adc" ? "Analog pin" : "Data pin");
    fill_pin_select(pin, state.sensorPins[t.pins] || [], selPin);
    pin2Col.prop("hidden", !t.pin2);
    fill_pin_select(pin2, t.pin2 ? state.sensorPins[t.pin2] || [] : [], selPin2);
  };
  type.on("change", function () {
    apply_type("", "");
  });

  if (s && L.sensorType(s.type)) type.val(s.type);
  name.val(s ? L.toText(s.name) : "");
  apply_type(s ? s.pin : "", s ? s.pin2 : "");

  row.append(
    col("col-12 col-md-4", "Sensor", type),
    col("col-6 col-md-2", "Name", name),
    $("<div>").addClass("col-6 col-md-2").append(pinLabel, pin),
    pin2Col,
    $("<div>").addClass("col-6 col-md-2").append(remove)
  );
  return row;
}

function sensor_rows_changed() {
  const n = $("#sensor_rows .sensor_row").length;
  $("#sensor_rows_empty").prop("hidden", n > 0);
  $("#sensor_add_btn").prop("disabled", n >= L.SENSOR_MAX);
}

function sensor_add_row() {
  if ($("#sensor_rows .sensor_row").length >= L.SENSOR_MAX) return;
  $("#sensor_rows").append(sensor_row(null));
  sensor_rows_changed();
}

function render_sensor_form() {
  const box = $("#sensor_rows");
  if (box.length === 0) return; // another page is showing
  const cfg = state.sensorCfg || { enabled: false, interval: 30, ack: false, sensors: [] };
  $("#sensor_enabled").prop("checked", cfg.enabled === true);
  $("#sensor_ack").prop("checked", cfg.ack === true);
  $("#sensor_interval").val(Number.isInteger(cfg.interval) ? cfg.interval : 30);
  box.empty();
  (Array.isArray(cfg.sensors) ? cfg.sensors : []).slice(0, L.SENSOR_MAX).forEach(function (s) {
    box.append(sensor_row(s));
  });
  sensor_rows_changed();
}

function save_sensor_config() {
  const list = [];
  $("#sensor_rows .sensor_row").each(function () {
    const r = $(this);
    list.push({
      type: r.find(".sensor_type").val(),
      name: (r.find(".sensor_name").val() || "").trim(),
      pin: r.find(".sensor_pin").val(),
      pin2: r.find(".sensor_pin2").val(),
    });
  });
  const v = L.validateSensorConfig(
    {
      enabled: $("#sensor_enabled").prop("checked"),
      interval: $("#sensor_interval").val(),
      ack: $("#sensor_ack").prop("checked"),
      sensors: list,
    },
    state.sensorPins
  );
  if (!v.ok) {
    alertText("Invalid sensor settings:\n" + v.errors.join("\n"));
    return;
  }
  if (v.config.enabled && (state.sending || receiver.isActive())) {
    alertText("A file transfer is in progress. Wait for it to finish first.");
    return;
  }
  const send = function () {
    ws_send({ "request-type": "set_sensor_config", val: JSON.stringify(v.config) });
  };
  if (v.config.enabled && state.serialMode)
    confirm_prompt("Sensor broadcast", "Turning sensor broadcast on switches LoRa to Serial off. Continue?", send);
  else send();
}

function sensor_item(name, typeLabel, text, ok) {
  const li = $("<li>").addClass("list-group-item");
  li.append($("<b>").text(name));
  li.append(document.createTextNode(" (" + typeLabel + ") : "));
  li.append($("<span>").addClass(ok ? "" : "text-danger").text(text));
  return li;
}

function render_sensor_local() {
  const ul = $("#sensor_local");
  if (ul.length === 0) return;
  ul.empty();
  const local = state.sensorLocal;
  if (!local || local.readings.length === 0) {
    ul.append($("<li>").addClass("list-group-item text-muted").text("No readings yet."));
    return;
  }
  local.readings.forEach(function (r) {
    ul.append(sensor_item(r.name, r.typeLabel, r.text, r.ok));
  });
  ul.append(
    $("<li>").addClass("list-group-item text-muted small").text("Read at " + local.time.toLocaleTimeString())
  );
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
  const body = $("#sensor_rx_body");
  if (body.length === 0) return;
  body.empty();
  if (state.sensorRx.length === 0) {
    body.append(
      $("<tr>").append($("<td>").attr("colspan", 5).addClass("text-muted").text("Nothing received yet."))
    );
    return;
  }
  state.sensorRx.forEach(function (e) {
    $("<tr>")
      .append(
        $("<td>").text(e.sender),
        $("<td>").text(e.name),
        $("<td>").text(e.typeLabel),
        $("<td>").addClass(e.ok ? "" : "text-danger").text(e.text),
        $("<td>").text(e.time.toLocaleTimeString())
      )
      .appendTo(body);
  });
}
