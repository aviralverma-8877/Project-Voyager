var hostname_url;
$(document).ready(function () {
  init_socket();
  get_hostname();
});

$("#myModal").on("shown.bs.modal", function () {
  $("#myInput").trigger("focus");
});

function set_sync_word(val) {
  $.get("lora_config.json", function (lora_config) {
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
  var alertModal = new bootstrap.Modal($("#promptModal"), {});
  alertModal.show();
  $("#promptModelProceed").click(function () {
    alertModal.hide();
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
    $.get('/lora_serial.json', function(config){
      $("#lora_to_serial").attr("checked", config.lora_serial)
      config_lora_to_serial_fields(config.lora_serial)
      setTimeout(function () {
        get_username();
        set_sync_word();
      }, 10);  
    })
  });
}

function file_transfer()
{
  $.get("file_transfer.html", function(data){
    $("#main_content").html(data);
    $.get('/lora_serial.json', function(config){
      config_lora_to_serial_fields(config.lora_serial)  
    });
  });
}

function wifi() {
  $(".nav-link").removeClass("active");
  $("#navbar-wifi").addClass("active");
  $.get("wifi.html", function (data) {
    $("#main_content").html(data);
  });
}

function save_lora_config() {
  $.get("lora_config.json", function (lora_config) {
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

function lora() {
  $(".nav-link").removeClass("active");
  $("#navbar-lora").addClass("active");
  $.get("lora.html", function (data) {
    $("#main_content").html(data);
    $.get("lora_config.json", function (lora_config) {
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
    });
  });
}

function update() {
  $(".nav-link").removeClass("active");
  $("#navbar-update").addClass("active");
  $.get("update", function (data) {
    $("#main_content").html(data);
  });
}

function reset() {
  $("#promptModalLabel").html("Device Reset");
  $("#prompt_body").html("Are you sure you want to reset the device");
  var alertModal = new bootstrap.Modal($("#promptModal"), {});
  alertModal.show();
  $("#promptModelProceed").click(function () {
    alertModal.hide();
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
function init_socket() {
  //console.log("Initilizing web sockets.");
  Socket = new WebSocket(
    "ws://" + window.location.hostname + ":" + window.location.port + "/ws"
  );
  Socket.onmessage = function (event) {
    console.log(event.data)
    try{
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
          quality = 2 * (wifi_ssid.rssi + 100)
          output +=
            '<li class="list-group-item d-flex justify-content-between align-items-center">\
            <a href="#wifi_password" onclick="update_wifi_ssid(\'' +
            wifi_ssid.ssid +
            "')\">" +
            wifi_ssid.ssid +
            ' ('+ quality +' %)</a></li>';
        }
        $("#wifi_ssid_list").html(output);
      }
      if (response_type == "alert") {
        var msg = data.alert_msg;
        alert(msg);
      }
      if (response_type == "lora_rx") {
        data = JSON.parse(data.lora_msg);
        if (typeof data == "string") {
          if (file_transfer_mode) {
            var _href = $("#file_download").attr("src");
            $("#file_download").attr("src", _href + data);
            return;
          }
        } 
        else 
        {
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
        }
        if (pack_type == "action") {
          action = data["data"];
          if (action == "enable_file_tx_mode") {
            $("#file_download").attr("src", "");
            Socket.send(
              JSON.stringify({
                "request-type": "enable_LoRa_file_rx_mode",
              })
            );
            start_file_transfer_mode();
          } else if (action == "disable_file_tx_mode") {
            Socket.send(
              JSON.stringify({
                "request-type": "disable_LoRa_file_rx_mode",
              })
            );
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
    }
    catch(e){
      console.log(e);
    }
  };
  Socket.onopen = function (event) {
    //console.log("Connected to web sockets...");
    dashboard();
  };
  Socket.onclose = function (event) {
    //console.log("Connection to websockets closed....");
    setTimeout(function () {
      //console.log("Retrying websocket connection....");
      init_socket();
    }, 1000);
  };
  Socket.onerror = function (event) {
    console.log("Error in websockets"+event);
  };
}

function config_lora_to_serial_fields(val)
{
  if(val)
  {
    $(".lora_transmittion").attr("disabled","true");
  }
  else
  {
    $(".lora_transmittion").removeAttr("disabled");
  } 
}

function set_lora_to_serial(val)
{
  config_lora_to_serial_fields(val);
  Socket.send(JSON.stringify({
    "request-type": "set_serial_mode",
    "value":val
  }))
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

function reset_progress_bar()
{
  $("#file_upload_progress_bar").css("width","0%");
}

function file_broadcast() {
  const file = $("#broadcastFile").prop("files")[0];
  const reader = new FileReader();
  reader.readAsDataURL(file);
  reader.onload = function (e) {
    const dataURL = reader.result;
    const chunkSize = parseInt($("#chunk_size").val());
    const waitTime = parseInt($("#wait_time").val());
    if (chunkSize > 250 || chunkSize < 0) {
      alert("packet size should be between 1-250");
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
    function loop(s) {
      if (s < dataURL.length) {
        uploadChunk(dataURL.slice(s, s + chunkSize));
        s += chunkSize;
        var percent = Math.abs((s / dataURL.length) * 100);
        $("#file_upload_progress_bar").css("width", percent + "%");
        setTimeout(()=>{loop(s)}, waitTime);
      } else {
        Socket.send(
          JSON.stringify({
            "request-type": "disable_LoRa_file_tx_mode",
          })
        );
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
    Socket.send(
      JSON.stringify({
        "request-type": "enable_LoRa_file_tx_mode",
      })
    );
    setTimeout(() => {
      tx_msg = {
        pack_type: "action",
        data: "enable_file_tx_mode",
      };
      Socket.send(
        JSON.stringify({
          "request-type": "lora_transmit",
          data: JSON.stringify(tx_msg),
          get_response: false,
        })
      );
      setTimeout(()=>{loop(0)}, 2000);
    }, 2000);
  };
  reader.onerror = function (e) {
    //console.log("Error : " + e.type);
  };
}

function uploadChunk(chunk) {
  //console.log(chunk);
  Socket.send(
    JSON.stringify({
      "request-type": "send_raw",
      val: JSON.stringify(chunk),
    })
  );
}
var file_transfer_mode = false;
function start_file_transfer_mode() {
  file_transfer_mode = true;
}
function stop_file_transfer_mode() {
  file_transfer_mode = false;
}