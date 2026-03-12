var hostname_url;
var loading_alert;
var promptModal;
var alertModel;

$(document).ready(function () {
  init_socket();
  init_events();
  get_hostname();
  promptModal = new bootstrap.Modal($("#promptModal"), {});
  alertModel = new bootstrap.Modal($("#alertModal"), {});
  loading_alert = new bootstrap.Modal($("#loadingModal"), {});
  $("#loading_model_body").html("Connecting...");
  loading_alert.show();
  window.onbeforeunload = function () {
    return "Are you sure you want to leave?";
  };
});

$("#myModal").on("shown.bs.modal", function () {
  $("#myInput").trigger("focus");
});

function set_sync_word(val) {
  $.get("/config/lora_config.json", function (lora_config) {
    $("#sync_word").val(lora_config.SyncWord);
  });
}

function generate_sync_word() {
  for (var i = 1; i < 256; i++) $("#sync_word").append(new Option(i, i));
}

function get_username() {
  Socket.send(
    JSON.stringify({
      "request-type": "get_username",
    })
  );
}

function send_lora(msg) {
  if (msg != "") {
    tx_msg = { pack_type: "msg", data: msg };
    $("#lora_msg").val("");
    $("#lora_msg").attr("readonly", true);
    $.post("/lora_transmit", { data: JSON.stringify(tx_msg) })
      .done(function (data) {
        $("#lora_msg").attr("readonly", false);
        if (data.akn == 1) {
          $("#lora_rx_msg").prepend(
            "<li class='list-group-item'>" +
              data.username +
              " : " +
              msg +
              "</li>"
          );
        }
      })
      .fail(function (data) {
        $("#lora_msg").attr("readonly", false);
      });
  }
}

function restart() {
  $("#promptModalLabel").html("Device Restart");
  $("#prompt_body").html("Are you sure you want to restart the device");
  promptModal.show();
  $("#promptModelProceed").click(function () {
    promptModal.hide();
    Socket.send(JSON.stringify({ "request-type": "restart_device" }));
  });
}

function get_hostname() {
  $.get("hostname", function (data) {
    hostname_url = "http://" + data + ".local/";
    $("#project_title").attr("href", hostname_url);
  });
}

function dashboard() {
  $(".nav-link").removeClass("active");
  $("#navbar-dashboard").addClass("active");
  $.get("dashboard.html", function (data) {
    $("#main_content").html(data);
    var block_input_status = transmission;
    $.get("/config/lora_serial.json", function (config) {
      $("#lora_to_serial").attr("checked", config.lora_serial);
      block_input_status = block_input_status || config.lora_serial;
      block_inputs(block_input_status);
      setTimeout(function () {
        get_username();
        set_sync_word();
      }, 10);
    });
  });
}

function file_transfer() {
  $.get("file_transfer.html", function (data) {
    $("#main_content").html(data);
    var block_input_status = transmission;
    $.get("/config/lora_serial.json", function (config) {
      block_input_status = block_input_status || config.lora_serial;
      block_inputs(block_input_status);
    });
  });
}

function wifi() {
  $(".nav-link").removeClass("active");
  $("#navbar-wifi").addClass("active");
  $.get("wifi.html", function (data) {
    $("#main_content").html(data);
    block_inputs(transmission);
    $.get("/config/wifi_config.json", function (data) {
      $("#wifi_ssid_name").html(data.wifi_ssid);
    });
  });
}

function save_lora_config() {
  $.get("/config/lora_config.json", function (lora_config) {
    freq = $("#freq_range").val();
    tx_power = $("#tx_power_range").val();
    s_fact = $("#spreading_factor").val();
    bandwidth = $("#bandwidth").val();
    coding_rate = $("#coding_rate").val();
    sync_word = $("#sync_word").val();

    lora_config["freq"] = parseInt(freq * 1000000);
    lora_config["TxPower"] = parseInt(tx_power);
    lora_config["SpreadingFactor"] = parseInt(s_fact);
    lora_config["SignalBandwidth"] = parseInt(bandwidth);
    lora_config["CodingRate4"] = parseInt(coding_rate);
    lora_config["SyncWord"] = parseInt(sync_word);
    Socket.send(
      JSON.stringify({
        "request-type": "set_lora_config",
        val: JSON.stringify(lora_config),
      })
    );
  });
}

function update_lora_config_from_file() {
  const file = $("#LoraConfigFile").prop("files")[0];
  const reader = new FileReader();
  reader.readAsText(file);
  reader.onload = function (e) {
    try {
      lora_config = JSON.parse(reader.result);
      $("#LoraConfigFile").val("");
      set_lora_config(lora_config);
      alert("Settings applied.<br>Press save to apply.");
    } catch (e) {
      alert("Invalid file input.");
    }
  };
}

function download_lora_config() {
  $.get("/config/lora_config.json", function (lora_config) {
    freq = $("#freq_range").val();
    tx_power = $("#tx_power_range").val();
    s_fact = $("#spreading_factor").val();
    bandwidth = $("#bandwidth").val();
    coding_rate = $("#coding_rate").val();
    sync_word = $("#sync_word").val();

    lora_config["freq"] = parseInt(freq * 1000000);
    lora_config["TxPower"] = parseInt(tx_power);
    lora_config["SpreadingFactor"] = parseInt(s_fact);
    lora_config["SignalBandwidth"] = parseInt(bandwidth);
    lora_config["CodingRate4"] = parseInt(coding_rate);
    lora_config["SyncWord"] = parseInt(sync_word);
    JSONToFile(lora_config, "project_voyager_lora_config.json");
  });
}

const JSONToFile = (obj, filename) => {
  const blob = new Blob([JSON.stringify(obj, null, 2)], {
    type: "application/json",
  });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = `${filename}.json`;
  a.click();
  URL.revokeObjectURL(url);
};

function lora() {
  $(".nav-link").removeClass("active");
  $("#navbar-lora").addClass("active");
  $.get("lora.html", function (data) {
    $("#main_content").html(data);
    block_inputs(transmission);
    $.get("/config/lora_config.json", function (lora_config) {
      generate_sync_word();

      $("#freq_range").val(lora_config.freq / 1000000);
      $("#selected_lora_freq").html(lora_config.freq / 1000000);

      $("#tx_power_range").val(lora_config.TxPower);
      $("#selected_lora_tx_power").html(lora_config.TxPower);

      $("#spreading_factor").val(lora_config.SpreadingFactor);
      $("#selected_lora_spreading_factor").html(lora_config.SpreadingFactor);

      $("#bandwidth").val(lora_config.SignalBandwidth);

      $("#coding_rate").val(lora_config.CodingRate4);
      $("#selected_lora_coding_rate").html(lora_config.CodingRate4);

      $("#sync_word").val(lora_config.SyncWord);
      $.get("/config/config_list.json", function (config_list) {
        config_list.forEach((element) => {
          $("#lora_config").append(
            '<option value="' +
              element.value +
              '">' +
              element.name +
              "</option>"
          );
        });
      });
    });
  });
}

function set_lora_config(lora_config) {
  generate_sync_word();

  $("#freq_range").val(lora_config.freq / 1000000);
  $("#selected_lora_freq").html(lora_config.freq / 1000000);

  $("#tx_power_range").val(lora_config.TxPower);
  $("#selected_lora_tx_power").html(lora_config.TxPower);

  $("#spreading_factor").val(lora_config.SpreadingFactor);
  $("#selected_lora_spreading_factor").html(lora_config.SpreadingFactor);

  $("#bandwidth").val(lora_config.SignalBandwidth);

  $("#coding_rate").val(lora_config.CodingRate4);
  $("#selected_lora_coding_rate").html(lora_config.CodingRate4);

  $("#sync_word").val(lora_config.SyncWord);
}

function load_lora_config(val) {
  if (val != "") {
    $.get("/config/" + val, function (lora_config) {
      set_lora_config(lora_config);
    });
  }
}

function update() {
  $(".nav-link").removeClass("active");
  $("#navbar-update").addClass("active");
  $.get("update", function (data) {
    $("#main_content").html(data);
    block_inputs(transmission);
  });
}

function alert(msg) {
  $("#alert_body").html(msg);
  alertModel.show();
}

function reset() {
  $("#promptModalLabel").html("Device Reset");
  $("#prompt_body").html("Are you sure you want to reset the device ?");
  promptModal.show();
  $("#promptModelProceed").click(function () {
    promptModal.hide();
    Socket.send(JSON.stringify({ "request-type": "reset_device" }));
    setTimeout(function () {
      window.location.replace(hostname_url);
    }, 10000);
  });
}

function scan_ssid() {
  $("#wifi_ssid_list").html("");
  $("#wifi_scan_btn").html("Scanning...");
  $("#wifi_scan_btn").attr("onclick", "");
  Socket.send(JSON.stringify({ "request-type": "wifi_ssid_scan" }));
}

function wifi_connect() {
  ssid = $("#wifi_ssid").val();
  psk = $("#wifi_password").val();
  if (ssid == "" || ssid == undefined) {
    alert("Invalid SSID.");
    return;
  }
  if (psk == "" || psk == undefined) {
    alert("Invalid password.");
    return;
  }
  $("#loading_model_body").html("Connecting to WiFi...");
  loading_alert.show();
  Socket.send(
    JSON.stringify({
      "request-type": "connect_wifi",
      wifi_ssid: ssid,
      wifi_pass: psk,
    })
  );
  setTimeout(function () {
    window.location.replace(hostname_url);
  }, 10000);
}

function wifi_ap_mode() {
  $("#promptModalLabel").html("Access Point");
  $("#prompt_body").html(
    "Are you sure you want to switch to access point mode ?"
  );
  promptModal.show();
  $("#promptModelProceed").click(function () {
    promptModal.hide();
    $.get("/username", function (username) {
      Socket.send(
        JSON.stringify({
          "request-type": "wifi_ap_mode",
        })
      );
      alert(
        'Switched to AP mode.<br />Connect the wifi to "' +
          username +
          '" access point.'
      );
      setTimeout(function () {
        window.location.replace(hostname_url);
      }, 10000);
    });
  });
}

function set_username(val) {
  if (val != "") {
    Socket.send(
      JSON.stringify({
        "request-type": "set_username",
        val: val,
      })
    );
  }
}
function isJSON(str) {
  try {
    return JSON.parse(str) && !!str;
  } catch (e) {
    return false;
  }
}
function update_wifi_ssid(ssid) {
  $("#wifi_ssid").attr("value", ssid);
}

var file_name = "";
var lastEventTimestamp = 0;
var lastDebugEventTimestamp = 0;

// ── Web Serial state ─────────────────────────────────────────────────────────
var serialPort   = null;
var serialReader = null;
var serialWriter = null;
var xmodemCancel = false;   // set true to abort an in-progress transfer

// XModem control bytes
var XMODEM_SOH = 0x01;
var XMODEM_EOT = 0x04;
var XMODEM_ACK = 0x06;
var XMODEM_NAK = 0x15;
var XMODEM_CAN = 0x18;
var XMODEM_C   = 0x43;  // 'C' — triggers CRC mode on sender
function init_events() {
  var source = new EventSource("/rawEvents");
  source.addEventListener(
    "open",
    function (e) {
      console.log("Events Connected");
    },
    false
  );
  source.addEventListener(
    "error",
    function (e) {
      if (e.target.readyState != EventSource.OPEN) {
        console.log("Events Disconnected");
      }
    },
    false
  );
  source.addEventListener(
    "message",
    function (e) {
      // console.log("message", e.data);
    },
    false
  );

  source.addEventListener("WIFI_DATA", function (e) {
    var data = JSON.parse(e.data);
    if (data != "") {
      var rssi = parseInt(data.rssi);
      quality = 2 * (rssi + 100);
      $("#wifi_quality").html(quality+" %")
    }
  });
  source.addEventListener("RAM_DATA", function (e) {
    var data = JSON.parse(e.data);
    if (data != "") {
      var free_heap = parseInt(data.free_heap);
      var heap_size = parseInt(data.heap_size);
      var heap_per = Math.round(((heap_size - free_heap) / heap_size) * 100);
      $("#ram_ratio").html(
        "<b>" + heap_per + "%</b> (" + free_heap + "/" + heap_size + ")"
      );
      $("#heap_progress_bar").removeClass("bg-success");
      $("#heap_progress_bar").removeClass("bg-warning");
      $("#heap_progress_bar").removeClass("bg-danger");
      if (heap_per < 30) $("#heap_progress_bar").addClass("bg-success");
      if (heap_per > 30 && heap_per <= 70)
        $("#heap_progress_bar").addClass("bg-warning");
      if (heap_per > 70) $("#heap_progress_bar").addClass("bg-danger");
      $("#heap_progress_bar").css("width", heap_per + "%");
    }
  });
  source.addEventListener("DEBUG", function (e) {
    var data = JSON.parse(e.data);
    if (lastDebugEventTimestamp == data.millis) return;
    lastDebugEventTimestamp = data.millis;
    data = data.data;
    if (data != "") {
      const d = new Date();
      let time = d.getTime();
      $("#debug_textarea").prepend(time + " > " + data + "\n");
    }
  });
}

function get_string_size(stringdata) {
  const byteSize = (str) => new Blob([str]).size;
  var file_size = byteSize(stringdata);
  var file_size_string;
  if (file_size / 1024 < 1) {
    file_size_string = file_size.toFixed(2) + " Bytes";
  } else if (file_size / 1024 / 1024 < 1) {
    file_size_string = (file_size / 1204).toFixed(2) + " KB";
  } else {
    file_size_string = (file_size / 1204 / 1024).toFixed(2) + " MB";
  }
  return file_size_string;
}

function init_socket() {
  console.log("Initilizing web sockets.");
  Socket = new WebSocket(
    "ws://" + window.location.hostname + ":" + window.location.port + "/ws"
  );
  Socket.onmessage = function (event) {
    // console.log(event.data);
    try {
      var data = JSON.parse(event.data);
      var response_type = data.response_type;
      //console.log("Web socket message recieved...");
      // console.log(data);
      if (response_type == "wifi_scan") {
        $("#wifi_scan_btn").html("Scan SSID");
        $("#wifi_scan_btn").attr("onclick", "scan_ssid()");
        var ssid_list = data.SSID;
        var output = "";
        for (wifi_ssid in ssid_list) {
          var wifi_ssid = ssid_list[wifi_ssid];
          var quality = 2 * (wifi_ssid.rssi + 100);
          var cl = "";
          if (quality >= 80) {
            cl = "list-group-item-success";
          } else if (quality >= 60) {
            cl = "list-group-item-primary";
          } else if (quality >= 40) {
            cl = "list-group-item-warning";
          } else if (quality < 40) {
            cl = "list-group-item-danger";
          }
          output +=
            '<li class="list-group-item ' +
            cl +
            ' d-flex justify-content-between align-items-center">\
            <a href="#wifi_password" onclick="update_wifi_ssid(\'' +
            wifi_ssid.ssid +
            "')\">" +
            wifi_ssid.ssid +
            "</a></li>";
        }
        $("#wifi_ssid_list").html(output);
      }
      if (response_type == "alert") {
        var msg = data.alert_msg;
        alert(msg);
      }
      if (response_type == "lora_rx") {
        data = JSON.parse(data.lora_msg);
        // Socket.send(
        //   JSON.stringify({
        //     "request-type": "send_akn",
        //     "akn": 1,
        //   })
        // );
        var uname = data.name;
        var data = JSON.parse(data.data);
        var pack_type = data["pack_type"];
        if (pack_type == "msg") {
          msg = data["data"];
          $("#lora_rx_msg").prepend(
            "<li class='list-group-item'>" + uname + " : " + msg + "</li>"
          );
        }
        if (pack_type == "action") {
          action = data["data"];
          if (action == "sos") {
            alert("User " + uname + " has sent a SOS request.");
          }
        }
      }
      if (response_type == "set_uname") {
        $("#username").val(data.uname);
      }
      if (response_type == "set_sync_word") {
        set_sync_word(data.value);
      }
    } catch (e) {
      console.log(e);
    }
  };
  Socket.onopen = function (event) {
    console.log("Connected to web sockets...");
    setTimeout(function () {
      loading_alert.hide();
    }, 1000);
    dashboard();
  };
  Socket.onclose = function (event) {
    $("#loading_model_body").html("Connecting...");
    loading_alert.show();
    console.log("Connection to websockets closed....");
    setTimeout(function () {
      console.log("Retrying websocket connection....");
      init_socket();
    }, 1000);
  };
  Socket.onerror = function (event) {
    console.log("Error in websockets" + event);
  };
}

function block_inputs(val) {
  if (val) {
    $(".lora_transmission").attr("disabled", "true");
  } else {
    $(".lora_transmission").removeAttr("disabled");
  }
}

function set_lora_to_serial(val) {
  block_inputs(val);
  Socket.send(
    JSON.stringify({
      "request-type": "set_serial_mode",
      value: val,
    })
  );
}

function set_freq_range(val) {
  $("#selected_lora_freq").html(val);
}
function set_tx_power(val) {
  $("#selected_lora_tx_power").html(val);
}
function set_spreading_factor(val) {
  $("#selected_lora_spreading_factor").html(val);
}

function set_coding_rate(val) {
  $("#selected_lora_coding_rate").html(val);
}

function reset_progress_bar() {
  $("#file_upload_progress_bar").css("width", "0%");
}

var transmission = false;

// ── Helpers ──────────────────────────────────────────────────────────────────

function serialDelay(ms) {
  return new Promise(function(resolve) { setTimeout(resolve, ms); });
}

/** CRC-16/CCITT-FALSE used by XModem-CRC */
function crc16(buf) {
  var crc = 0;
  for (var i = 0; i < buf.length; i++) {
    crc ^= (buf[i] << 8);
    for (var j = 0; j < 8; j++) {
      crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) : (crc << 1);
    }
  }
  return crc & 0xFFFF;
}

/** Read exactly `n` bytes from serialReader with a per-byte timeout (ms). */
async function serialReadBytes(n, timeoutMs) {
  var buf = new Uint8Array(n);
  var got = 0;
  var deadline = Date.now() + timeoutMs;
  while (got < n) {
    if (Date.now() > deadline) throw new Error("serial read timeout");
    if (xmodemCancel) throw new Error("cancelled");
    var result = await Promise.race([
      serialReader.read(),
      new Promise(function(_, reject) { setTimeout(function(){ reject(new Error("byte timeout")); }, timeoutMs - (Date.now() - (deadline - timeoutMs))); })
    ]);
    if (result.done) throw new Error("serial port closed");
    buf.set(result.value.subarray(0, Math.min(result.value.length, n - got)), got);
    got += Math.min(result.value.length, n - got);
  }
  return buf;
}

/** Write bytes to serialWriter. */
async function serialWrite(bytes) {
  await serialWriter.write(new Uint8Array(bytes));
}

// ── Web Serial connect / disconnect ──────────────────────────────────────────

async function connectSerial() {
  if (!("serial" in navigator)) {
    alert("Web Serial is not supported in this browser.<br>Use Chrome or Edge (Chromium 89+).");
    return;
  }
  try {
    serialPort = await navigator.serial.requestPort();
    await serialPort.open({ baudRate: 115200 });
    serialReader = serialPort.readable.getReader();
    serialWriter = serialPort.writable.getWriter();
    $("#serial_status_dot").css("background", "#198754");
    $("#serial_status_label").text("Connected — " + (serialPort.getInfo().usbVendorId ? "USB device" : "Serial device"));
    $("#connectSerialBtn").hide();
    $("#disconnectSerialBtn").show();
  } catch (e) {
    alert("Could not open serial port: " + e.message);
  }
}

async function disconnectSerial() {
  xmodemCancel = true;
  try {
    if (serialReader) { await serialReader.cancel(); serialReader = null; }
  } catch(e) {}
  try {
    if (serialWriter) { await serialWriter.close(); serialWriter = null; }
  } catch(e) {}
  try {
    if (serialPort) { await serialPort.close(); serialPort = null; }
  } catch(e) {}
  xmodemCancel = false;
  $("#serial_status_dot").css("background", "#dc3545");
  $("#serial_status_label").text("Not connected");
  $("#connectSerialBtn").show();
  $("#disconnectSerialBtn").hide();
  setTransferUI(false);
}

// ── Transfer UI helpers ───────────────────────────────────────────────────────

function setTransferUI(active) {
  transmission = active;
  block_inputs(false);  // always unlock other nav inputs
  if (active) {
    $("#sendFileBtn, #receiveFileBtn").attr("disabled", true);
    $("#cancelTransferDiv").show();
    $("#file_upload_progress_bar").css("width", "0%").removeClass("bg-success bg-danger").addClass("progress-bar-animated");
  } else {
    $("#sendFileBtn, #receiveFileBtn").removeAttr("disabled");
    $("#cancelTransferDiv").hide();
    $("#file_upload_progress_bar").removeClass("progress-bar-animated");
  }
}

function cancelTransfer() {
  xmodemCancel = true;
}

// ── XModem-CRC sender ─────────────────────────────────────────────────────────
// fileBytes: Uint8Array of the file to send.
// totalBlocks: total number of 128-byte blocks (used for progress).

async function xmodemSend(fileBytes, totalBlocks) {
  var MAX_RETRIES = 10;
  var ACK_TIMEOUT = 12000;   // ms to wait for ACK per block (LoRa round-trip can be ~2s)
  var INIT_TIMEOUT = 60000;  // ms to wait for 'C' from receiver

  // Pad to multiple of 128
  var padded = new Uint8Array(totalBlocks * 128);
  padded.set(fileBytes);
  for (var i = fileBytes.length; i < padded.length; i++) padded[i] = 0x1A; // SUB pad

  // Wait for receiver 'C'
  $("#chunk_ratio").text("Waiting for receiver to be ready (sending 'C')…");
  var gotC = false;
  var initDeadline = Date.now() + INIT_TIMEOUT;
  while (!gotC && Date.now() < initDeadline) {
    if (xmodemCancel) throw new Error("cancelled");
    var r = await Promise.race([
      serialReader.read(),
      new Promise(function(resolve) { setTimeout(function(){ resolve({value: new Uint8Array(0)}); }, 3000); })
    ]);
    if (r.value) {
      for (var i = 0; i < r.value.length; i++) {
        if (r.value[i] === XMODEM_C) { gotC = true; break; }
      }
    }
  }
  if (!gotC) throw new Error("Receiver did not respond with 'C' within timeout");

  // Send blocks
  for (var blk = 0; blk < totalBlocks; blk++) {
    if (xmodemCancel) throw new Error("cancelled");
    var chunk = padded.subarray(blk * 128, blk * 128 + 128);
    var blkNum = (blk + 1) & 0xFF;
    var crc = crc16(chunk);
    var frame = new Uint8Array(133);
    frame[0] = XMODEM_SOH;
    frame[1] = blkNum;
    frame[2] = (~blkNum) & 0xFF;
    frame.set(chunk, 3);
    frame[131] = (crc >> 8) & 0xFF;
    frame[132] = crc & 0xFF;

    var acked = false;
    for (var attempt = 0; attempt < MAX_RETRIES && !acked; attempt++) {
      if (xmodemCancel) throw new Error("cancelled");
      await serialWrite(frame);
      // Wait for ACK or NAK
      try {
        var resp = await serialReadBytes(1, ACK_TIMEOUT);
        if (resp[0] === XMODEM_ACK) {
          acked = true;
        } else if (resp[0] === XMODEM_CAN) {
          throw new Error("Remote cancelled transfer (CAN)");
        }
        // NAK or anything else → retry
      } catch(e) {
        if (e.message === "cancelled" || e.message.indexOf("CAN") >= 0) throw e;
        // timeout → retry
      }
    }
    if (!acked) throw new Error("Block " + (blk+1) + " failed after " + MAX_RETRIES + " retries");

    var pct = Math.round(((blk + 1) / totalBlocks) * 100);
    $("#file_upload_progress_bar").css("width", pct + "%");
    $("#chunk_ratio").text("Sent block " + (blk+1) + " / " + totalBlocks + " (" + pct + "%)");
  }

  // Send EOT until ACKed
  for (var i = 0; i < MAX_RETRIES; i++) {
    await serialWrite([XMODEM_EOT]);
    try {
      var r = await serialReadBytes(1, 5000);
      if (r[0] === XMODEM_ACK) break;
    } catch(e) { /* retry */ }
  }
}

// ── XModem-CRC receiver ───────────────────────────────────────────────────────
// Returns a Uint8Array of the received file data (padded to 128-byte boundary).

async function xmodemReceive() {
  var MAX_RETRIES  = 10;
  var BLOCK_TIMEOUT = 12000;  // ms to wait for a block start byte
  var C_INTERVAL   = 3000;    // ms between 'C' retries
  var C_MAX        = 20;      // max 'C' sends before giving up

  var receivedBlocks = [];
  var expectedBlk = 1;

  // Send 'C' to initiate CRC mode; wait for SOH
  var started = false;
  for (var attempt = 0; attempt < C_MAX && !started; attempt++) {
    if (xmodemCancel) throw new Error("cancelled");
    await serialWrite([XMODEM_C]);
    $("#chunk_ratio").text("Waiting for sender… (attempt " + (attempt+1) + "/" + C_MAX + ")");
    // Wait up to C_INTERVAL ms for a byte
    try {
      var r = await Promise.race([
        serialReader.read(),
        new Promise(function(resolve) { setTimeout(function(){ resolve({value: new Uint8Array(0)}); }, C_INTERVAL); })
      ]);
      if (r.value && r.value.length > 0) {
        if (r.value[0] === XMODEM_SOH) {
          started = true;
          // Process this first SOH below by feeding it to the loop
          // Store it so the block-reading loop can use it
          var pendingFirstByte = r.value[0];
          var receiving = true;

          while (receiving) {
            if (xmodemCancel) { await serialWrite([XMODEM_CAN, XMODEM_CAN]); throw new Error("cancelled"); }

            var headerByte = (pendingFirstByte !== undefined) ? pendingFirstByte : null;
            pendingFirstByte = undefined;

            if (headerByte === null) {
              // Read next start byte
              try {
                var hdr = await serialReadBytes(1, BLOCK_TIMEOUT);
                headerByte = hdr[0];
              } catch(e) {
                if (e.message === "cancelled") throw e;
                throw new Error("Timed out waiting for block start byte");
              }
            }

            if (headerByte === XMODEM_EOT) {
              await serialWrite([XMODEM_ACK]);
              receiving = false;
              break;
            }
            if (headerByte === XMODEM_CAN) {
              throw new Error("Remote cancelled transfer (CAN)");
            }
            if (headerByte !== XMODEM_SOH) {
              // Unexpected byte — send NAK
              await serialWrite([XMODEM_NAK]);
              continue;
            }

            // Read remaining 132 bytes of block (blkNum + ~blkNum + 128 data + 2 CRC)
            var rest;
            try {
              rest = await serialReadBytes(132, BLOCK_TIMEOUT);
            } catch(e) {
              if (e.message === "cancelled") throw e;
              await serialWrite([XMODEM_NAK]);
              continue;
            }

            var blkNum    = rest[0];
            var blkNumInv = rest[1];
            var data      = rest.subarray(2, 130);
            var crcHi     = rest[130];
            var crcLo     = rest[131];
            var receivedCrc = (crcHi << 8) | crcLo;
            var calcCrc     = crc16(data);

            // Validate
            var blkOk = ((blkNum + blkNumInv) === 0xFF) && (calcCrc === receivedCrc);

            if (!blkOk) {
              await serialWrite([XMODEM_NAK]);
              continue;
            }

            if (blkNum === ((expectedBlk - 1) & 0xFF)) {
              // Duplicate — ACK but don't store
              await serialWrite([XMODEM_ACK]);
              continue;
            }

            if (blkNum !== (expectedBlk & 0xFF)) {
              // Out of sequence — CAN
              await serialWrite([XMODEM_CAN, XMODEM_CAN]);
              throw new Error("Block sequence error (expected " + expectedBlk + " got " + blkNum + ")");
            }

            receivedBlocks.push(new Uint8Array(data));
            expectedBlk++;
            await serialWrite([XMODEM_ACK]);

            $("#chunk_ratio").text("Received block " + receivedBlocks.length + "…");
            var pct = Math.min(99, receivedBlocks.length);  // unknown total
            $("#file_upload_progress_bar").css("width", "50%");  // indeterminate
          }
        } else if (r.value[0] === XMODEM_EOT) {
          // Empty transfer
          await serialWrite([XMODEM_ACK]);
          started = true;
        }
      }
    } catch(e) {
      if (e.message === "cancelled") throw e;
      // timeout — retry 'C'
    }
  }

  if (!started && receivedBlocks.length === 0) {
    throw new Error("No response from sender after " + C_MAX + " attempts");
  }

  // Assemble
  var total = new Uint8Array(receivedBlocks.length * 128);
  for (var i = 0; i < receivedBlocks.length; i++) {
    total.set(receivedBlocks[i], i * 128);
  }
  return total;
}

// ── Send file flow ────────────────────────────────────────────────────────────

async function sendFile() {
  if (!serialPort) { alert("Connect your device via USB first."); return; }
  var fileInput = $("#broadcastFile").prop("files")[0];
  if (!fileInput) { alert("Select a file to send."); return; }

  setTransferUI(true);
  xmodemCancel = false;

  // Enable serial-over-LoRa on the ESP
  Socket.send(JSON.stringify({"request-type": "set_serial_mode", value: true}));
  await serialDelay(600);

  try {
    var arrayBuf = await fileInput.arrayBuffer();
    var bytes = new Uint8Array(arrayBuf);
    var totalBlocks = Math.ceil(bytes.length / 128);
    file_name = fileInput.name;
    $("#chunk_ratio").text("Sending \"" + file_name + "\" - " + totalBlocks + " blocks...");
    await xmodemSend(bytes, totalBlocks);
    $("#file_upload_progress_bar").css("width", "100%").addClass("bg-success");
    $("#chunk_ratio").text("Transfer complete.");
  } catch(e) {
    $("#file_upload_progress_bar").addClass("bg-danger");
    $("#chunk_ratio").text("Transfer failed: " + e.message);
  } finally {
    // Disable serial-over-LoRa
    Socket.send(JSON.stringify({"request-type": "set_serial_mode", value: false}));
    setTransferUI(false);
    xmodemCancel = false;
  }
}

// ── Receive file flow ─────────────────────────────────────────────────────────

async function receiveFile() {
  if (!serialPort) { alert("Connect your device via USB first."); return; }

  setTransferUI(true);
  xmodemCancel = false;

  // Enable serial-over-LoRa on the ESP
  Socket.send(JSON.stringify({"request-type": "set_serial_mode", value: true}));
  await serialDelay(600);

  try {
    $("#chunk_ratio").text("Ready — waiting for sender to start XModem transfer…");
    var received = await xmodemReceive();

    // Trim trailing 0x1A (SUB) pad bytes
    var end = received.length;
    while (end > 0 && received[end - 1] === 0x1A) end--;
    var trimmed = received.subarray(0, end);

    // Download
    var blob = new Blob([trimmed]);
    var url  = URL.createObjectURL(blob);
    var a    = document.createElement("a");
    a.href   = url;
    a.download = file_name || "voyager_received_file";
    a.click();
    URL.revokeObjectURL(url);

    $("#file_upload_progress_bar").css("width", "100%").addClass("bg-success");
    $("#chunk_ratio").text("File received and downloaded successfully.");
  } catch(e) {
    $("#file_upload_progress_bar").addClass("bg-danger");
    $("#chunk_ratio").text("Receive failed: " + e.message);
  } finally {
    Socket.send(JSON.stringify({"request-type": "set_serial_mode", value: false}));
    setTransferUI(false);
    xmodemCancel = false;
  }
}
