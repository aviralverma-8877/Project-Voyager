---
name: voyager-test
description: Test strategy for Project Voyager components: compile gate, static analysis, native Unity unit tests, web UI checks, and the two-board hardware test protocol. Use when writing or running tests for any component, and to report test results honestly.
---

# Testing Project Voyager

Most of this is embedded code, and agents can't touch the radio. Tests come in layers.
Run every layer that applies to your component, then report each one as
**passed / failed / not run (reason)**. Never write "tested" when only the compile gate ran.

```bash
PIO=~/.platformio/penv/bin/pio
```

## L0 Compile gate (always)

Both `esp_lora_v1` and `esp_lora_v2` must build. The commands are in the `voyager-build` skill.

## L1 Static analysis

```bash
$PIO check -e esp_lora_v1 --skip-packages --severity=medium --pattern="src/<your files>"
```
Triage the results against the `voyager-review` checklist instead of pasting the raw output.

## L2 Native unit tests (Unity, run on the host)

Put tests in `test/test_<component>/test_main.cpp`. Run them with:
```bash
$PIO test -e native -f test_<component>
```
The `[env:native]` environment (`platform = native`, `test_framework = unity`) belongs
to `build-release-engineer`. If it doesn't exist yet, ask that agent to add it rather
than editing `platformio.ini` yourself.

Only logic that doesn't depend on Arduino or ESP-IDF can run natively. When a function
you own mixes logic with hardware calls, pull the pure part out into a small function
that takes plain types. That function can live in a header under `src/` or a library
under `lib/`. For example:
- packet framing and parsing, the checksum, and the retry state machine (lora)
- `request-type` dispatch and argument validation (core)
- config JSON parsing and defaults (wifi, lora, core)
- text wrapping and progress-bar math (oled)

ArduinoJson builds natively. Arduino's `String` doesn't, so use `std::string` in the
code you extract.

## L3 Web UI checks (web-ui-engineer)

Use `jsc` (macOS JavaScriptCore, see `voyager-build`) to check syntax, and run the
`data/` sync check. Logic tests live in `test/web/test_*.js`. Each one starts with
`load("test/web/harness.js")` and `load("data_main/<file>.js")`, then makes assertions.
Run them all with `bash test/web/run.sh`. There's no DOM, so keep testable logic in
pure functions (for example in `data_main/lib.js`). `script.js` should only handle the
DOM and network wiring.

## L4 Hardware-in-the-loop (written by agents, run by the user)

Agents can't run this layer. For each change, write a short checklist the user can run
with **two boards** A and B: steps, the expected serial or OLED output, and pass
criteria. Reusable scenarios:
1. Boot both boards and check each one comes up as an AP. The OLED shows the SSID and IP.
2. A → B text message. It appears on B's dashboard, and A's request returns `akn:1`.
3. Send a 20 KB file A → B and compare SHA-256 of the sent and received files.
4. Power B off mid-transfer. A should retry 3 times, show the alert and stop cleanly
   without rebooting.
5. Run a two-minute burst of messages in both directions, then check free heap in
   `RAM_DATA` hasn't kept falling.
6. Switch to STA, then take the router down for 30 s. The device should reconnect and
   keep its saved network.

## Coverage expectation

Each owner keeps a list of their component's public functions in their report and marks
which layer covers each one. Anything not covered by L0–L3 needs an L4 step.
