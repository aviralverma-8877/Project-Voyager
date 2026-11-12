#include "web_sockets.h"
#include "auth.h"
#include "web_validate.h"

#define WS_RX_QUEUE_LEN    8
#define WS_OUT_QUEUE_LEN   32
#define WS_ENQUEUE_WAIT_MS 20
#define WS_CLEANUP_MS      1000

volatile uint32_t ws_client_count = 0;
static volatile uint32_t sse_client_count = 0;
AsyncWebSocket webSocket("/ws");
AsyncEventSource rawEvents("/rawEvents");

static QueueHandle_t ws_rx_queue = NULL;   // char* (malloc'd, NUL-terminated), worker frees
static QueueHandle_t ws_out_queue = NULL;  // OutMsg*, sender task deletes

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
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, msg);
    free(msg);
    if (err || !doc.is<JsonObject>()) {
      serial_print("WS: invalid JSON ignored");
      continue;
    }
    const char* type = doc["request-type"] | "";
    char safe[33];
    wv::sanitize_for_log(type, safe, sizeof(safe));
    serial_print(String("WS request-type: ") + (safe[0] ? safe : "(missing)"));
    handle_operations(doc);   // core: ignores and logs missing/unknown types
  }
}

// Called from the AsyncTCP task when a WS frame arrives. Copies valid messages to the
// RX queue for the worker task. Rejects fragmented, binary, or oversized messages.
static void ws_handle_data(AsyncWebSocketClient* client, void* arg, uint8_t* data, size_t len) {
  AwsFrameInfo* info = (AwsFrameInfo*)arg;
  // info->num is only written by the library for partial frames; for a complete
  // frame it is whatever the allocation held, so it must not be trusted here.
  // A continuation frame (the only case where num matters) has opcode 0 and is
  // rejected by the opcode check anyway.
  const uint32_t frame_num = (info != NULL && info->opcode == 0) ? info->num : 0;
  if (info == NULL ||
      !wv::ws_frame_acceptable(info->final, info->index, frame_num, info->len, len, info->opcode)) {
    // Fragmented, binary, partial or > 2 KB: drop. Log once per message, not per chunk.
    if (info == NULL || info->index == 0)
      serial_print("WS: rejected frame from #" + String(client->id()));
    if (info && info->index == 0 && info->len > wv::WS_MAX_MESSAGE)
      client->close(1009);   // message too big
    return;
  }
  char* copy = (char*)malloc(len + 1);
  if (copy == NULL) {
    serial_print("WS: out of memory, message dropped");
    return;
  }
  memcpy(copy, data, len);
  copy[len] = '\0';
  if (ws_rx_queue == NULL || xQueueSend(ws_rx_queue, &copy, 0) != pdTRUE) {
    free(copy);
    serial_print("WS: busy, message dropped");
  }
}

void onEvent(AsyncWebSocket* server, AsyncWebSocketClient* client, AwsEventType type,
             void* arg, uint8_t* data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      ws_client_count++;
      serial_print("WS client #" + String(client->id()) + " connected (" + String(ws_client_count) + ")");
      break;
    case WS_EVT_DISCONNECT:
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
  OutMsg* m = new (std::nothrow) OutMsg();
  if (m == NULL) return;
  m->to_ws = to_ws;
  m->data = data;
  if (!to_ws) m->topic = topic;
  if (xQueueSend(ws_out_queue, &m, pdMS_TO_TICKS(WS_ENQUEUE_WAIT_MS)) != pdTRUE) delete m;
}

void send_to_ws(const String& message) { enqueue_out(true, message, String()); }

void send_to_events(const String& data, const String& topic) { enqueue_out(false, data, topic); }

// Sender task: drains the outbound queue and periodically cleans up stale WS clients.
// No serial_print here - DEBUG events are produced from serial_print and would recurse.
static void ws_sender_task(void* param) {
  TickType_t last_cleanup = xTaskGetTickCount();
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
