var hostname_url;
var loading_alert;
var promptModal;
var alertModel;

function getEleById(id)
{
    if(document.getElementById(id) != null)
    {
        return document.getElementById(id);
    }
    else{
        return document.createElement("null");
    }
}

function JSONToFile(obj, filename){
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

function httpGET(theUrl, passed_callback = function(){}, failed_callback = function(){}){
  var xmlHttp = new XMLHttpRequest();
  xmlHttp.open( "GET", theUrl, false );
  xmlHttp.onreadystatechange = function() {
    if(xmlHttp.readyState == 4 && xmlHttp.status == 200) {
        data = xmlHttp.responseText
        try{
            return_data = JSON.parse(data);
            passed_callback(return_data);
        }
        catch
        {
            passed_callback(data);
        }
    }
    else{
      failed_callback();
    }
  }
  xmlHttp.send( null );
}

function httpPOST(theUrl, params, passed_callback = function(){}, failed_callback = function(){}){
  var xmlHttp = new XMLHttpRequest();
  xmlHttp.open( "POST", theUrl, false );
  xmlHttp.setRequestHeader('Content-type', 'application/x-www-form-urlencoded');
  xmlHttp.onreadystatechange = function() {
    if(xmlHttp.readyState == 4 && xmlHttp.status == 200) {
      passed_callback(xmlHttp.responseText);
    }
    else{
      failed_callback();
    }
  }
  xmlHttp.send(params);
}

function init() {
  init_socket();
  init_events();
  get_hostname();
  promptModal = new bootstrap.Modal(getEleById("promptModal"), {});
  alertModel = new bootstrap.Modal(getEleById("alertModal"), {});
  loading_alert = new bootstrap.Modal(getEleById("loadingModal"), {});
  getEleById("loading_model_body").innerHTML = "Connecting...";
  loading_alert.show();
  window.onbeforeunload = function () {
    return "Are you sure you want to leave?";
  };
}

function set_sync_word(val) {
  httpGET("/config/lora_config.json", function (lora_config) {
    getEleById("sync_word").value = lora_config.SyncWord;
  });
}

function generate_sync_word() {
  for (var i = 1; i < 256; i++) getEleById("sync_word").append(new Option(i, i));
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
    getEleById("lora_msg").value = "";
    getEleById("lora_msg").setAttribute("readonly", true);
    httpPOST("/lora_transmit", { data: JSON.stringify(tx_msg) }, function (data) {
        getEleById("lora_msg").setAttribute("readonly", false);
        if (data.akn == 1) {
          getEleById("lora_rx_msg").prepend(
            "<li class='list-group-item'>" +
              data.username +
              " : " +
              msg +
              "</li>"
          );
        }
      }, function (data) {
        getEleById("lora_msg").setAttribute("readonly", false);
      });
  }
}

function restart() {
  getEleById("promptModalLabel").innerHTML = "Device Restart";
  getEleById("prompt_body").innerHTML = "Are you sure you want to restart the device";
  promptModal.show();
  getEleById("promptModelProceed").click(function () {
    promptModal.hide();
    Socket.send(JSON.stringify({ "request-type": "restart_device" }));
  });
}

function get_hostname() {
  httpGET("hostname", function (data) {
    hostname_url = "http://" + data + ".local/";
    getEleById("project_title").setAttribute("href", hostname_url);
  });
}

function dashboard() {
    Array.prototype.forEach.call(document.getElementsByClassName("nav-link"),function(item){
    item.removeAttribute("active"); 
  });
  getEleById("navbar-dashboard").classList.add("active");
  httpGET("dashboard.html", function (data) {
    getEleById("main_content").innerHTML = data;
    var block_input_status = transmission;
    httpGET("/config/lora_serial.json", function (config) {
      getEleById("lora_to_serial").setAttribute("checked", config.lora_serial);
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
  httpGET("file_transfer.html", function (data) {
    getEleById("main_content").innerHTML = data;
    var block_input_status = transmission;
    httpGET("/config/lora_serial.json", function (config) {
      block_input_status = block_input_status || config.lora_serial;
      block_inputs(block_input_status);
    });
  });
}

function wifi() {
    Array.prototype.forEach.call(document.getElementsByClassName("nav-link"),function(item){
    item.removeAttribute("style"); 
  });
  getEleById("navbar-wifi").classList.add("active");
  httpGET("wifi.html", function (data) {
    getEleById("main_content").innerHTML = data;
    block_inputs(transmission);
    httpGET("/config/wifi_config.json", function (data) {
      getEleById("wifi_ssid_name").innerHTML = data.wifi_ssid;
    });
  });
}

function save_lora_config() {
  httpGET("/config/lora_config.json", function (lora_config) {
    freq = getEleById("freq_range").value;
    tx_power = getEleById("tx_power_range").value;
    s_fact = getEleById("spreading_factor").value;
    bandwidth = getEleById("bandwidth").value;
    coding_rate = getEleById("coding_rate").value;
    sync_word = getEleById("sync_word").value;

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
  const file = getEleById("LoraConfigFile").prop("files")[0];
  const reader = new FileReader();
  reader.readAsText(file);
  reader.onload = function (e) {
    try {
      lora_config = JSON.parse(reader.result);
      getEleById("LoraConfigFile").value = "";
      set_lora_config(lora_config);
      alert("Settings applied.<br>Press save to apply.");
    } catch (e) {
      alert("Invalid file input.");
    }
  };
}

function download_lora_config() {
  httpGET("/config/lora_config.json", function (lora_config) {
    freq = getEleById("freq_range").value;
    tx_power = getEleById("tx_power_range").value;
    s_fact = getEleById("spreading_factor").value;
    bandwidth = getEleById("bandwidth").value;
    coding_rate = getEleById("coding_rate").value;
    sync_word = getEleById("sync_word").value;

    lora_config["freq"] = parseInt(freq * 1000000);
    lora_config["TxPower"] = parseInt(tx_power);
    lora_config["SpreadingFactor"] = parseInt(s_fact);
    lora_config["SignalBandwidth"] = parseInt(bandwidth);
    lora_config["CodingRate4"] = parseInt(coding_rate);
    lora_config["SyncWord"] = parseInt(sync_word);
    JSONToFile(lora_config, "project_voyager_lora_config.json");
  });
}

function lora() {
    Array.prototype.forEach.call(document.getElementsByClassName("nav-link"),function(item){
    item.removeAttribute("active"); 
  });
  getEleById("navbar-lora").classList.add("active");
  httpGET("lora.html", function (data) {
    getEleById("main_content").innerHTML = data;
    block_inputs(transmission);
    httpGET("/config/lora_config.json", function (lora_config) {
      generate_sync_word();

      getEleById("freq_range").value = (lora_config.freq / 1000000);
      getEleById("selected_lora_freq").innerHTML = lora_config.freq / 1000000;

      getEleById("tx_power_range").value = lora_config.TxPower;
      getEleById("selected_lora_tx_power").innerHTML = lora_config.TxPower;

      getEleById("spreading_factor").value = lora_config.SpreadingFactor;
      getEleById("selected_lora_spreading_factor").innerHTML = lora_config.SpreadingFactor;

      getEleById("bandwidth").value = lora_config.SignalBandwidth;

      getEleById("coding_rate").value = lora_config.CodingRate4;
      getEleById("selected_lora_coding_rate").innerHTML = lora_config.CodingRate4;

      getEleById("sync_word").value = lora_config.SyncWord;
      httpGET("/config/config_list.json", function (config_list) {
        config_list.forEach((element) => {
          getEleById("lora_config").append(
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

  getEleById("freq_range").value = lora_config.freq / 1000000;
  getEleById("selected_lora_freq").innerHTML = lora_config.freq / 1000000;

  getEleById("tx_power_range").value = lora_config.TxPower;
  getEleById("selected_lora_tx_power").innerHTML = lora_config.TxPower;

  getEleById("spreading_factor").value = lora_config.SpreadingFactor;
  getEleById("selected_lora_spreading_factor").innerHTML = lora_config.SpreadingFactor;

  getEleById("bandwidth").value = lora_config.SignalBandwidth;

  getEleById("coding_rate").value = lora_config.CodingRate4;
  getEleById("selected_lora_coding_rate").innerHTML = lora_config.CodingRate4;

  getEleById("sync_word").value = lora_config.SyncWord;
}

function load_lora_config(val) {
  if (val != "") {
    httpGET("/config/" + val, function (lora_config) {
      set_lora_config(lora_config);
    });
  }
}

function update() {
    Array.prototype.forEach.call(document.getElementsByClassName("nav-link"), function(item){
    item.removeAttribute("active"); 
  });
  getEleById("navbar-update").classList.add("active");
  httpGET("update", function (data) {
    getEleById("main_content").innerHTML = data;
    block_inputs(transmission);
  });
}

function alert(msg) {
  getEleById("alert_body").innerHTML = msg;
  alertModel.show();
}

function reset() {
  getEleById("promptModalLabel").innerHTML = "Device Reset";
  getEleById("prompt_body").innerHTML = "Are you sure you want to reset the device ?";
  promptModal.show();
  getEleById("promptModelProceed").click(function () {
    promptModal.hide();
    Socket.send(JSON.stringify({ "request-type": "reset_device" }));
    setTimeout(function () {
      window.location.replace(hostname_url);
    }, 10000);
  });
}

function scan_ssid() {
  getEleById("wifi_ssid_list").innerHTML = "";
  getEleById("wifi_scan_btn").innerHTML = "Scanning...";
  getEleById("wifi_scan_btn").setAttribute("onclick", "");
  Socket.send(JSON.stringify({ "request-type": "wifi_ssid_scan" }));
}

function wifi_connect() {
  ssid = getEleById("wifi_ssid").value;
  psk = getEleById("wifi_password").value;
  if (ssid == "" || ssid == undefined) {
    alert("Invalid SSID.");
    return;
  }
  if (psk == "" || psk == undefined) {
    alert("Invalid password.");
    return;
  }
  getEleById("loading_model_body").innerHTML = "Connecting to WiFi...";
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
  getEleById("promptModalLabel").innerHTML = "Access Point";
  getEleById("prompt_body").innerHTML = "Are you sure you want to switch to access point mode ?";
  promptModal.show();
  getEleById("promptModelProceed").click(function () {
    promptModal.hide();
    httpGET("/username", function (username) {
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
  getEleById("wifi_ssid").setAttribute("value", ssid);
}

var total_packets = 0;
var current_packet = 0;
var file_data = "";
var file_name = "";
var lastEventTimestamp = 0;
var lastDebugEventTimestamp = 0;
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

  source.addEventListener("RAW_DATA", function (e) {
    data = JSON.parse(e.data);
    if (lastEventTimestamp == data.millis) return;
    lastEventTimestamp = data.millis;
    var data = data.data;
    if (data != "") {
      // Socket.send(
      //   JSON.stringify({
      //     "request-type": "send_akn",
      //     "akn": 1,
      //   })
      // );
      file_data += data;
      file_size_string = get_string_size(file_data);
      getEleById("chunk_ratio").innerHTML = 
        "(" +
          current_packet +
          " / " +
          total_packets +
          ") " +
          file_size_string +
          " Received";
      current_packet += 1;
      var percent = (current_packet / total_packets) * 100;
      getEleById("file_upload_progress_bar").style.width = percent + "%";
    }
  });
  source.addEventListener("WIFI_DATA", function (e) {
    var data = JSON.parse(e.data);
    if (data != "") {
      var rssi = parseInt(data.rssi);
      quality = 2 * (rssi + 100);
      getEleById("wifi_quality").innerHTML = quality+" %";
    }
  });
  source.addEventListener("RAM_DATA", function (e) {
    var data = JSON.parse(e.data);
    if (data != "") {
      var free_heap = parseInt(data.free_heap);
      var heap_size = parseInt(data.heap_size);
      var heap_per = Math.round(((heap_size - free_heap) / heap_size) * 100);
      getEleById("ram_ratio").innerHTML ="<b>" + heap_per + "%</b> (" + free_heap + "/" + heap_size + ")";
      getEleById("heap_progress_bar").classList.remove("bg-success");
      getEleById("heap_progress_bar").classList.remove("bg-warning");
      getEleById("heap_progress_bar").classList.remove("bg-danger");
      if (heap_per < 30) getEleById("heap_progress_bar").classList.add("bg-success");
      if (heap_per > 30 && heap_per <= 70)
        getEleById("heap_progress_bar").classList.add("bg-warning");
      if (heap_per > 70) getEleById("heap_progress_bar").classList.add("bg-danger");
      getEleById("heap_progress_bar").style.width = heap_per + "%";
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
      getEleById("debug_textarea").prepend(time + " > " + data + "\n");
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
      if (response_type == "stop_transmission") {
        stop_file_transfer_mode();
      }
      if (response_type == "wifi_scan") {
        getEleById("wifi_scan_btn").innerHTML = "Scan SSID";
        getEleById("wifi_scan_btn").setAttribute("onclick", "scan_ssid()");
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
        getEleById("wifi_ssid_list").innerHTML = output;
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
          getEleById("lora_rx_msg").prepend(
            "<li class='list-group-item'>" + uname + " : " + msg + "</li>"
          );
        }
        if (pack_type == "action") {
          action = data["data"];
          if (action == "enable_file_tx_mode") {
            total_packets = data["total_packets"];
            file_size = data["size"];
            file_name = data["name"];
            time = data["time"];
            current_packet = 0;
            getEleById("file_upload_progress_bar").classList.add("bg-success");
            getEleById("rec_info").innerHTML =
              "Total " +
                total_packets +
                " file chunks will be recieved." +
                "<br />Total Size: <b>" +
                file_size +
                "</b><br />File Name: <b>" +
                file_name +
                "</b><br />Estimated min time <b>" +
                time +
                "</b>";
            start_file_transfer_mode();
          } else if (action == "disable_file_tx_mode") {
            stop_file_transfer_mode();
          } else if (action == "sos") {
            alert("User " + uname + " has sent a SOS request.");
          }
        }
      }
      if (response_type == "set_uname") {
        getEleById("username").value = data.uname;
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
    getEleById("loading_model_body").innerHTML = "Connecting...";
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
    Array.prototype.forEach.call(document.getElementsByClassName("lora_transmission"), function(item){
        item.setAttribute("disabled", "true");
      });
  } else {
    Array.prototype.forEach.call(document.getElementsByClassName("lora_transmission"), function(item){
        item.removeAttribute("disabled");
      });
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
  getEleById("selected_lora_freq").innerHTML = val;
}
function set_tx_power(val) {
  getEleById("selected_lora_tx_power").innerHTML = val;
}
function set_spreading_factor(val) {
  getEleById("selected_lora_spreading_factor").innerHTML = val;
}

function set_coding_rate(val) {
  getEleById("selected_lora_coding_rate").innerHTML = val;
}

function reset_progress_bar() {
  getEleById("file_upload_progress_bar").style.width = "0%";
}

var transmission = false;

function stop_broadcast() {
  stop_file_transfer_mode();
}

function dataURItoBlob(dataURI) {
  var byteString = atob(dataURI.split(",")[1]);

  var mimeString = dataURI.split(",")[0].split(":")[1].split(";")[0];

  var ab = new ArrayBuffer(byteString.length);
  var ia = new Uint8Array(ab);
  for (var i = 0; i < byteString.length; i++) {
    ia[i] = byteString.charCodeAt(i);
  }
  return new Blob([ab], { type: mimeString });
}

function downloadFile() {
  if (transmission) {
    alert("File transfer in progress.");
    return;
  }
  const link = document.createElement("a");
  const file = dataURItoBlob(file_data);
  link.href = URL.createObjectURL(file);
  if (file_name == "") link.download = "Voyager_Download";
  else link.download = file_name;
  link.click();
  URL.revokeObjectURL(link.href);
};

function file_broadcast() {
  if (transmission) return;
  const file = getEleById("broadcastFile").prop("files")[0];
  const reader = new FileReader();
  reader.readAsDataURL(file);
  reader.onload = function (e) {
    const dataURL = reader.result;
    const chunkSize = parseInt(getEleById("chunk_size").value);
    const waitTime = parseInt(getEleById("wait_time").value);
    file_data = "";
    file_name = file.name;
    if (chunkSize > 200 || chunkSize < 0) {
      alert("packet size should be between 1-200");
      return;
    }
    if (waitTime < 10) {
      alert("Wait time should be greater than 10 ms");
      return;
    }
    file_size_string = get_string_size(dataURL);
    const total_chunk = Math.floor(dataURL.length / chunkSize);
    var time_estimate;
    if (waitTime < 500) {
      time_estimate = Math.abs(total_chunk * (500 / 1000));
    } else {
      time_estimate = Math.abs(total_chunk * (waitTime / 1000));
    }

    var h = Math.floor(time_estimate / 3600);
    var m = Math.floor((time_estimate % 3600) / 60);
    var s = Math.floor((time_estimate % 3600) % 60);
    getEleById("chunk_ratio").innerHTML = 
      "Total <b>" +
        total_chunk +
        "</b> file chunks will be transmitted.<br>" +
        "Total Size <b>" +
        file_size_string +
        "</b>.<br>" +
        "Estimated min time <b>" +
        h +
        ":" +
        m +
        ":" +
        s +
        "</b>.";
    getEleById("file_upload_progress_bar").classList.remove("bg-success");
    function loop(s) {
      if (s < dataURL.length && transmission) {
        function passed_callback() {
          s += chunkSize;
          var percent = Math.abs((s / dataURL.length) * 100);
          getEleById("file_upload_progress_bar").style.width = percent + "%";
          setTimeout(() => {
            loop(s);
          }, waitTime);
        }
        function failed_callback() {
          stop_broadcast();
          tx_msg = { pack_type: "action", data: "disable_file_tx_mode" };
          httpPOST("/lora_transmit", { data: JSON.stringify(tx_msg) }, function (data) {
              if (data.akn == 1) {
                stop_broadcast();
              }
            });
        }
        var retry = 0;
        uploadChunk(dataURL.slice(s, s + chunkSize), passed_callback, () => {
          setTimeout(() => {
            if (retry < 4) {
              retry++;
              passed_callback();
            } else {
              failed_callback();
            }
          }, 1000);
        });
      } else {
        stop_broadcast();
        tx_msg = { pack_type: "action", data: "disable_file_tx_mode" };
        httpPOST("/lora_transmit", { data: JSON.stringify(tx_msg) }, function (data) {
            if (data.akn == 1) {
              stop_broadcast();
            }
          });
      }
    }
    setTimeout(() => {
      tx_msg = {
        pack_type: "action",
        data: "enable_file_tx_mode",
        total_packets: total_chunk,
        size: file_size_string,
        name: file_name,
        time: h + ":" + m + ":" + s,
      };
      httpPOST("/lora_transmit", { data: JSON.stringify(tx_msg) }, function (data) {
          if (data.akn == 1) {
            start_file_transfer_mode();
            setTimeout(() => {
              loop(0);
            }, 2000);
          }
        });
    }, 2000);
  };
  reader.onerror = function (e) {
    //console.log("Error : " + e.type);
  };
}

function uploadChunk(chunk, passed_callback, failed_callback) {
  file_data += chunk;
  // console.log(chunk);
  httpPOST("/send_raw", { data: chunk }, function () {
      passed_callback();
    }, function () {
      failed_callback();
    });
}

function start_file_transfer_mode() {
  transmission = true;
  file_data = "";
  block_inputs(transmission);
}
function stop_file_transfer_mode() {
  if (transmission) alert("Transmission Stopped/finished.");
  transmission = false;
  block_inputs(transmission);
}
