/*
 * Project Voyager web UI - WiFi page.
 * Function declarations only: the shared constants and `state` live in
 * script.js, which index.html loads last (see the loader there).
 */
"use strict";

/* ------------------------------------------------------------------ */
/* WiFi page                                                          */
/* ------------------------------------------------------------------ */

function scan_ssid() {
  $("#wifi_ssid_list").empty();
  if (!ws_send({ "request-type": "wifi_ssid_scan" })) return;
  $("#wifi_scan_btn").text("Scanning...").prop("disabled", true);
  clearTimeout(state.scanTimer);
  state.scanTimer = setTimeout(function () {
    $("#wifi_scan_btn").text("Scan SSID").prop("disabled", false);
  }, SCAN_TIMEOUT_MS);
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
    schedule_redirect();
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
        schedule_redirect();
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
