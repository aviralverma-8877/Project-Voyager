var hostname_url;
$(document).ready(function () {
  init_socket();
  get_hostname();
  dashboard();
});

$("#myModal").on("shown.bs.modal", function () {
  $("#myInput").trigger("focus");
});

function file_broadcast() {
  const file = $("#broadcastFile").prop("files")[0];
  const chunkSize = 200;
  let start = 0;
  let i = 0;
  while (start < file.size) {
    uploadChunk(file.slice(start, start + chunkSize));
    start += chunkSize;
  }
}

function uploadChunk(chunk) {
  const reader = new FileReader();
  reader.onload = function (e) {
    const dataURL = reader.result;
    console.log(dataURL);
  };
  reader.onerror = function (e) {
    console.log("Error : " + e.type);
  };
  reader.readAsDataURL(chunk);
}

function send_lora(msg) {
  if (msg != "") {
    Socket.send(
      JSON.stringify({
        "request-type": "lora_transmit",
        data: msg,
        get_response: false,
      })
    );
    $("#lora_msg").val("");
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
  $.get("dashboard.html", function (data) {
    $("#main_content").html(data);
  });
}

function wifi() {
  $.get("wifi.html", function (data) {
    $("#main_content").html(data);
  });
}

function update() {
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

function update_wifi_ssid(ssid) {
  $("#wifi_ssid").attr("value", ssid);
}
function init_socket() {
  console.log("Initilizing web sockets.");
  Socket = new WebSocket(
    "ws://" + window.location.hostname + ":" + window.location.port + "/ws"
  );
  Socket.onmessage = function (event) {
    var data = JSON.parse(event.data);
    var response_type = data.response_type;
    console.log("Web socket message recieved...");
    console.log(data);
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
      var msg = data.lora_msg;
      $("#lora_rx_msg").prepend("<li class='list-group-item'>" + msg + "</li>");
    }
  };
  Socket.onopen = function (event) {
    console.log("Connected to web sockets...");
  };
  Socket.onclose = function (event) {
    console.log("Connection to websockets closed....");
    setTimeout(function () {
      console.log("Retrying websocket connection....");
      init_socket();
    }, 1000);
  };
  Socket.onerror = function (event) {
    console.log("Error in websockets");
  };
}
