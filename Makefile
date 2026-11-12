# Project Voyager - Makefile
# Convenient shortcuts for common development tasks
# Author: Aviral Verma
# Repository: https://github.com/aviralverma-8877/Project-Voyager

.PHONY: help build build-v1 build-v2 buildfs upload upload-v1 upload-v2 uploadfs monitor clean webflash \
	test test-web release all flash-v1 flash-v2 full-v1 full-v2 size-check check lint sync-data

# PlatformIO CLI: honour $(PIO) from the environment, else `pio` on PATH,
# else the default PlatformIO Core install location.
PIO ?= $(shell command -v pio 2>/dev/null || echo $(HOME)/.platformio/penv/bin/pio)
PYTHON ?= python3
DEFAULT_ENV ?= esp_lora_v1

# Default target
help:
	@echo "Project Voyager - Build System"
	@echo ""
	@echo "Available targets:"
	@echo "  make build        - Build firmware for all versions"
	@echo "  make build-v1     - Build firmware for version 1"
	@echo "  make build-v2     - Build firmware for version 2"
	@echo "  make buildfs      - Build LittleFS filesystem"
	@echo "  make release      - Build release firmware (DEBUG=false) for all versions"
	@echo "  make test         - Run native unit tests (pio test -e native)"
	@echo "  make test-web     - Run web UI tests (test/web/run.sh)"
	@echo "  make upload       - Upload firmware (default environment: $(DEFAULT_ENV))"
	@echo "  make upload-v1    - Upload firmware to version 1"
	@echo "  make upload-v2    - Upload firmware to version 2"
	@echo "  make uploadfs     - Upload filesystem (default environment: $(DEFAULT_ENV))"
	@echo "  make monitor      - Open serial monitor"
	@echo "  make clean        - Clean build artifacts"
	@echo "  make webflash     - Build release + fs for both boards into webflash/"
	@echo "                      (WEBFLASH_ARGS=--yes to skip the prompt)"
	@echo "  make size-check   - Show RAM/Flash usage for both boards"
	@echo "  make check        - Run full validation (build + tests + JS syntax)"
	@echo "  make lint         - Run PlatformIO static analysis"
	@echo "  make sync-data    - Sync data_main/ -> data/ (minify JS, compact JSON)"
	@echo ""
	@echo "Examples:"
	@echo "  make build-v1     # Build version 1 firmware"
	@echo "  make upload-v1    # Upload to connected ESP32"
	@echo "  make monitor      # View serial output"
	@echo "  make webflash     # Prepare web flasher files"
	@echo ""

# Build targets
build:
	@echo "Building firmware for all versions..."
	$(PIO) run -e esp_lora_v1
	$(PIO) run -e esp_lora_v2

build-v1:
	@echo "Building firmware for version 1..."
	$(PIO) run -e esp_lora_v1

build-v2:
	@echo "Building firmware for version 2..."
	$(PIO) run -e esp_lora_v2

buildfs:
	@echo "Building LittleFS filesystem..."
	$(PIO) run -e esp_lora_v1 --target buildfs
	$(PIO) run -e esp_lora_v2 --target buildfs

# Upload targets
upload:
	@echo "Uploading firmware (default)..."
	$(PIO) run -e $(DEFAULT_ENV) --target upload

upload-v1:
	@echo "Uploading firmware for version 1..."
	$(PIO) run -e esp_lora_v1 --target upload

upload-v2:
	@echo "Uploading firmware for version 2..."
	$(PIO) run -e esp_lora_v2 --target upload

uploadfs:
	@echo "Uploading LittleFS filesystem..."
	$(PIO) run -e $(DEFAULT_ENV) --target uploadfs

# Monitor
monitor:
	@echo "Opening serial monitor..."
	$(PIO) device monitor -b 115200

# Clean
clean:
	@echo "Cleaning build artifacts..."
	-$(PIO) run --target clean
	@if [ -d .pio ]; then rm -rf .pio; fi
	@echo "Clean complete!"

# Webflash preparation
webflash:
	@echo "Building and preparing webflash binaries..."
	@if [ -f scripts/build_webflash.py ]; then \
		PIO="$(PIO)" $(PYTHON) scripts/build_webflash.py $(WEBFLASH_ARGS); \
	else \
		echo "Error: scripts/build_webflash.py not found!"; \
		exit 1; \
	fi

# Release firmware (DEBUG=false); these are what webflash packages
release:
	@echo "Building release firmware for all versions..."
	$(PIO) run -e esp_lora_v1_release
	$(PIO) run -e esp_lora_v2_release

# Tests
test:
	@echo "Running native unit tests..."
	$(PIO) test -e native

test-web:
	@echo "Running web UI tests..."
	bash test/web/run.sh

# Combined workflows
all: build buildfs
	@echo "All builds complete!"

flash-v1: build-v1 upload-v1
	@echo "Version 1 flashed successfully!"

flash-v2: build-v2 upload-v2
	@echo "Version 2 flashed successfully!"

full-v1: build-v1 buildfs upload-v1 uploadfs
	@echo "Version 1 fully flashed (firmware + filesystem)!"

full-v2: build-v2 buildfs upload-v2 uploadfs
	@echo "Version 2 fully flashed (firmware + filesystem)!"

# Size check - shows RAM/Flash usage without rebuilding if up-to-date
size-check: build
	@echo ""
	@echo "=== Firmware Size Summary ==="
	@$(PIO) run -e esp_lora_v1 --target size 2>/dev/null | grep -E "(RAM|Flash):" || true
	@$(PIO) run -e esp_lora_v2 --target size 2>/dev/null | grep -E "(RAM|Flash):" || true

# Full validation suite
check: build
	@echo ""
	@echo "=== Running validation suite ==="
	@echo "Checking JavaScript syntax..."
	@for f in data_main/script*.js data_main/lib.js; do node --check $$f || exit 1; done; echo "JavaScript: OK"
	@echo ""
	@echo "Checking data/ sync (expect script*.js, lib.js, the .gz assets and config/ to differ)..."
	@diff -rq data data_main 2>/dev/null || true
	@echo ""
	@echo "=== Validation complete ==="

# Static analysis
lint:
	@echo "Running PlatformIO static analysis..."
	$(PIO) check -e esp_lora_v1 --skip-packages --severity=medium

# Sync data_main/ to data/ (minify JS, compact JSON)
sync-data:
	@echo "Syncing data_main/ -> data/..."
	rsync -a --delete --exclude config/ --exclude 'script*.js' --exclude lib.js data_main/ data/
	@echo "Minifying JavaScript..."
	@for src in data_main/script*.js data_main/lib.js; do \
		f=$$(basename $$src); \
		npx --yes terser@5.31.0 $$src -c -m -o data/$$f || exit 1; \
		echo "  $$f minified"; \
	done
	@echo "Compressing large assets (the web server serves <file>.gz automatically)..."
	@for src in data/bootstrap.min.css data/bootstrap.min.js data/jquery-3.7.1.min.js data/script*.js data/lib.js; do \
		f=$$(basename $$src); \
		gzip -9 -n -f data/$$f; \
		echo "  $$f.gz"; \
	done
	@echo "Compacting JSON configs..."
	@mkdir -p data/config
	@for f in data_main/config/*.json; do \
		$(PYTHON) -c "import json,sys;print(json.dumps(json.load(open(sys.argv[1])),separators=(',',':')),end='')" "$$f" > "data/config/$$(basename $$f)"; \
		echo "  $$(basename $$f) compacted"; \
	done
	@echo "Sync complete!"
