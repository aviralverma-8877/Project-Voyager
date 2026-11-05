#include "web_server.h"
#include "web_sockets.h"
#include "auth.h"
#include "web_validate.h"
#include <lora/lora_support.h>
#include <wifi/wifi_support.h>
#include <atomic>
#include <new>

AsyncWebServer server(80);
QueueHandle_t serial_packet_send;
QueueHandle_t serial_packet_rec;
QueueHandle_t send_packets;
QueueHandle_t recv_packets;
QueueHandle_t debug_msg;

#define API_MAX_BODY 2048   // every non-OTA request body; url-encoded 251 B payload fits easily

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static void send_error(AsyncWebServerRequest* r, int code, const char* err) {
  String body = String("{\"error\":\"") + err + "\"}";
  r->send(code, "application/json", body);
}

static bool origin_ok(AsyncWebServerRequest* r) {
  const AsyncWebHeader* o = r->getHeader("Origin");
  return wv::origin_allowed(o ? o->value().c_str() : nullptr, r->host().c_str());
}

// Origin check for state-changing requests, then auth. Replies itself on failure.
static bool guard(AsyncWebServerRequest* r, bool state_changing) {
  if (state_changing && !origin_ok(r)) {
    send_error(r, 403, "origin");
    return false;
  }
  return auth_require(r);
}

static bool is_ota_url(const String& u) { return u == "/update_flash" || u == "/update_littlefs"; }

// Set when a LittleFS image starts being written; stays set until the reboot that
// follows (or is cleared if the upload fails before the partition was touched).
// While set, nothing served over HTTP or WS may touch LittleFS: the partition is being
// overwritten under the mounted filesystem, and ota_finish_task unmounts/remounts it.
// A stat() racing LittleFS.end() crashed the board (HIL: LoadProhibited in
// lfs_bd_read from onNotFound's LittleFS.exists right after "OTA success").
static std::atomic<bool> g_fs_offline{false};

bool web_fs_offline() { return g_fs_offline.load(); }

// First handler in the chain: blocks /config/** and oversized bodies before any
// parsing or later handlers run. Must be registered first via server.addHandler.
class PreGate : public AsyncWebHandler {
 public:
  // The library has no chunked request-body support: it would see Content-Length 0,
  // skip the body and run the handler without parameters (HIL: chunked 3 KB POST ->
  // 400 missing_data instead of 413). Refuse it up front. canHandle runs before the
  // library drops headers that no handler asked for, so the header is visible here.
  static bool is_chunked(AsyncWebServerRequest* r) { return r->hasHeader("Transfer-Encoding"); }

  bool canHandle(AsyncWebServerRequest* r) override {
    if (wv::path_is_blocked(r->url().c_str())) return true;
    if (is_chunked(r)) {
      r->addInterestingHeader("Transfer-Encoding");   // keep it for handleRequest
      return true;
    }
    if (g_fs_offline.load() && !wv::allowed_while_fs_offline(r->url().c_str())) return true;
    return r->contentLength() > API_MAX_BODY && !is_ota_url(r->url());
  }
  void handleRequest(AsyncWebServerRequest* r) override {
    if (wv::path_is_blocked(r->url().c_str())) send_error(r, 403, "forbidden");
    else if (is_chunked(r)) send_error(r, 411, "length_required");
    else if (g_fs_offline.load() && !wv::allowed_while_fs_offline(r->url().c_str()))
      send_error(r, 503, "fs_updating");
    else send_error(r, 413, "too_large");
  }
};
static PreGate pre_gate;

// While no admin password is set, page loads ("/", "*.html", "/update") redirect to
// the built-in /setup page. It lives in firmware, so it works even when LittleFS
// holds an older web UI without its own setup screen. API calls are untouched and
// still get 403 {"error":"setup_required"}.
class SetupGate : public AsyncWebHandler {
 public:
  bool canHandle(AsyncWebServerRequest* r) override {
    return r->method() == HTTP_GET && wv::is_page_request(r->url().c_str()) && !auth_is_set();
  }
  void handleRequest(AsyncWebServerRequest* r) override { r->redirect("/setup"); }
};
static SetupGate setup_gate;

static const char SETUP_HTML[] PROGMEM =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Voyager Setup</title><style>"
    "body{font-family:sans-serif;background:#f4f4f4;margin:0;padding:16px}"
    ".c{max-width:24rem;margin:3rem auto;background:#fff;padding:24px;border-radius:8px;"
    "box-shadow:0 1px 4px rgba(0,0,0,.15)}"
    "h1{font-size:1.4em;margin:0 0 8px}p{color:#555;font-size:.9em}"
    "input,button{width:100%;box-sizing:border-box;padding:10px;margin:6px 0;font-size:1em}"
    "button{background:#0d6efd;color:#fff;border:0;border-radius:4px;cursor:pointer}"
    "button:disabled{opacity:.6}#e{color:#c00;min-height:1.2em;font-size:.9em}"
    "</style></head><body><div class='c'><h1>Set admin password</h1>"
    "<p>This device has no admin password yet. Choose one (8-63 characters). "
    "You will log in as <b>admin</b>.</p>"
    "<form id='f'><input id='p1' type='password' placeholder='Password' minlength='8' "
    "maxlength='63' autocomplete='new-password' required>"
    "<input id='p2' type='password' placeholder='Confirm password' minlength='8' "
    "maxlength='63' autocomplete='new-password' required>"
    "<div id='e'></div><button id='b' type='submit'>Save</button></form></div><script>"
    "var f=document.getElementById('f'),e=document.getElementById('e'),b=document.getElementById('b');"
    "f.onsubmit=function(ev){ev.preventDefault();"
    "var a=document.getElementById('p1').value,c=document.getElementById('p2').value;"
    "if(a.length<8||a.length>63){e.textContent='Password must be 8-63 characters.';return;}"
    "if(a!==c){e.textContent='Passwords do not match.';return;}"
    "e.textContent='';b.disabled=true;"
    "fetch('/api/auth/setup',{method:'POST',body:new URLSearchParams({password:a})})"
    ".then(function(r){if(r.ok||r.status===409){location.href='/';return;}"
    "return r.json().then(function(j){e.textContent='Failed: '+(j.error||r.status);});})"
    ".catch(function(){e.textContent='Device not reachable.';})"
    ".then(function(){b.disabled=false;});};"
    "</script></body></html>";

/* ------------------------------------------------------------------ */
/* LoRa TX (Contract 2: 202 {id}, result later as WS tx_result)        */
/* ------------------------------------------------------------------ */

// Atomic TX id counter. Uses CAS loop because tx_id_after skips 0 (reserved for
// device-originated packets like SOS).
static std::atomic<uint32_t> g_tx_id{0};

static uint32_t next_tx_id() {
  uint32_t cur = g_tx_id.load();
  uint32_t nxt;
  do {
    nxt = wv::tx_id_after(cur);
  } while (!g_tx_id.compare_exchange_weak(cur, nxt));
  return nxt;
}

static void enqueue_tx(AsyncWebServerRequest* r, int type, String& message) {
  QueueParam* q = new (std::nothrow) QueueParam();
  if (q == nullptr) {
    send_error(r, 503, "no_memory");
    return;
  }
  const uint32_t id = next_tx_id();
  q->type = type;
  q->message = message;
  q->id = id;
  // The TX task owns q as soon as it is queued: never touch it afterwards.
  if (send_packets == NULL || xQueueSend(send_packets, &q, 0) != pdTRUE) {
    delete q;
    send_error(r, 503, "queue_full");
    return;
  }
  // Contract 2: 202 {"id":N,"queued":Q}. Q (informational) = packets waiting in the
  // TX queue right after this one was added, including it unless the TX task has
  // already taken it.
  char body[48];
  wv::tx_accepted_body(id, (uint32_t)uxQueueMessagesWaiting(send_packets), body, sizeof(body));
  r->send(202, "application/json", body);
}

static const char* tx_error_name(wv::TxCheck c) {
  switch (c) {
    case wv::TX_MISSING:   return "missing_data";
    case wv::TX_BAD_JSON:  return "bad_json";
    case wv::TX_TOO_LARGE: return "too_large";
    default:               return "error";
  }
}

static void handle_lora_transmit(AsyncWebServerRequest* r) {
  if (!guard(r, true)) return;
  const AsyncWebParameter* p = r->getParam("data", true);
  String name = get_username();
  String mac = WiFi.macAddress();
  wv::TxCheck c = wv::check_lora_transmit(p ? p->value().c_str() : nullptr, name.c_str(),
                                          mac.c_str(), LORA_MAX_PAYLOAD);
  if (c != wv::TX_OK) {
    send_error(r, wv::tx_check_status(c), tx_error_name(c));
    return;
  }
  String payload;
  {
    JsonDocument doc;
    wv::fill_lora_msg(doc, name.c_str(), mac.c_str(), p->value().c_str());
    serializeJson(doc, payload);
  }
  enqueue_tx(r, LORA_MSG, payload);
}

static void handle_send_raw(AsyncWebServerRequest* r) {
  if (!guard(r, true)) return;
  const AsyncWebParameter* p = r->getParam("data", true);
  wv::TxCheck c = wv::check_send_raw(p ? p->value().c_str() : nullptr, LORA_MAX_PAYLOAD);
  if (c != wv::TX_OK) {
    send_error(r, wv::tx_check_status(c), tx_error_name(c));
    return;
  }
  String data = p->value();
  enqueue_tx(r, RAW_DATA, data);
}

/* ------------------------------------------------------------------ */
/* auth endpoints                                                      */
/* ------------------------------------------------------------------ */

static void send_session_cookie(AsyncWebServerRequest* r) {
  char tok[33];
  if (!auth_session_create(tok)) {
    send_error(r, 403, "setup_required");
    return;
  }
  AsyncWebServerResponse* resp = r->beginResponse(200, "application/json", "{\"ok\":true}");
  resp->addHeader("Set-Cookie", String(AUTH_SESSION_COOKIE "=") + tok +
                                    "; Path=/; HttpOnly; SameSite=Strict");
  resp->addHeader("Cache-Control", "no-store");
  r->send(resp);
}

static void define_auth_routes() {
  server.on("/api/auth/status", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "application/json", auth_is_set() ? "{\"set\":true}" : "{\"set\":false}");
  });

  server.on("/api/auth/setup", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (!origin_ok(r)) return send_error(r, 403, "origin");
    if (auth_is_set()) return send_error(r, 409, "already_set");
    const AsyncWebParameter* p = r->getParam("password", true);
    if (p == nullptr || !wv::password_valid(p->value().c_str()))
      return send_error(r, 400, "password_length");
    if (!auth_set(p->value().c_str())) return send_error(r, 500, "nvs_write");
    serial_print("Admin password set");
    r->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/auth/change", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (!guard(r, true)) return;
    const AsyncWebParameter* p = r->getParam("password", true);
    if (p == nullptr || !wv::password_valid(p->value().c_str()))
      return send_error(r, 400, "password_length");
    if (!auth_set(p->value().c_str())) return send_error(r, 500, "nvs_write");
    serial_print("Admin password changed");
    r->send(200, "application/json", "{\"ok\":true}");
  });

  // Digest (or a still-valid cookie) -> fresh vsid cookie for WS/SSE and later requests.
  server.on("/api/session", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!guard(r, false)) return;
    send_session_cookie(r);
  });
  server.on("/api/session", HTTP_DELETE, [](AsyncWebServerRequest* r) {
    if (!origin_ok(r)) return send_error(r, 403, "origin");
    const AsyncWebHeader* c = r->getHeader("Cookie");
    char tok[40];
    if (c && wv::cookie_get(c->value().c_str(), AUTH_SESSION_COOKIE, tok, sizeof(tok)))
      auth_session_revoke(tok);
    AsyncWebServerResponse* resp = r->beginResponse(200, "application/json", "{\"ok\":true}");
    resp->addHeader("Set-Cookie", AUTH_SESSION_COOKIE "=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
    r->send(resp);
  });
}

/* ------------------------------------------------------------------ */
/* config read (whitelisted, wifi secrets removed)                     */
/* ------------------------------------------------------------------ */

static void handle_api_config(AsyncWebServerRequest* r) {
  if (!guard(r, false)) return;
  const AsyncWebParameter* p = r->getParam("name");
  if (p == nullptr || !wv::is_allowed_config_name(p->value().c_str()))
    return send_error(r, 400, "bad_name");
  String path = "/config/" + p->value();
  // read_file (core) takes the FS lock and refuses during a filesystem OTA, so this
  // handler never touches LittleFS directly. Config files are small (<= 4 KB).
  String raw;
  if (!read_file(path.c_str(), raw)) return send_error(r, 404, "not_found");
  if (!wv::is_wifi_config_name(p->value().c_str())) {
    AsyncWebServerResponse* resp = r->beginResponse(200, "application/json", raw);
    resp->addHeader("Cache-Control", "no-store");
    r->send(resp);
    return;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, raw);
  raw.clear();
  if (err || !wv::redact_wifi_config(doc)) return send_error(r, 500, "parse_failed");
  String out;
  serializeJson(doc, out);
  AsyncWebServerResponse* resp = r->beginResponse(200, "application/json", out);
  resp->addHeader("Cache-Control", "no-store");
  r->send(resp);
}

/* ------------------------------------------------------------------ */
/* restart / reset                                                     */
/* ------------------------------------------------------------------ */

static void reply_and_spawn(AsyncWebServerRequest* r, TaskFunction_t fn, const char* name,
                            const char* msg) {
  // restart()/reset_device() wait before rebooting, so the reply gets out first.
  if (xTaskCreatePinnedToCore(fn, name, 6000, NULL, 1, NULL, 1) != pdPASS)
    return send_error(r, 500, "task_create");
  r->send(200, "text/plain", msg);
}

/* ------------------------------------------------------------------ */
/* routes                                                              */
/* ------------------------------------------------------------------ */

void define_api(void* param) {
  server.addHandler(&pre_gate);          // must stay first: /config/** 403, body limit
  server.addHandler(&setup_gate);        // page loads -> /setup until a password is set
  initWebSocket();                       // /ws + /rawEvents behind the auth/Origin gate
  define_auth_routes();

  server.on("/setup", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (auth_is_set()) return r->redirect("/");
    AsyncWebServerResponse* resp = r->beginResponse_P(200, "text/html", SETUP_HTML);
    resp->addHeader("Cache-Control", "no-store");
    r->send(resp);
  });

  server.on("/hostname", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "text/plain", hostname);
  });
  server.on("/username", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "text/plain", get_username());
  });
  server.on("/send_raw", HTTP_POST, handle_send_raw);
  server.on("/lora_transmit", HTTP_POST, handle_lora_transmit);
  server.on("/api/config", HTTP_GET, handle_api_config);

  server.on("/restart", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (!guard(r, true)) return;
    serial_print("HTTP /restart");
    reply_and_spawn(r, restart, "restart", "Restarting device");
  });
  server.on("/reset", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (!guard(r, true)) return;
    serial_print("HTTP /reset (factory reset)");
    reply_and_spawn(r, reset_device, "reset_device", "Resetting device");
  });

  firmware_web_updater();

  // Defence in depth: the static handler never serves a blocked path even if a
  // route above is reordered.
  server.serveStatic("/", LittleFS, "/")
      .setDefaultFile("index.html")
      .setFilter([](AsyncWebServerRequest* r) { return !wv::path_is_blocked(r->url().c_str()); });

  server.onNotFound([](AsyncWebServerRequest* r) {
    // A missing asset (/bootstrap.min.css) is a 404, not the HTML shell.
    if (r->method() == HTTP_GET && wv::is_missing_asset(r->url().c_str())) r->send(404);
    else if (r->method() == HTTP_GET && !g_fs_offline.load() && LittleFS.exists("/index.html"))
      r->send(LittleFS, "/index.html");
    else if (r->method() == HTTP_GET) r->redirect("/update");
    else r->send(404);
  });
  server.begin();
  vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* OTA (/update, /update_flash, /update_littlefs)                      */
/* ------------------------------------------------------------------ */

// OTA state machine: only one upload can run at a time. ota_owner tracks the active
// request; ota_radio_held tracks whether we hold lora_write_mutex. Both are only
// touched from the AsyncTCP task (upload/request/disconnect callbacks run there).
// Per-request state lives in _tempObject so the library frees it when the request ends.

enum OtaStatus : uint8_t { OTA_RUNNING = 0, OTA_DONE, OTA_ORIGIN, OTA_SETUP, OTA_UNAUTH, OTA_BUSY, OTA_FAIL };

struct OtaReqState {
  uint8_t status;
  char error[64];
};

static AsyncWebServerRequest* ota_owner = nullptr;
static bool ota_radio_held = false;
// The LittleFS partition has been (partly) overwritten by a failed upload. The
// filesystem stays offline and the settings snapshot is kept for the retry, because
// re-reading the half-written filesystem would snapshot garbage (or crash).
static bool ota_fs_dirty = false;

// LittleFS OTA: user settings (wifi, lora, user_data, lora_serial) are snapshotted
// before the first write and restored after the remount. Only one OTA runs at a time,
// so a single static set is enough. wifi_config also has a boot-time backup as a
// fallback if the snapshot fails or the file was larger than the limit.
static String ota_keep_data[wv::OTA_KEEP_COUNT];
static bool ota_keep_present[wv::OTA_KEEP_COUNT] = {false};

static void ota_keep_clear() {
  for (size_t i = 0; i < wv::OTA_KEEP_COUNT; i++) {
    ota_keep_data[i] = String();   // frees the buffer
    ota_keep_present[i] = false;
  }
}

static void ota_keep_snapshot() {
  ota_keep_clear();
  for (size_t i = 0; i < wv::OTA_KEEP_COUNT; i++) {
    const char* path = wv::OTA_KEEP_FILES[i];
    bool exists = LittleFS.exists(path);
    File f = exists ? LittleFS.open(path, "r") : File();
    size_t size = f ? f.size() : 0;
    if (f && wv::ota_keep_size_ok(true, size)) {
      ota_keep_data[i] = f.readString();
      ota_keep_present[i] = ota_keep_data[i].length() == size;
      if (!ota_keep_present[i]) ota_keep_data[i] = String();
    } else if (exists) {
      serial_print(String("OTA: not keeping ") + path + " (" + String((uint32_t)size) + " B)");
    }
    if (f) f.close();
  }
}

static void ota_release_radio(bool resume_rx) {
  if (!ota_radio_held) return;
  if (resume_rx) LoRa_rxMode();
  xSemaphoreGive(lora_write_mutex);
  ota_radio_held = false;
}

// Client gone, write error, bad image: undo everything the upload started.
static void ota_abort_owner(AsyncWebServerRequest* r) {
  if (ota_owner != r) return;
  Update.abort();
  ota_owner = nullptr;
  if (ota_fs_dirty) {
    serial_print("OTA: filesystem image incomplete; upload littlefs.bin again");
  } else {
    ota_keep_clear();
    g_fs_offline.store(false);
  }
  ota_release_radio(true);
}

static void ota_fail(AsyncWebServerRequest* r, OtaReqState* st, const char* why) {
  st->status = OTA_FAIL;
  strlcpy(st->error, why && why[0] ? why : "update failed", sizeof(st->error));
  ota_abort_owner(r);
}

static void ota_finish_task(void* arg) {
  const int cmd = (int)(intptr_t)arg;
  vTaskDelay(pdMS_TO_TICKS(1000));   // let the 200 reach the browser
  if (cmd == U_SPIFFS) {
    // New filesystem image: remount, then put the user's settings back
    // (snapshotted at index==0). WiFi falls back to the boot-time backup.
    // Hold core's FS lock across end/begin and the restore so no read_file /
    // write_file_atomic runs against an unmounted filesystem. Readers fail fast while
    // web_fs_offline(), so the wait only covers one that was already inside.
    bool locked = false;
    for (int i = 0; i < 3 && !locked; i++) locked = fs_maintenance_begin(5000);
    if (!locked) serial_print("OTA: FS lock busy, remounting anyway");
    LittleFS.end();
    if (!LittleFS.begin(false)) {
      serial_print("OTA: LittleFS remount failed");
    } else {
      for (size_t i = 0; i < wv::OTA_KEEP_COUNT; i++) {
        const char* path = wv::OTA_KEEP_FILES[i];
        const String* src = ota_keep_present[i] ? &ota_keep_data[i] : nullptr;
        if (src == nullptr && strcmp(path, WIFI_CONFIG_PATH) == 0 && wifi_backup.backup_done)
          src = &wifi_backup.backup_config;
        if (src == nullptr) continue;
        serial_print(String("OTA: restore ") + path +
                     (write_file_atomic(path, *src) ? " ok" : " FAILED"));
      }
    }
    if (locked) fs_maintenance_end();
    ota_keep_clear();
  }
  restart(NULL);   // reboots; never returns
  vTaskDelete(NULL);
}

static uint8_t ota_admit(AsyncWebServerRequest* r) {
  if (!origin_ok(r)) return OTA_ORIGIN;
  if (!auth_is_set()) return OTA_SETUP;
  if (!auth_check(r)) return OTA_UNAUTH;
  if (ota_owner != nullptr) return OTA_BUSY;
  // Contract 6: quiet the radio for the whole flash write. Never block AsyncTCP:
  // if a transmission holds the radio, the client gets 503 and retries.
  if (lora_write_mutex == NULL || xSemaphoreTake(lora_write_mutex, 0) != pdTRUE) return OTA_BUSY;
  ota_radio_held = true;
  LoRa.idle();
  return OTA_RUNNING;
}

static void ota_upload(AsyncWebServerRequest* r, const String& filename, size_t index,
                       uint8_t* data, size_t len, bool final, int cmd) {
  OtaReqState* st = (OtaReqState*)r->_tempObject;
  if (index == 0) {
    if (st != nullptr) {      // a second file part in the same request
      if (st->status == OTA_RUNNING) ota_fail(r, st, "only one file per upload");
      return;
    }
    st = (OtaReqState*)calloc(1, sizeof(OtaReqState));
    if (st == nullptr) return;   // request handler answers 500
    r->_tempObject = st;
    st->status = ota_admit(r);
    if (st->status != OTA_RUNNING) return;
    ota_owner = r;
    r->onDisconnect([r]() {
      if (ota_owner == r) {      // client went away mid-upload
        ota_abort_owner(r);
        serial_print("OTA aborted: client disconnected");
      }
    });
    serial_print(String("OTA start: ") + (cmd == U_FLASH ? "firmware" : "filesystem"));
    if (cmd == U_SPIFFS) {
      // Under core's FS lock (short wait: this is the AsyncTCP task), snapshot the
      // settings and take the filesystem offline, so no config read/write is midway
      // when the partition starts being overwritten. After a failed earlier attempt
      // the filesystem is half-overwritten: reuse that attempt's snapshot instead.
      if (!fs_maintenance_begin(200)) {
        st->status = OTA_BUSY;
        ota_owner = nullptr;
        ota_release_radio(true);
        serial_print("OTA: filesystem busy, try again");
        return;
      }
      if (!ota_fs_dirty) ota_keep_snapshot();
      g_fs_offline.store(true);   // from here on, no HTTP/WS path touches LittleFS
      fs_maintenance_end();
    }
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, cmd)) return ota_fail(r, st, Update.errorString());
  }
  if (st == nullptr || st->status != OTA_RUNNING || ota_owner != r) return;
  if (len && cmd == U_SPIFFS) ota_fs_dirty = true;   // set before the write: it erases first
  if (len && Update.write(data, len) != len) return ota_fail(r, st, Update.errorString());
  if (final) {
    if (!Update.end(true)) return ota_fail(r, st, Update.errorString());
    st->status = OTA_DONE;
    ota_owner = nullptr;
    serial_print("OTA success: " + String((uint32_t)(index + len)) + " B");
    // Keep the radio quiet (mutex held) until the reboot.
    if (xTaskCreatePinnedToCore(ota_finish_task, "ota_finish", 6000, (void*)(intptr_t)cmd, 1, NULL, 1) != pdPASS)
      xTaskCreatePinnedToCore(restart, "restart", 4096, NULL, 1, NULL, 1);
  }
}

static const char OTA_OK_HTML[] PROGMEM =
    "Upload successful, rebooting...<br /><a href='/'>Home Page</a>"
    "<script>setTimeout(function(){window.location.href='/';},10000);</script>";

static void ota_request(AsyncWebServerRequest* r) {
  OtaReqState* st = (OtaReqState*)r->_tempObject;
  if (st == nullptr) {              // no file part (or out of memory)
    if (!guard(r, true)) return;
    return send_error(r, 400, "missing_update_file");
  }
  if (ota_owner == r) ota_fail(r, st, "upload incomplete");
  switch (st->status) {
    case OTA_DONE:   r->send_P(200, "text/html", OTA_OK_HTML); break;
    case OTA_ORIGIN: send_error(r, 403, "origin"); break;
    case OTA_SETUP:  send_error(r, 403, "setup_required"); break;
    case OTA_UNAUTH: r->requestAuthentication(AUTH_REALM, true); break;
    case OTA_BUSY:   send_error(r, 503, "busy"); break;
    default:         r->send(500, "text/plain", st->error[0] ? st->error : "update failed"); break;
  }
}

static const char OTA_FORM_HTML[] PROGMEM =
    "<fieldset class=\"lora_transmission\">"
    "<form method='POST' action='/update_flash' enctype='multipart/form-data'>"
    "<div class='ota'>"
    "<label for='firmare_file'><h5>firmware.bin</h5></label>"
    "<input id='firmare_file' type='file' class='input' placeholder='firmware.bin' name='update'>"
    "</div><input class='btn primary mb-2' type='submit' value='Update'></form><hr />"
    "<form method='POST' action='/update_littlefs' enctype='multipart/form-data'>"
    "<div class='ota'>"
    "<label for='littlefs_file'><h5>littlefs.bin</h5></label>"
    "<input id='littlefs_file' type='file' class='input' placeholder='littlefs.bin' name='update'>"
    "</div><input class='btn primary mb-2' type='submit' value='Update'></form></fieldset>";

void firmware_web_updater() {
  server.on("/update", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!guard(r, false)) return;
    r->send_P(200, "text/html", OTA_FORM_HTML);
  });
  server.on("/update_flash", HTTP_POST, ota_request,
            [](AsyncWebServerRequest* r, String filename, size_t index, uint8_t* data, size_t len, bool final) {
              ota_upload(r, filename, index, data, len, final, U_FLASH);
            });
  server.on("/update_littlefs", HTTP_POST, ota_request,
            [](AsyncWebServerRequest* r, String filename, size_t index, uint8_t* data, size_t len, bool final) {
              ota_upload(r, filename, index, data, len, final, U_SPIFFS);
            });
}
