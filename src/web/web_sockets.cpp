#include "web_sockets.h"
#include "auth.h"
#include "web_validate.h"
#include <atomic>

#define WS_RX_QUEUE_LEN    8
#define WS_OUT_QUEUE_LEN   32
#define WS_ENQUEUE_WAIT_MS 50    // WS replies only; SSE items never wait
#define WS_OUT_RESERVED    8     // ws_out_queue slots kept free for WS replies
#define WS_DROP_REPORT_MS  1000
#define WS_CLEANUP_MS      1000

volatile uint32_t ws_client_count = 0;
static volatile uint32_t sse_client_count = 0;
AsyncWebSocket webSocket("/ws");
AsyncEventSource rawEvents("/rawEvents");

static QueueHandle_t ws_rx_queue = NULL;   // char* (malloc'd, NUL-terminated), worker frees
static QueueHandle_t ws_out_queue = NULL;  // OutMsg*, sender task deletes
// Outbound messages dropped because ws_out_queue was full (or out of memory).
// Reported by the sender task through ets_printf (UART only): serial_print would
// produce a DEBUG event and recurse into enqueue_out.
static std::atomic<uint32_t> out_dropped_ws{0};
static std::atomic<uint32_t> out_dropped_sse{0};

struct OutMsg {
  bool to_ws;
  String data;
  String topic;
};

uint32_t events_client_count() { return sse_client_count; }

/* ---------------- handshake gate: auth (cookie or Digest) + Origin ---------------- */

enum GateResult : uint8_t { GATE_OK, GATE_SETUP, GATE_ORIGIN, GATE_UNAUTH };

static GateResult gate_check(AsyncWebServerRequest* r) {
  const AsyncWebHeader* o = r->getHeader("Origin");
  if (!wv::origin_allowed(o ? o->value().c_str() : nullptr, r->host().c_str())) return GATE_ORIGIN;
  if (!auth_is_set()) return GATE_SETUP;
  if (!auth_check(r)) return GATE_UNAUTH;
  return GATE_OK;
}

// Auth gate for /ws and /rawEvents. Registered before the library handlers. It claims
// only handshakes that must be refused (origin mismatch, no password set, bad creds),
// letting allowed ones fall through to the WebSocket/EventSource handlers.
class StreamGate : public AsyncWebHandler {
 public:
  bool canHandle(AsyncWebServerRequest* r) override {
    if (r->method() != HTTP_GET) return false;
    const String& u = r->url();
    if (!(u == "/ws" || u == "/rawEvents")) return false;
    if (gate_check(r) == GATE_OK) return false;
    r->addInterestingHeader("ANY");
    return true;
  }
  void handleRequest(AsyncWebServerRequest* r) override {
    switch (gate_check(r)) {
      case GATE_ORIGIN:
        r->send(403, "application/json", "{\"error\":\"origin\"}");
        break;
      case GATE_SETUP:
        r->send(403, "application/json", "{\"error\":\"setup_required\"}");
        break;
      case GATE_UNAUTH:
        r->requestAuthentication(AUTH_REALM, true);
        break;
      default:
        r->send(500);   // state changed between canHandle and here; client retries
        break;
    }
  }
};
static StreamGate stream_gate;

/* ---------------- inbound: AsyncTCP callback -> worker task ---------------- */

// Worker task: parses WS messages off the RX queue and dispatches to handle_operations
// (system-core-engineer). Runs on a separate task so the AsyncTCP callback never blocks.
static void ws_rx_worker(void* param) {
  for (;;) {
    char* msg = NULL;
    if (xQueueReceive(ws_rx_queue, &msg, portMAX_DELAY) != pdTRUE || msg == NULL) continue;
    if (web_fs_offline()) {   // handle_operations reads/writes /config: not during FS OTA
      free(msg);
      serial_print("WS: filesystem update in progress, request dropped");
      continue;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, msg);
    free(msg);
    if (err || !doc.is<JsonObject>()) {
      serial_print("WS: invalid JSON ignored");
      continue;
    }
    const char* type = doc["request-type"] | "";
    if (type[0]) {   // missing/non-string types are logged by handle_operations
      char safe[33];
      wv::sanitize_for_log(type, safe, sizeof(safe));
      serial_print(String("WS request-type: ") + safe);
    }
    handle_operations(doc);   // core: ignores and logs missing/unknown types
  }
}

// Per-client reassembly of one text frame that arrives in several TCP segments.
// Stored in client->_tempObject (owned here; the library never touches it). Freed
// when the frame completes, when a new frame starts, and on WS_EVT_DISCONNECT, which
// the library raises exactly once, from the client's destructor.
struct WsAssembly {
  size_t total;
  size_t received;
  char buf[1];   // total + 1 bytes allocated
};

static void ws_assembly_free(AsyncWebSocketClient* client) {
  if (client->_tempObject != NULL) {
    free(client->_tempObject);
    client->_tempObject = NULL;
  }
}

// Hands a complete message (malloc'd, NUL-terminated) to the worker task.
static void ws_deliver(char* msg) {
  if (ws_rx_queue == NULL || xQueueSend(ws_rx_queue, &msg, 0) != pdTRUE) {
    free(msg);
    serial_print("WS: busy, message dropped");
  }
}

// Called from the AsyncTCP task for each chunk of a WS frame. Accepts only complete
// single-frame text messages of at most 2 KB (Contract 3), reassembling a frame that
// spans TCP segments, and queues each one for the worker task. AwsFrameInfo::num is
// deliberately not read (uninitialised in the library, see web_validate.h).
static void ws_handle_data(AsyncWebSocketClient* client, void* arg, uint8_t* data, size_t len) {
  AwsFrameInfo* info = (AwsFrameInfo*)arg;
  if (info == NULL) return;

  if (info->index == 0) {          // first chunk of a new frame
    ws_assembly_free(client);      // an unfinished earlier frame is abandoned
    if (!wv::ws_frame_start_ok(info->final, info->opcode, info->len)) {
      serial_print("WS: rejected frame from #" + String(client->id()));   // once per frame
      if (info->len > wv::WS_MAX_MESSAGE) client->close(1009);             // message too big
      return;
    }
    if (len == info->len) {        // whole frame in one chunk: the common case
      char* copy = (char*)malloc(len + 1);
      if (copy == NULL) {
        serial_print("WS: out of memory, message dropped");
        return;
      }
      memcpy(copy, data, len);
      copy[len] = '\0';
      ws_deliver(copy);
      return;
    }
    WsAssembly* a = (WsAssembly*)malloc(sizeof(WsAssembly) + (size_t)info->len);
    if (a == NULL) {
      serial_print("WS: out of memory, message dropped");
      return;
    }
    a->total = (size_t)info->len;
    a->received = 0;
    client->_tempObject = a;
  }

  // Chunk of a frame being reassembled (rejected frames have no assembly: ignored).
  WsAssembly* a = (WsAssembly*)client->_tempObject;
  if (a == NULL) return;
  if (!wv::ws_chunk_in_order(a->received, a->total, info->index, info->len, len)) {
    ws_assembly_free(client);
    serial_print("WS: out-of-order chunk from #" + String(client->id()) + ", message dropped");
    return;
  }
  memcpy(a->buf + a->received, data, len);
  a->received += len;
  if (a->received < a->total) return;
  // Complete: reuse the block as a plain NUL-terminated string (it is at least
  // total + 1 bytes long), so the worker's free() releases it.
  const size_t total = a->total;
  client->_tempObject = NULL;
  char* msg = (char*)a;
  memmove(msg, a->buf, total);
  msg[total] = '\0';
  ws_deliver(msg);
}

void onEvent(AsyncWebSocket* server, AsyncWebSocketClient* client, AwsEventType type,
             void* arg, uint8_t* data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      ws_client_count++;
      serial_print("WS client #" + String(client->id()) + " connected (" + String(ws_client_count) + ")");
      break;
    case WS_EVT_DISCONNECT:
      ws_assembly_free(client);
      if (ws_client_count > 0) ws_client_count--;
      serial_print("WS client #" + String(client->id()) + " disconnected (" + String(ws_client_count) + ")");
      break;
    case WS_EVT_DATA:
      ws_handle_data(client, arg, data, len);
      break;
    case WS_EVT_ERROR:
      serial_print("WS error from client #" + String(client->id()));
      break;
    case WS_EVT_PONG:
    default:
      break;
  }
}

void onRawEvents(AsyncEventSourceClient* client) {
  sse_client_count++;
  if (client->lastId())
    serial_print("SSE client reconnected, last id " + String(client->lastId()));
}

static void onRawEventsDisconnect(AsyncEventSource* source, AsyncEventSourceClient* client) {
  if (sse_client_count > 0) sse_client_count--;
}

/* ---------------- outbound: one sender task ---------------- */

// Enqueues a copy for the sender task. The early client-count check is an optimization
// to avoid allocation when nobody is listening. A client may disconnect between this
// check and the actual send - the sender task rechecks before sending, and the
// AsyncWebSocket/EventSource APIs handle vanished clients gracefully. No memory leaks.
static void enqueue_out(bool to_ws, const String& data, const String& topic) {
  if (ws_out_queue == NULL) return;
  if (to_ws ? ws_client_count == 0 : sse_client_count == 0) return;
  std::atomic<uint32_t>& dropped = to_ws ? out_dropped_ws : out_dropped_sse;
  // SSE (telemetry/DEBUG) never waits and leaves WS_OUT_RESERVED slots for WS replies.
  if (!to_ws &&
      !wv::out_queue_admit(false, (size_t)uxQueueSpacesAvailable(ws_out_queue), WS_OUT_RESERVED)) {
    dropped++;
    return;
  }
  OutMsg* m = new (std::nothrow) OutMsg();
  if (m == NULL) {
    dropped++;
    return;
  }
  m->to_ws = to_ws;
  m->data = data;
  if (!to_ws) m->topic = topic;
  if (xQueueSend(ws_out_queue, &m, to_ws ? pdMS_TO_TICKS(WS_ENQUEUE_WAIT_MS) : 0) != pdTRUE) {
    delete m;
    dropped++;
  }
}

void send_to_ws(const String& message) { enqueue_out(true, message, String()); }

void send_to_events(const String& data, const String& topic) { enqueue_out(false, data, topic); }

// Sender task: drains the outbound queue and periodically cleans up stale WS clients.
// No serial_print here - DEBUG events are produced from serial_print and would recurse.
static void ws_sender_task(void* param) {
  TickType_t last_cleanup = xTaskGetTickCount();
  TickType_t last_report = last_cleanup;
  uint32_t reported_ws = 0, reported_sse = 0;
  for (;;) {
    OutMsg* m = NULL;
    if (xQueueReceive(ws_out_queue, &m, pdMS_TO_TICKS(WS_CLEANUP_MS)) == pdTRUE && m != NULL) {
      // Recheck client count: a client may have disconnected since enqueue.
      if (m->to_ws) {
        if (ws_client_count > 0) webSocket.textAll(m->data);
      } else if (sse_client_count > 0) {
        rawEvents.send(m->data.c_str(), m->topic.c_str());
      }
      delete m;
    }
    if (xTaskGetTickCount() - last_cleanup >= pdMS_TO_TICKS(WS_CLEANUP_MS)) {
      webSocket.cleanupClients();
      last_cleanup = xTaskGetTickCount();
    }
    if (xTaskGetTickCount() - last_report >= pdMS_TO_TICKS(WS_DROP_REPORT_MS)) {
      last_report = xTaskGetTickCount();
      const uint32_t dw = out_dropped_ws.load(), ds = out_dropped_sse.load();
      if (dw != reported_ws || ds != reported_sse) {
        ets_printf("WS: outbound queue full, dropped %u WS / %u SSE messages (total)\n",
                   (unsigned)dw, (unsigned)ds);
        reported_ws = dw;
        reported_sse = ds;
      }
    }
  }
}

void initWebSocket() {
  serial_print("Initializing WebSocket/SSE");
  if (ws_rx_queue == NULL) ws_rx_queue = xQueueCreate(WS_RX_QUEUE_LEN, sizeof(char*));
  if (ws_out_queue == NULL) ws_out_queue = xQueueCreate(WS_OUT_QUEUE_LEN, sizeof(OutMsg*));
  if (ws_rx_queue == NULL || ws_out_queue == NULL) {
    serial_print("WS: queue creation failed, WebSocket/SSE disabled");
    return;
  }
  if (xTaskCreatePinnedToCore(ws_rx_worker, "ws_rx", 8192, NULL, 1, NULL, 1) != pdPASS ||
      xTaskCreatePinnedToCore(ws_sender_task, "ws_tx", 4096, NULL, 1, NULL, 1) != pdPASS) {
    serial_print("WS: task creation failed, WebSocket/SSE disabled");
    return;
  }
  webSocket.onEvent(onEvent);
  rawEvents.onConnect(onRawEvents);
  rawEvents.onDisconnect(onRawEventsDisconnect);
  server.addHandler(&stream_gate);
  server.addHandler(&rawEvents);
  server.addHandler(&webSocket);
}
