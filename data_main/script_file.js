/*
 * Project Voyager web UI - File transfer page.
 * Function declarations only: the shared constants and `state` live in
 * script.js, which index.html loads last (see the loader there).
 */
"use strict";

/* ------------------------------------------------------------------ */
/* File transfer                                                      */
/* ------------------------------------------------------------------ */

function file_broadcast() {
  if (state.sending) {
    alertText("A file transfer is already in progress.");
    return;
  }
  if (receiver.isActive()) {
    alertText("A file is being received. Cancel receiving first.");
    return;
  }
  if (state.sensorMode) {
    alertText("Sensor broadcast is on. Turn it off in the Sensors tab to transfer files.");
    return;
  }
  const files = $("#broadcastFile").prop("files");
  const file = files && files[0];
  if (!file) {
    alertText("Choose a file first.");
    return;
  }
  const chunkRaw = $("#chunk_size").val();
  if (!L.validateChunkSize(chunkRaw)) {
    alertText("Packet size must be a whole number from " + L.CHUNK_MIN + " to " + L.CHUNK_MAX + ".");
    return;
  }
  const chunkSize = Number(chunkRaw);
  const waitTime = Number($("#wait_time").val());
  if (!(isFinite(waitTime) && waitTime >= 10)) {
    alertText("Wait time must be at least 10 ms.");
    return;
  }

  state.sending = true;
  state.cancelSend = false;
  refresh_locks();

  const reader = new FileReader();
  reader.onerror = function () {
    end_send();
    alertText("Could not read the file.");
  };
  reader.onload = function () {
    const dataURL = String(reader.result || "");
    const chunks = L.chunkString(dataURL, chunkSize);
    const total = chunks.length;
    const sha = L.sha256Hex(dataURL);
    const sizeText = L.get_string_size(dataURL);
    const time = L.formatDuration(L.estimateSeconds(total, waitTime));
    const enable = L.buildEnableFileTx(
      { total_packets: total, size: sizeText, name: file.name, time: time },
      state.username
    );
    if (!enable) {
      end_send();
      alertText("Could not fit the file details into one radio packet.");
      return;
    }

    render_kv("#rec_info", [
      ["Sending", file.name],
      ["Total size", sizeText],
      ["Chunks", String(total)],
      ["Estimated min time", time],
    ]);
    $("#rec_verify").text("").removeClass("text-success text-danger");
    $("#chunk_ratio").text("(0 / " + total + ") chunks sent");
    set_progress(0, "bg-info");

    const sender = L.createChunkSender({
      chunks: chunks,
      waitMs: waitTime,
      retryDelayMs: RETRY_DELAY_MS,
      maxRetries: L.CHUNK_MAX_RETRIES,
      schedule: function (fn, ms) {
        setTimeout(fn, ms);
      },
      isCancelled: function () {
        return state.cancelSend;
      },
      send: function (chunk, _i, done) {
        lora_tx("/send_raw", chunk, done);
      },
      onProgress: function (sent, tot) {
        $("#chunk_ratio").text("(" + sent + " / " + tot + ") chunks sent");
        set_progress((sent / tot) * 100);
      },
      onFinish: function (res) {
        finish_send(res, sha);
      },
    });

    lora_tx_retry("/lora_transmit", enable, L.CHUNK_MAX_RETRIES, function (r) {
      if (!r.ok) {
        end_send();
        alertText("Could not start the transfer: " + r.reason + ".");
        return;
      }
      // Give the receiving browser time to process enable_file_tx_mode (WS)
      // before the first chunk arrives on SSE.
      setTimeout(sender.start, RETRY_DELAY_MS);
    });
  };
  reader.readAsDataURL(file);
}

function finish_send(res, sha) {
  // Always tell the receiver we're done, with what it should have received.
  lora_tx_retry(
    "/lora_transmit",
    L.buildDisableFileTx(res.total, sha),
    L.CHUNK_MAX_RETRIES,
    function (r) {
      end_send();
      let msg;
      if (res.ok) {
        msg = "File sent: " + res.total + " chunks.";
        if (!r.ok)
          msg += "\nWarning: the end-of-transfer message failed (" + r.reason +
            "), so the receiver can't verify the file.";
        set_progress(100, "bg-success");
      } else {
        msg = "Transfer aborted after " + res.sent + " of " + res.total + " chunks: " + res.reason + ".";
        set_progress((res.sent / Math.max(1, res.total)) * 100, "bg-danger");
      }
      $("#rec_verify").text(msg).toggleClass("text-success", res.ok).toggleClass("text-danger", !res.ok);
      alertText(msg);
    }
  );
}

function end_send() {
  state.sending = false;
  state.cancelSend = false;
  refresh_locks();
}

function stop_broadcast() {
  if (!state.sending) return;
  state.cancelSend = true;
  $("#chunk_ratio").text("Stopping after the current chunk...");
}

function on_file_rx_start(msg) {
  if (state.sending || state.sensorMode) {
    console.log("Ignoring an incoming file while sending one or in sensor mode.");
    return;
  }
  if (msg.totalPackets < 0) return;
  receiver.start({
    totalPackets: msg.totalPackets,
    fileName: msg.fileName,
    size: msg.size,
    time: msg.time,
    sender: msg.sender,
  });
  state.lastRawMillis = null;
  render_kv("#rec_info", [
    ["Receiving from", msg.sender],
    ["File name", msg.fileName],
    ["Total size", msg.size],
    ["Chunks", String(msg.totalPackets)],
    ["Estimated min time", msg.time],
  ]);
  $("#rec_verify").text("").removeClass("text-success text-danger");
  $("#chunk_ratio").text("(0 / " + msg.totalPackets + ") 0 Bytes received");
  set_progress(0, "bg-success");
  refresh_locks();
}

function on_file_rx_end(msg) {
  if (!receiver.isActive()) return;
  const r = receiver.finish({ totalPackets: msg.totalPackets, sha256: msg.sha256 });
  if (r.verified) {
    $("#rec_verify")
      .text("Verified: " + r.received + " packets, SHA-256 matches. Ready to download.")
      .removeClass("text-danger")
      .addClass("text-success");
  } else {
    $("#rec_verify")
      .text("Corrupted: " + r.reason + ". Download is disabled.")
      .removeClass("text-success")
      .addClass("text-danger");
    set_progress(receiver.progress().percent, "bg-danger");
  }
  refresh_locks();
}

function cancel_receive() {
  if (!receiver.cancel()) return;
  $("#rec_verify").text("Receiving cancelled.").removeClass("text-success").addClass("text-danger");
  refresh_locks();
}

function downloadFile() {
  if (receiver.isActive()) {
    alertText("A file transfer is in progress.");
    return;
  }
  if (!receiver.canDownload()) {
    alertText("There is no verified file to download.");
    return;
  }
  let decoded;
  try {
    decoded = L.dataURIToBytes(receiver.data());
  } catch (e) {
    alertText("The received data isn't a valid file.");
    return;
  }
  const link = document.createElement("a");
  link.href = URL.createObjectURL(new Blob([decoded.bytes], { type: decoded.mime }));
  link.download = receiver.fileName() || "Voyager_Download";
  link.click();
  setTimeout(function () {
    URL.revokeObjectURL(link.href);
  }, URL_REVOKE_DELAY_MS);
}
