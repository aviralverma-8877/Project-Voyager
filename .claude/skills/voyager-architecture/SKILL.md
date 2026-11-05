---
name: voyager-architecture
description: Project Voyager component ownership map and the cross-component contracts (LoRa packet format, HTTP endpoints, auth, WebSocket request/response types, SSE topics, OLED API, config files, FreeRTOS queues). Load before developing, reviewing or testing any part of the repo, and whenever a change might cross a component boundary.
---

# Project Voyager architecture and contracts (v2)

ESP32 (Arduino + FreeRTOS, PlatformIO) firmware that moves text messages and files
between devices over LoRa, driven from a web UI served by the device itself.
v2 changes the wire format, so v1 and v2 devices **do not interoperate**.

## Ownership map (every file has exactly one owner)

| Agent | Owns |
|---|---|
| `lora-radio-engineer` | `src/lora_support.cpp/.h` |
| `web-api-engineer` | `src/web_server.cpp/.h`, `src/web_sockets.cpp/.h`, `src/auth.cpp/.h` |
| `wifi-network-engineer` | `src/wifi_support.cpp/.h`; in `src/support_method.cpp`: `setup_mdns`, `setup_dns`, `dns_request_process` |
| `system-core-engineer` | `src/main.cpp`, `src/tasks.cpp/.h`, `src/support_method.cpp/.h` (everything not listed for wifi) |
| `oled-display-engineer` | `src/oled_display.cpp/.h` |
| `web-ui-engineer` | `data_main/**` (source of truth), `data/**` (copy that gets flashed) |
| `build-release-engineer` | `platformio.ini`, `Makefile`, `scripts/**`, `webflash/**` (except README), root `index.html` (web flasher), `test/` harness and `test/web/run.sh`, `include/`, `lib/`, `.trunk/`, `.vscode/`, `.gitignore`, `LICENSE` |
| `docs-maintainer` | `README.md`, `docs/**`, `webflash/README.md`, `test/README`, `include/README`, `lib/README` |

Each component owner owns its tests: `test/test_<component>/` (L2) and `test/web/*` (L3).

Rules:
- Edit only files you own. If a fix needs another owner's file, stop and report the
  exact change needed and which agent owns it. Don't make the edit yourself.
- The contracts below are fixed by the orchestrator. Implement them exactly. If one
  can't work, say so in your report, but don't edit this file.

## Contract 1: LoRa packet v2 (lora_support)

```
[len u8][type u8][seq u8][crc8 u8][payload len bytes]
```
- `LORA_MAX_PAYLOAD` = **251** (255 FIFO bytes minus the 4-byte header). It's defined
  in `lora_support.h` and used by web_server for 413 checks.
- `crc8` = CRC8 over the `seq` byte followed by the payload.
- Types: `0 LORA_MSG`, `1 RAW_DATA` (**retired**: never sent; a received one is dropped
  without an ACK), `2 REC_AKNG`, `3 LORA_SERIAL`.
- `seq`: the sender seeds it randomly at boot (`esp_random`), then adds 1 for each
  **new** LORA_MSG. Retries reuse the same seq. LORA_SERIAL frames are sent with
  seq 0 and don't consume the counter (they're never ACKed or deduplicated).
- LORA_SERIAL framing (core `serial_to_lora`): a frame ends at CR, at 251 bytes, or
  after 10 ms of serial silence, so one Kermit packet = one LoRa frame. Received
  frames are written to serial only while the bridge is on (`lora_serial`).
- `REC_AKNG`: `len=1`, `seq` = the seq of the packet being ACKed, `crc=0`, payload is
  one byte (`1` = OK, `0` = retry). The sender ignores an ACK whose seq doesn't match
  its outstanding packet, and treats any value other than 1 as a NACK.
- Receiver: when a packet's checksum is valid but its `seq` equals the last accepted
  data packet's seq, send ACK 1 and **don't deliver it again**.
- Sender retries up to `LORA_MAX_RETRIES` (3) times, with a `LORA_ACK_TIMEOUT_MS`
  (5000) timeout on each. Transmissions are blocking (`endPacket()`); RX mode comes
  back after TX finishes.
- IQ inversion: **unchanged** until the two-board hardware test confirms it.
- The radio interrupt only signals an RX task. Everything else (reading the FIFO,
  checking the CRC, queueing, ACKing, logging) runs in task context.
- `LORA_MSG` payload JSON: `{"name","mac","data":"<inner JSON string>"}`. The inner
  object is `{"pack_type":"msg","data":text}` or `{"pack_type":"action","data":<action>,...}`:
  - `sos`
  - File Transfer handshake (sent by the UI via `/lora_transmit`):
    `file_tx_invite` + `tx_id`, `name` (display only, max 64), `size`;
    `file_tx_join` + `tx_id`, `nonce`; `file_tx_go` + `tx_id`, `nonce` (the one receiver
    picked). The file itself then travels as Kermit over the LORA_SERIAL bridge.

## Contract 2: HTTP v2 (web_server, auth)

Authentication is HTTP **Digest**: realm `Voyager`, user `admin`. The NVS stores
HA1 = md5("admin:Voyager:<password>") (`Preferences` namespace `voyager`, key `ha1`),
never the password. Passwords must be 8–63 chars.

| Method | Path | Auth | Input | Output |
|---|---|---|---|---|
| GET | `/api/auth/status` | no | – | `{"set":bool}` |
| POST | `/api/auth/setup` | no, but only works while no password is set | `password` | 200 / 400 / 409 |
| POST | `/api/auth/change` | yes | `password` (new) | 200 / 400 |
| POST | `/lora_transmit` | yes | form `data` (inner JSON string) | **202** `{"id":N}`; 400 missing; 413 if the wrapped payload > 251; 503 if the queue is full |
| GET | `/api/config?name=<file>` | yes | allowed files: `lora_config.json`, `long_lora_config.json`, `short_lora_config.json`, `config_list.json`, `lora_serial.json`, `wifi_config.json` | the file's JSON. `wifi_config.json` has `wifi_pass`/`ap_pass` **removed** |
| GET | `/api/session` | yes | – | 200 `{"ok":true}` + `Set-Cookie: vsid=<128-bit random hex>; HttpOnly; SameSite=Strict; Path=/`. The cookie authenticates WS/SSE (and any HTTP route as an alternative to Digest). It lasts until reboot. At most 4 sessions, oldest dropped first |
| GET | `/hostname`, `/username` | no | – | text/plain |
| POST | `/restart`, `/reset` | yes | – | 200, then restart / factory reset |
| GET | `/update` | yes | – | OTA HTML fragment |
| POST | `/update_flash`, `/update_littlefs` | yes (checked when `index==0`) | multipart `update` | 200 on success, 500 + error string on failure |
| GET | `/config/*` | – | – | **403 always** |
| GET | other paths | no | – | static LittleFS (UI shell and assets), falls back to `/index.html` |

- `/send_akn`, `/send_raw` and GET `/restart` / `/reset` are **removed**.
- State-changing requests check that `Origin`/`Host` match the device. A mismatch gets 403.
- The TX result arrives later over WS as `tx_result` (Contract 3).
- Until a password is set, every auth-required endpoint returns 403
  `{"error":"setup_required"}`, and the UI shows a set-password screen.

## Contract 3: WebSocket `/ws` (valid `vsid` cookie or Digest on the handshake, Origin checked)

The UI calls `GET /api/session` once after load, which brings up the browser's Digest prompt. Only then does it open WS and SSE.

Messages are handled on a worker task, never inside the AsyncTCP callback. Only
complete single-frame text messages up to 2 KB are accepted.

Client → device `request-type` (missing or unknown types are ignored and logged):
`wifi_ssid_scan`, `connect_wifi` (`wifi_ssid`, `wifi_pass`, which may be empty for open
networks), `wifi_ap_mode`, `set_ap_pass` (`val`: 8–63 chars, or `""` to go back to the derived default), `reset_device`,
`restart_device`, `set_username` (`val`, 1–32 chars), `get_username`,
`set_lora_config` (`val` = JSON string, range-checked), `set_serial_mode` (`value`),
`get_serial_mode`. **Removed:** `send_akn`.

Device → client `response_type`:
- `wifi_scan` (`SSID:[{ssid,rssi}]`)
- `alert` (`alert_msg`, plain text)
- `lora_rx` (`lora_msg` = raw LORA_MSG payload)
- `tx_result` (`id`, `akn` 0/1, `username`)
- `set_uname` (`uname`)
- `serial_mode` (`value`)

`stop_transmission` is **removed** (it belonged to the retired chunked file transfer).

`set_sync_word` is removed from the UI.

Every outbound WS and SSE message goes through **one** sender task (web_sockets).
`send_to_ws` / `send_to_events` only enqueue a copy, so they're safe to call from any task.

## Contract 4: Server-sent events `/rawEvents` (valid `vsid` cookie or Digest)

| Event | Data | Producer |
|---|---|---|
| `WIFI_DATA` | STA: `{"mode":"STA","rssi","bssid","ch"}`; AP: `{"mode":"AP","clients"}`; else `{"mode":"OFF"}`; every 1 s | tasks |
| `RAM_DATA` | `{"free_heap","heap_size"}` every 1 s | tasks |
| `DEBUG` | `{"millis","data"}` | support_method. Sent only when built with `DEBUG=true` and `events_client_count()>0` (web_sockets.h; never call `rawEvents.count()` from other tasks) |

Nothing logged through `serial_print` may contain a password, `wifi_pass`, `ap_pass`
or the HA1.

## Contract 5: config files (LittleFS `/config/`)

- `wifi_config.json` / `wifi_default.json`:
  `{wifi_function:"AP"|"STA", wifi_ssid, wifi_pass, ap_pass}`.
  - An empty `ap_pass` means the derived default: the 8 uppercase hex characters of
    the lower 4 MAC bytes.
  - The AP always uses WPA2.
- `lora_config.json` holds the active radio config. `lora_default.json` is the factory
  default (same keys). The presets are `long_`/`short_lora_config.json`. Keys:
  `{freq, TxPower, SpreadingFactor, SignalBandwidth, CodingRate4, SyncWord}`.
  - Range checks: freq 410–525 MHz, TxPower 2–20, SF 6–12, BW one of the SX127x
    values, CR 5–8, SyncWord 0–255.
  - Defaults (510 MHz, SyncWord 52) are **unchanged** until the user decides.
- `config_list.json` `[{name,value}]`, `lora_serial.json` `{lora_serial}`,
  `user_data.json` `{username}` (empty means the MAC), `device_config.json` `{device_mode}`.
- **Factory reset** doesn't format the filesystem. It:
  - rewrites `wifi_config.json` from `wifi_default.json`, `lora_config.json` from
    `lora_default.json`, `user_data.json` as `{"username":""}` and `lora_serial.json`
    as `{"lora_serial":false}`
  - calls `auth_clear()`
  - reboots
- Config writes go to `<file>.tmp` first and are then renamed (atomic). Readers treat
  a parse error or missing key as "use the default", never as a crash.

## Contract 6: FreeRTOS objects and shared functions

- Queues carry **pointers**. Whoever receives an item must `delete` it, and if
  `xQueueSend` fails the sender deletes it.
- `send_packets` / `recv_packets` carry `QueueParam*` `{type, message, id}`. There is
  no request pointer any more. `id` is 0 for packets that come from the device itself
  (SOS) and send no `tx_result`.
- `lora_write_mutex` guards **every** SPI access to the radio, including setting config.
- Every task loops forever or calls `vTaskDelete(NULL)` on every exit path.
- Queue creation is checked in `setup()`. A failure shows an OLED alert and halts.
- `serial_print(String)` must not be called from an ISR (it returns immediately if it is).
- A LittleFS OTA image keeps the user's settings. Before writing it, the device backs up `wifi_config.json`, `lora_config.json`, `user_data.json` and `lora_serial.json` in RAM, then restores them atomically after remounting. NVS (auth) isn't affected.
- `led_nortifier()` doesn't block. It notifies the LED task and can be called from any task.
- `auth_clear()`, `auth_is_set()` (auth.h, web-api) are used by core's factory reset.
- `bool write_file_atomic(const char* path, const String& content)` (support_method, core):
  writes `<path>.tmp`, closes it, then renames it. wifi, lora and core use it for every
  config write.
- WiFi API (wifi_support), called by core's `handle_operations`. Each validates its
  input, saves atomically, schedules **one** restart (500 ms later, on its own task) and
  returns false (sending an `alert`) on invalid input:
  `bool wifi_save_sta(const char* ssid, const char* pass)`, `bool wifi_switch_to_ap()`,
  `bool wifi_set_ap_pass(const char* pass)` (`""` stores an empty `ap_pass`, which means the derived default). `String wifi_default_ap_pass()` returns
  the derived default.
- `bool save_lora_config(String json)` (lora_support): validates the ranges, writes
  atomically, and applies the settings under `lora_write_mutex`. It returns false
  (with an `alert`) when the input is invalid, and doesn't change the file in that case.
- OTA (web_server): when `index==0`, take `lora_write_mutex` and call `LoRa_txMode()`
  (radio quiet). Keep holding the mutex. On success the device restarts. On failure,
  call `LoRa_rxMode()` and give the mutex back.

## Contract 7: OLED API (oled_display, thread-safe; callable from any task, not from ISRs)

All drawing happens on one display task. The calls below copy their arguments into a
queue and return straight away. `display_buffer`, `display_text_oled` and
`clear_oled_display` are **removed**.

```cpp
void init_oled();                                    // starts the display task. If there's no panel or no memory: no-op, never hangs
void oled_set_wifi(WiFiStatus s, const char* ssid, const char* ip);
void oled_set_ap(const char* ssid, const char* pass, const char* ip); // AP details for the Network page
void oled_set_wifi_rssi(int rssi);
void oled_set_lora(LoRaStatus s);
void oled_set_lora_params(long freq, int sf, long bw);   // Hz; shown on the LoRa page
void oled_lora_packet(bool tx, bool ok, int rssi, float snr); // counters + last RSSI/SNR (rssi/snr ignored for tx)
void oled_message(const char* from, const char* text);       // received chat message: overlay + Messages page (last 5)
void oled_alert(const char* title, const char* text, uint32_t ms); // overlay, e.g. SOS / TX failed / Resetting
void oled_progress(const char* title, uint8_t pct);          // pct 0-100 shows it, 255 hides it
void oled_button(uint8_t btn, bool long_press);              // input from core's button task
void oled_status(const char* text);                          // one-line status (boot steps, "Connecting 12s")
```
Pages (BTN2 short press moves to the next page): **Status**, **Messages**, **LoRa**
(freq/SF/BW, last RSSI/SNR, TX ok/fail and RX counts), **Network** (mode, SSID, IP, AP
password when in AP mode), **System** (free heap, uptime, firmware build date).
- BTN2 long press: screen on/off.
- BTN1 short press goes to `oled_button(1,false)`: it dismisses an alert and steps through messages on the Messages page.
- `oled_alert` with `ms==0` stays on screen until a button is pressed.
- The screen sleeps after 60 s idle. Any button press, message or alert wakes it, and
  the press that wakes it does nothing else.
- BTN1: hold for 2 s to send SOS. Core shows `oled_progress("Hold for SOS", pct)`
  while it's held, then `oled_alert("SOS","sent",3000)`. Releasing early cancels.

## Hardware

Pins come from `build_flags` in `platformio.ini`. There are two boards:
`esp_lora_v1` / `esp_lora_v2` (dev, `DEBUG=true`) and `esp_lora_v1_release` /
`esp_lora_v2_release` (`DEBUG=false`). Every change must compile for both dev
environments. GPIO 34–39 have no internal pull resistors.
