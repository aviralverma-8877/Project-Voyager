#include <Arduino.h>
#include <display/oled_display.h>
#include "tasks.h"
#include <wifi/wifi_support.h>
#include <lora/lora_support.h>
#include "support_method.h"
#include <web/web_server.h>
#include <web/web_sockets.h>
#include "FS.h"
#include "LittleFS.h"

// Contract 6: queue depths (all queues carry pointers).
#define SEND_QUEUE_DEPTH    20
#define RECV_QUEUE_DEPTH    20
#define DEBUG_QUEUE_DEPTH   20
#define SERIAL_QUEUE_DEPTH  20
// How long setup() waits for config_wifi before starting the API anyway
// (STA attempt is ~20 s, then AP fallback).
#define WIFI_WAIT_MS        45000

static void halt(const char* title, const char* text)
{
    Serial.printf("FATAL: %s: %s\n", title, text);
    oled_alert(title, text, 0);
    while (true)
        vTaskDelay(pdMS_TO_TICKS(1000));
}

static QueueHandle_t create_queue(UBaseType_t depth, const char* name)
{
    QueueHandle_t q = xQueueCreate(depth, sizeof(void*));
    if (q == NULL)
        halt("Boot error", name);
    return q;
}

static void create_task(TaskFunction_t fn, const char* name, uint32_t stack,
                        UBaseType_t prio, TaskHandle_t* handle)
{
    if (xTaskCreatePinnedToCore(fn, name, stack, NULL, prio, handle, 1) != pdPASS)
        halt("Boot error", name);
}

void setup() {
  // The LoRa-serial bridge receives whole YMODEM blocks (1029 bytes in ~90 ms at
  // 115200) while the radio may be busy transmitting the previous chunk. With the
  // default 256-byte buffer (+128-byte FIFO) everything past 384 bytes was dropped.
  // Must be set before begin().
  Serial.setRxBufferSize(4096);
  Serial.begin(BAUD);
  init_oled();   // no-op without a panel; never hangs
  oled_status("Booting");

  send_packets       = create_queue(SEND_QUEUE_DEPTH,   "send queue");
  recv_packets       = create_queue(RECV_QUEUE_DEPTH,   "recv queue");
  debug_msg          = create_queue(DEBUG_QUEUE_DEPTH,  "debug queue");
  serial_packet_send = create_queue(SERIAL_QUEUE_DEPTH, "serial tx queue");
  serial_packet_rec  = create_queue(SERIAL_QUEUE_DEPTH, "serial rx queue");
  create_task(debugger_print, "debugger_print", 4096, 1, &debug_handler);

  oled_status("Mounting storage");
  fs_lock_init();
  fs_mounted = LittleFS.begin(FORMAT_LITTLEFS_IF_FAILED);
  if (!fs_mounted)
  {
    // Keep going: WiFi + API come up with defaults so OTA (/update_littlefs) can recover.
    serial_print("LittleFS Mount Failed");
    oled_alert("Storage error", "FS mount failed, OTA to recover", 10000);
  }

  config_gpios();
  get_lora_serial();     // defaults to false without FS
  load_username();       // defaults to MAC without FS
  // Note: device_mode config exists but is reserved for future use (e.g., BLE, LoRa-only).
  // Currently only "WiFi" is supported, so we don't read it at boot.

  // Initialize Bluetooth Serial if enabled in config. Must run after WiFi.macAddress()
  // is available and LittleFS is mounted. Does nothing if bt_enabled is false.
  init_bt_serial();

  // LED, buttons and telemetry first so led_nortifier() has a target before the radio starts.
  setupTasks();

  oled_status("Starting WiFi");
  WiFi.onEvent(onWifiConnect, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.onEvent(onWifiDisconnect, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  create_task(config_wifi, "config_wifi", 6000, 2, NULL);

  oled_status("Starting LoRa");
  config_lora();          // creates lora_write_mutex
  start_serial_bridge();  // needs the radio mutex and the serial queues

  // server.begin() (inside define_api) must run after the network stack is up.
  uint32_t start = millis();
  while (!WiFi_setup_done && (millis() - start) < WIFI_WAIT_MS)
    vTaskDelay(pdMS_TO_TICKS(100));
  if (!WiFi_setup_done)
    serial_print("WiFi setup still pending, starting API anyway");
  create_task(define_api, "define_api", 6000, 2, NULL);

  serial_print("config done");
  oled_status("Ready");   // STA path overwrites this with "WiFi connected" later
}

void loop() {
  vTaskDelete(NULL);   // all work happens in tasks
}
