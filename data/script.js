$(document).ready(function () {
    init_socket();
    dashboard();
});

function dashboard() {
    $.get("dashboard.html", function (data) {
        $("#main_content").html(data);
    })
}

function wifi() {
    $.get("wifi.html", function (data) {
        $("#main_content").html(data);
    })
}

function scan_ssid() {
    $("#wifi_ssid_list").html("");
    $("#wifi_scan_btn").html("Scanning...");
    $("#wifi_scan_btn").attr("onclick", "");
    Socket.send(JSON.stringify({ 'request-type': 'wifi_ssid_scan' }))
}

function init_socket() {
    console.log("Initilizing web sockets.")
    Socket = new WebSocket('ws://' + window.location.hostname + "/ws");
    Socket.onmessage = function (event) {
        var data = JSON.parse(event.data);
        var response_type = data.response_type;
        console.log("Web socket message recieved...");
        console.log(data);
        if (response_type == "wifi_scan") {
            $("#wifi_scan_btn").html("Scan SSID");
            $("#wifi_scan_btn").attr("onclick", "scan_ssid()");
            var ssid_list = data.SSID
            var output = ""
            for (wifi_ssid in ssid_list) {
                wifi_ssid = ssid_list[wifi_ssid]
                output += '<li class="list-group-item d-flex justify-content-between align-items-center"><a href="#" onclick="connect_wifi(\'' + wifi_ssid.ssid + '\')">' + wifi_ssid.ssid + '</a><span class="badge badge-primary badge-pill">' + wifi_ssid.rssi + '</span></li>';
            }
            $("#wifi_ssid_list").html(output);
        }
    }
    Socket.onopen = function (event) {
        console.log("Connected to web sockets...")
    }
    Socket.onclose = function (event) {
        console.log("Connection to websockets closed....")
        setTimeout(function () {
            console.log("Retrying websocket connection....")
            init_socket();
        }, 1000);
    }
    Socket.onerror = function (event) {
        console.log("Error in websockets");
    }
}