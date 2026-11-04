---
name: voyager-review
description: Review checklist for Project Voyager code (ESP32/FreeRTOS correctness, memory, concurrency, radio protocol, web security, UI injection) and the required findings format. Use when reviewing a diff or auditing a component.
---

# Reviewing Project Voyager code

Review your own component, or the diff you were given. Report only findings you can tie
to a concrete failure scenario.

## ESP32 / FreeRTOS checklist

- **ISR context**: the LoRa library calls `onReceive` and `onTxDone` from the DIO0
  interrupt. In that context, flag any heap allocation (`String`, `new`), blocking call
  (`xSemaphoreTake` with a timeout, `vTaskDelay`), use of the non-`FromISR` queue API,
  `serial_print`, SPI transaction, or `Serial`.
- **Task lifetime**: every `xTaskCreate*` function must loop forever or call
  `vTaskDelete(NULL)` on *every* exit path, including early `return`s.
- **Queue ownership**: queues carry pointers. Check for a leak when `xQueueSend` fails,
  a double delete, or a missing `delete` after `xQueueReceive`.
- **Shared state**: globals written by one task and read by another (`lora_serial`,
  `notify`, `AknRecieved`, `display_buffer`) need `volatile`, a mutex or a queue. Radio
  register access outside `lora_write_mutex` is a race.
- **Blocking in async callbacks**: AsyncWebServer and AsyncTCP handlers must not block
  or delay. Hand the work off to a task.
- **Null and parse handling**: check for `getParam(0)` without a null check, `strcmp` on
  a missing JSON key, and an ignored `deserializeJson` error.
- **Heap**: watch for `String` concatenation in loops and `JsonDocument` passed by value.
  Large stack objects inside 6000-byte tasks are also a risk.
- **Radio**: check the payload stays ≤ 252 bytes, the TX→RX mode switch timing
  (`endPacket(true)` is non-blocking), IQ inversion symmetry between peers, and that
  every non-serial packet is ACKed exactly once.
- **Flash writes**: files must be closed. Check for partial-write handling and for
  config being overwritten in failure paths.

## Security checklist

- Unauthenticated actions that are destructive or code-executing (OTA, reset, WiFi change).
- Secrets exposed through `serveStatic`, `serial_print` or the `DEBUG` event stream.
- Web UI: any received LoRa text, username or SSID inserted with `.html()`, `.prepend()`,
  `.append()` or string-built HTML is XSS. Use `.text()` or escape it.
- Open AP: `softAP` without a password.

## Findings format

Sort findings by severity and give each one:
```
[critical|high|medium|low] path/to/file.cpp:LINE: one-sentence defect
  Scenario: concrete input/state → wrong result/crash
  Fix: the smallest correct change (and owner, if not you)
```
Finish with "Not reviewed:" listing any files in your area you didn't read. If there
are no findings, say so explicitly.
