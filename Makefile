# Project Voyager - Makefile
# Convenient shortcuts for common development tasks
# Author: Aviral Verma
# Repository: https://github.com/aviralverma-8877/Project-Voyager

.PHONY: help build build-v1 build-v2 buildfs upload upload-v1 upload-v2 uploadfs monitor clean webflash

# Default target
help:
	@echo "Project Voyager - Build System"
	@echo ""
	@echo "Available targets:"
	@echo "  make build        - Build firmware for all versions"
	@echo "  make build-v1     - Build firmware for version 1"
	@echo "  make build-v2     - Build firmware for version 2"
	@echo "  make buildfs      - Build LittleFS filesystem"
	@echo "  make upload       - Upload firmware (default environment)"
	@echo "  make upload-v1    - Upload firmware to version 1"
	@echo "  make upload-v2    - Upload firmware to version 2"
	@echo "  make uploadfs     - Upload filesystem"
	@echo "  make monitor      - Open serial monitor"
	@echo "  make clean        - Clean build artifacts"
	@echo "  make webflash     - Build and prepare webflash binaries"
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
	pio run -e esp_lora_v1
	pio run -e esp_lora_v2

build-v1:
	@echo "Building firmware for version 1..."
	pio run -e esp_lora_v1

build-v2:
	@echo "Building firmware for version 2..."
	pio run -e esp_lora_v2

buildfs:
	@echo "Building LittleFS filesystem..."
	pio run -e esp_lora_v1 --target buildfs

# Upload targets
upload:
	@echo "Uploading firmware (default)..."
	pio run --target upload

upload-v1:
	@echo "Uploading firmware for version 1..."
	pio run -e esp_lora_v1 --target upload

upload-v2:
	@echo "Uploading firmware for version 2..."
	pio run -e esp_lora_v2 --target upload

uploadfs:
	@echo "Uploading LittleFS filesystem..."
	pio run -e esp_lora_v1 --target uploadfs

# Monitor
monitor:
	@echo "Opening serial monitor..."
	pio device monitor -b 115200

# Clean
clean:
	@echo "Cleaning build artifacts..."
	pio run --target clean
	@if [ -d .pio ]; then rm -rf .pio; fi
	@echo "Clean complete!"

# Webflash preparation
webflash:
	@echo "Building and preparing webflash binaries..."
	@if [ -f build_webflash.py ]; then \
		python build_webflash.py; \
	else \
		@echo "Error: build_webflash.py not found!"; \
		exit 1; \
	fi

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
