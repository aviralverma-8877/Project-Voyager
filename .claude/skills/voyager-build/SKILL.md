---
name: voyager-build
description: How to build, size-check and package Project Voyager firmware and filesystem with PlatformIO, and how to regenerate the minified web UI in data/ from data_main/. Use before claiming any firmware or web UI change compiles or is ready.
---

# Building Project Voyager

PlatformIO is not on PATH on this machine. Use the full path:

```bash
PIO=~/.platformio/penv/bin/pio
```

## Compile gate (required for every firmware change)

```bash
$PIO run -e esp_lora_v1
$PIO run -e esp_lora_v2
```
When several agents build at the same time, give each one its own build directory:
`PLATFORMIO_BUILD_DIR=.pio/build-<your-agent-name> $PIO run -e esp_lora_v1`.
Don't change `lib_deps` (that belongs to build-release-engineer) and don't delete `.pio/libdeps`.
Both must pass. If you change `platformio.ini` build flags or pins, both boards matter
because the pin maps differ.

Report the RAM/Flash usage lines that both builds print. Compare them with the numbers
from before your change. More than about 2% growth in RAM is worth calling out.

## Filesystem image

```bash
$PIO run -e esp_lora_v1 -t buildfs     # packs data/ into littlefs.bin
```
PlatformIO packs `data/`, **not** `data_main/`.

## Web UI: data_main → data

`data_main/` holds the readable source. `data/` holds the **minified** build that
ships. HTML, CSS, vendored libraries and other assets are copied unchanged. Our own JS
(`script.js`, `lib.js`) is minified with terser (pinned version), and the config JSON
is compacted:

```bash
rsync -a --delete --exclude config/ --exclude script.js --exclude lib.js data_main/ data/
for f in script.js lib.js; do [ -f data_main/$f ] && npx --yes terser@5.31.0 data_main/$f -c -m -o data/$f; done
mkdir -p data/config
for f in data_main/config/*.json; do python3 -c "import json,sys;print(json.dumps(json.load(open(sys.argv[1])),separators=(',',':')),end='')" "$f" > "data/config/$(basename "$f")"; done
```
Never hand-edit files in `data/`.

Sync check (it should list only `script.js`, `lib.js` and `config/*`):
```bash
diff -rq data data_main
```

JavaScript syntax check and test runner: use the JavaScriptCore shell that ships with macOS.
```bash
JSC=/System/Library/Frameworks/JavaScriptCore.framework/Versions/A/Helpers/jsc
node --check data_main/script.js && node --check data_main/lib.js
# web unit tests (JavaScriptCore harness):
bash test/web/run.sh
```

## Web flasher packaging

`python3 scripts/build_webflash.py` builds both boards and the filesystem, then copies
the binaries into `webflash/`. Run it only when you're preparing a release. The
binaries it creates are committed.

## Don't

- Don't run `upload`, `uploadfs` or `erase`. There may not be a board attached, and
  flashing is the user's decision.
- Don't commit `.pio/`.
