$(document).ready(function () {
  init_socket();
  dashboard();
});

$("#myModal").on("shown.bs.modal", function () {
  $("#myInput").trigger("focus");
});

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

function scan_ssid() {
  $("#wifi_ssid_list").html("");
  $("#wifi_scan_btn").html("Scanning...");
  $("#wifi_scan_btn").attr("onclick", "");
  Socket.send(JSON.stringify({ "request-type": "wifi_ssid_scan" }));
}

function wifi_connect() {
  debugger;
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
}

function update_wifi_ssid(ssid) {
  $("#wifi_ssid").attr("value", ssid);
}
function init_socket() {
  console.log("Initilizing web sockets.");
  Socket = new WebSocket("ws://" + window.location.hostname + "/ws");
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
