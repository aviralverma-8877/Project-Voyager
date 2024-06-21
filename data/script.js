var hostname_url;
$(document).ready(function () {
  init_socket();
  get_hostname();
});

$("#myModal").on("shown.bs.modal", function () {
  $("#myInput").trigger("focus");
});

function change_sync_word(val) {
  if (val != "") {
    Socket.send(
      JSON.stringify({
        "request-type": "set_sync_word",
        val: val,
      })
    );
  }
}

function get_sync_word() {
  Socket.send(
    JSON.stringify({
      "request-type": "get_sync_word",
    })
  );
}

function set_sync_word(val) {
  $("#sync_word").val(val);
}

function generate_sync_word() {
  for (var i = 1; i < 256; i++) $("#sync_word").append(new Option(i, i));
}

function reset_progress_bar() {
  $("#file_upload_progress_bar").css("width", "0%");
  $("#chunk_ratio").html("");
}

function file_broadcast() {
  const file = $("#broadcastFile").prop("files")[0];
  const reader = new FileReader();
  reader.readAsDataURL(file);
  reader.onload = function (e) {
    const dataURL = reader.result;
    const chunkSize = 250;
    let start = 0;
    const waitTime = 1000;
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
    function loop() {
      if (start < dataURL.length) {
        uploadChunk(dataURL.slice(start, start + chunkSize));
        start += chunkSize;
        var percent = Math.abs((start / dataURL.length) * 100);
        $("#file_upload_progress_bar").css("width", percent + "%");
        setTimeout(loop, waitTime);
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
      tx_msg = { pack_type: "action", data: "enable_file_tx_mode" };
      Socket.send(
        JSON.stringify({
          "request-type": "lora_transmit",
          data: JSON.stringify(tx_msg),
          get_response: false,
        })
      );
      setTimeout(loop, 2000);
    }, 2000);
  };
  reader.onerror = function (e) {
    //console.log("Error : " + e.type);
  };
}

function uploadChunk(chunk) {
  console.log(chunk);
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
  $(".nav-item").removeClass("active");
  $("#navbar-dashboard").addClass("active");
  $.get("dashboard.html", function (data) {
    $("#main_content").html(data);
    generate_sync_word();
    setTimeout(function () {
      get_username();
      get_sync_word();
    }, 10);
  });
}

function wifi() {
  $(".nav-item").removeClass("active");
  $("#navbar-wifi").addClass("active");
  $.get("wifi.html", function (data) {
    $("#main_content").html(data);
  });
}

function update() {
  $(".nav-item").removeClass("active");
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

function update_wifi_ssid(ssid) {
  $("#wifi_ssid").attr("value", ssid);
}
function init_socket() {
  //console.log("Initilizing web sockets.");
  Socket = new WebSocket(
    "ws://" + window.location.hostname + ":" + window.location.port + "/ws"
  );
  Socket.onmessage = function (event) {
    var data = JSON.parse(event.data);
    var response_type = data.response_type;
    //console.log("Web socket message recieved...");
    //console.log(data);
    if (response_type == "wifi_scan") {
      $("#wifi_scan_btn").html("Scan SSID");
      $("#wifi_scan_btn").attr("onclick", "scan_ssid()");
      var ssid_list = data.SSID;
      var output = "";
      for (wifi_ssid in ssid_list) {
        wifi_ssid = ssid_list[wifi_ssid];
        output +=
          '<li class="list-group-item d-flex justify-content-between align-items-center">\
          <a href="#" onclick="update_wifi_ssid(\'' +
          wifi_ssid.ssid +
          "')\">" +
          wifi_ssid.ssid +
          '</a><span class="badge badge-primary badge-pill">' +
          wifi_ssid.rssi +
          "</span></li>";
      }
      $("#wifi_ssid_list").html(output);
    }
    if (response_type == "alert") {
      var msg = data.alert_msg;
      alert(msg);
    }
    if (response_type == "lora_rx") {
      var data = JSON.parse(data.lora_msg);
      console.log(data);
      if (file_transfer_mode) {
        var _href = $("#file_download").attr("src");
        $("#file_download").attr("src", _href + data);
        return;
      }
      var mac = data.mac;
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
  };
  Socket.onopen = function (event) {
    //console.log("Connected to web sockets...");
    stop_file_transfer_mode();
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
    //console.log("Error in websockets");
  };
}
