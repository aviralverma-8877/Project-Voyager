/*
 * Project Voyager web UI - LoRa settings page.
 * Function declarations only: the shared constants and `state` live in
 * script.js, which index.html loads last (see the loader there).
 */
"use strict";

/* ------------------------------------------------------------------ */
/* LoRa page                                                          */
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
  }, URL_REVOKE_DELAY_MS);
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
