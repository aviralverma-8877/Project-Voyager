var hostname_url;
var loading_alert;
var promptModal;
var alertModel;
var wakeLock = null;
const canWakeLock = function(){
   return 'wakeLock' in navigator;
}
 
const setWakeLock = function () {
    if (!canWakeLock()) {
        console.error('Your browser is not support WakeLock API!');
        return;
    }
    if (wakeLock) {
        return;
    }
    navigator.wakeLock.request('screen').then(result => {
        wakeLock = result;
        console.log('Wake Lock is actived!');
        wakeLock.addEventListener('release', () => {
            wakeLock = null;
            console.log('Wake Lock is released!');
        });
    }).catch((err) => {
        console.error(`Wake Lock is faild：${err.message}`);
    });
};
 
$(document).ready(function () {
  init_socket();
  init_events();
  get_hostname();
  promptModal = new bootstrap.Modal($("#promptModal"), {});
  alertModel = new bootstrap.Modal($("#alertModal"), {});
  loading_alert = new bootstrap.Modal($("#loadingModal"), {});
  setWakeLock();
  $("#loading_model_body").html("Connecting...");
  loading_alert.show();
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
    Socket.send(
      JSON.stringify({
        "request-type": "lora_transmit",
        data: JSON.stringify(tx_msg),
        get_response: false,
      })
    );
    $("#lora_msg").val("");
    $("#lora_msg").attr("readonly", true);
    setTimeout(function () {
      $("#lora_msg").attr("readonly", false);
    }, 1000);
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
    $.get("/config/lora_serial.json", function (config) {
      $("#lora_to_serial").attr("checked", config.lora_serial);
      config_lora_to_serial_fields(config.lora_serial);
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
    $.get("/config/lora_serial.json", function (config) {
      config_lora_to_serial_fields(config.lora_serial);
    });
  });
}

function wifi() {
  $(".nav-link").removeClass("active");
  $("#navbar-wifi").addClass("active");
  $.get("wifi.html", function (data) {
    $("#main_content").html(data);
    $.get("/config/wifi_config.json", function (data) {
      $("#wifi_ssid_name").html(data.wifi_ssid);
    });
  });
}

function set_mqtt_enabled(val) {
  $("#mqtt_enabled").attr("checked", val);
  if (!val) {
    $(".mqtt_enabled").attr("disabled", "true");
    $("#mqtt_host").val("");
    $("#mqtt_port").val("");
    $("#mqtt_uname").val("");
    $("#mqtt_password").val("");
  } else {
    $(".mqtt_enabled").removeAttr("disabled");
  }
}

function set_mqtt_auth(val) {
  $("#mqtt_auth").attr("checked", val);
  if (!val) {
    $(".mqtt_auth").attr("disabled", "true");
    $("#mqtt_uname").val("");
    $("#mqtt_password").val("");
  } else {
    $(".mqtt_auth").removeAttr("disabled");
  }
}

function mqtt() {
  $(".nav-link").removeClass("active");
  $("#navbar-mqtt").addClass("active");
  $.get("mqtt.html", function (data) {
    $("#main_content").html(data);
    $.get("/config/mqtt_config.json", function (data) {
      $("#mqtt_enabled").attr("checked", data.mqtt_enabled);
      $("#mqtt_host").val(data.host);
      $("#mqtt_port").val(data.port);
      $("#mqtt_auth").attr("checked", data.auth);
      set_mqtt_enabled(data.mqtt_enabled);
      set_mqtt_auth(data.auth);
      if (data.auth) {
        $("#mqtt_uname").val(data.username);
        $("#mqtt_password").val(data.password);
      }
    });
  });
}

function save_mqtt() {
  enable = $("#mqtt_enabled").is(":checked") > 0;
  host = $("#mqtt_host").val();
  port = $("#mqtt_port").val();
  auth = $("#mqtt_auth").is(":checked") > 0;
  uname = $("#mqtt_uname").val();
  pass = $("#mqtt_password").val();
  $.get("/config/mqtt_config.json", function (data) {
    data.mqtt_enabled = enable;
    data.host = host;
    data.port = port;
    data.auth = auth;
    data.username = uname;
    data.password = pass;
    Socket.send(
      JSON.stringify({
        "request-type": "set_mqtt_config",
        val: JSON.stringify(data),
      })
    );
    alert("MQTT config saved.<br>New settings will be applied after reboot.");
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
  });
}

function alert(msg) {
  $("#alert_body").html(msg);
  alertModel.show();
}

function reset() {
  $("#promptModalLabel").html("Device Reset");
  $("#prompt_body").html("Are you sure you want to reset the device");
  promptModal.show();
  $("#promptModelProceed").click(function () {
    promptModal.hide();
    Socket.send(JSON.stringify({ "request-type": "reset_device" }));
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

var total_packets = 0;
var current_packet = 0;
var file_data = "";
var lastEventTimestamp = 0;
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
        init_events();
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

  source.addEventListener("RAW_DATA", function (e) {
    data = JSON.parse(e.data);
    if (lastEventTimestamp == data.millis) return;
    lastEventTimestamp = data.millis;
    var data = data.data;
    if (data != "") {
      file_data += data;
      $("#chunk_ratio").html(
        "(" + current_packet + " / " + total_packets + ") Received"
      );
      current_packet += 1;
      var percent = (current_packet / total_packets) * 100;
      $("#file_upload_progress_bar").css("width", percent + "%");
      $("#data_url_download_link").attr("href", file_data);
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
      if (heap_per > 30 && heap_per < 70)
        $("#heap_progress_bar").addClass("bg-warning");
      if (heap_per > 70) $("#heap_progress_bar").addClass("bg-danger");
      $("#heap_progress_bar").css("width", heap_per + "%");
    }
  });
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
          wifi_ssid = ssid_list[wifi_ssid];
          quality = 2 * (wifi_ssid.rssi + 100);
          output +=
            '<li class="list-group-item d-flex justify-content-between align-items-center">\
            <a href="#wifi_password" onclick="update_wifi_ssid(\'' +
            wifi_ssid.ssid +
            "')\">" +
            wifi_ssid.ssid +
            " (" +
            quality +
            " %)</a></li>";
        }
        $("#wifi_ssid_list").html(output);
      }
      if (response_type == "alert") {
        var msg = data.alert_msg;
        alert(msg);
      }
      if (response_type == "lora_rx") {
        data = JSON.parse(data.lora_msg);
        var uname = data.name;
        var data = JSON.parse(data.data);
        var pack_type = data["pack_type"];
        if (pack_type == "beacon") {
          //console.log("beacon from " + mac);
        }
        if (pack_type == "msg") {
          msg = data["data"];
          $("#lora_rx_msg").prepend(
            "<li class='list-group-item'>" + uname + " : " + msg + "</li>"
          );
        }
        if (pack_type == "action") {
          action = data["data"];
          if (action == "enable_file_tx_mode") {
            total_packets = data["total_packets"];
            current_packet = 0;
            $("#file_upload_progress_bar").addClass("bg-success");
            $("#chunk_ratio").html(
              "Total " + total_packets + " file chunks will be recieved."
            );
            file_data = "";
            start_file_transfer_mode();
          } else if (action == "disable_file_tx_mode") {
            stop_file_transfer_mode();
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

function config_lora_to_serial_fields(val) {
  if (val) {
    $(".lora_transmittion").attr("disabled", "true");
  } else {
    $(".lora_transmittion").removeAttr("disabled");
  }
}

function set_lora_to_serial(val) {
  config_lora_to_serial_fields(val);
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

function stop_broadcast() {
  stop_file_transfer_mode();
}

function file_broadcast() {
  if (transmission) return;
  const file = $("#broadcastFile").prop("files")[0];
  const reader = new FileReader();
  reader.readAsDataURL(file);
  reader.onload = function (e) {
    const dataURL = reader.result;
    const chunkSize = parseInt($("#chunk_size").val());
    const waitTime = parseInt($("#wait_time").val());
    $("#data_url_download_link").attr("href", "");
    if (chunkSize > 200 || chunkSize < 0) {
      alert("packet size should be between 1-200");
      return;
    }
    if (waitTime < 500) {
      alert("Wait time should be greater than 500ms");
      return;
    }
    const total_chunk = Math.floor(dataURL.length / chunkSize);
    const time_estimate = Math.abs(total_chunk * (waitTime / 1000));
    var h = Math.floor(time_estimate / 3600);
    var m = Math.floor((time_estimate % 3600) / 60);
    var s = Math.floor((time_estimate % 3600) % 60);
    $("#chunk_ratio").html(
      "Total " +
        total_chunk +
        " file chunks will be transmitted. Estimated time " +
        h +
        ":" +
        m +
        ":" +
        s +
        "."
    );
    $("#file_upload_progress_bar").removeClass("bg-success");
    function loop(s) {
      if (s < dataURL.length && transmission) {
        uploadChunk(
          dataURL.slice(s, s + chunkSize),
          () => {
            s += chunkSize;
            var percent = Math.abs((s / dataURL.length) * 100);
            $("#file_upload_progress_bar").css("width", percent + "%");
            setTimeout(() => {
              loop(s);
            }, waitTime);
          },
          () => {
            tx_msg = { pack_type: "action", data: "disable_file_tx_mode" };
            Socket.send(
              JSON.stringify({
                "request-type": "lora_transmit",
                data: JSON.stringify(tx_msg),
                get_response: false,
              })
            );
          }
        );
      } else {
        stop_broadcast();
        tx_msg = { pack_type: "action", data: "disable_file_tx_mode" };
        Socket.send(
          JSON.stringify({
            "request-type": "lora_transmit",
            data: JSON.stringify(tx_msg),
            get_response: false,
          })
        );
      }
    }
    setTimeout(() => {
      tx_msg = {
        pack_type: "action",
        data: "enable_file_tx_mode",
        total_packets: total_chunk,
      };
      Socket.send(
        JSON.stringify({
          "request-type": "lora_transmit",
          data: JSON.stringify(tx_msg),
          get_response: false,
        })
      );
      setTimeout(() => {
        transmission = true;
        loop(0);
      }, 2000);
    }, 2000);
  };
  reader.onerror = function (e) {
    //console.log("Error : " + e.type);
  };
}

function uploadChunk(chunk, passed_callback, failed_callback) {
  var _href = $("#data_url_download_link").attr("href");
  $("#data_url_download_link").attr("href", _href + chunk);
  // console.log(chunk);
  $.post("/send_raw", { data: chunk }, (timeout = 5))
    .done(function () {
      passed_callback();
    })
    .fail(function () {
      failed_callback();
    });
}

var file_transfer_mode = false;
function start_file_transfer_mode() {
  $("#data_url_download_link").attr("href", "");
  file_transfer_mode = true;
}
function stop_file_transfer_mode() {
  transmission = false;
  file_transfer_mode = false;
}
