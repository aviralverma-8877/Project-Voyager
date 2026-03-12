# Changelog

All notable changes to Project Voyager will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Professional OLED display system with modern UI
  - Status icons for WiFi, LoRa, MQTT, and signal strength
  - Multiple display modes (Splash, Status, Connecting, Error, Message)
  - Boot animation with expanding circles
  - Progress bars for long operations
  - Text wrapping and centered text helpers
  - Comprehensive API documentation
- MIT License file
- CONTRIBUTORS.md file
- CHANGELOG.md file
- Comprehensive README.md with detailed documentation

### Changed
- Migrated from SPIFFS to LittleFS filesystem
  - Better reliability and performance
  - Updated all file operations
  - Modified OTA update endpoints
  - Updated platformio.ini build flags
- Major performance and communication improvements
  - Replaced blocking ACK wait with semaphore-based system
  - Fixed race conditions with proper FreeRTOS synchronization
  - Optimized string concatenation with pre-allocated buffers
  - Implemented queue backpressure instead of system reboot
  - Improved WebSocket client tracking with proper counting
- Enhanced error handling throughout codebase
  - Added JSON deserialization error checking
  - ISR-safe queue operations
  - Memory leak prevention in queue overflow scenarios

### Fixed
- LoRa transmission blocking issues (replaced tight polling loop)
- Race conditions on ACK flag between ISR and tasks
- Memory fragmentation from inefficient string operations
- System reboots on queue overflow
- WebSocket state tracking with multiple clients
- Deadlock risks with infinite waits

### Performance Improvements
- Non-blocking ACK mechanism reduces latency
- Pre-allocated string buffers reduce heap fragmentation
- Task yielding prevents starvation
- Mutex-protected LoRa access prevents race conditions
- Queue backpressure maintains system stability under load

## [1.0.0] - Initial Release

### Features
- ESP32-based LoRa communication system
- Web-based configuration interface
- WiFi Access Point and Station modes
- MQTT broker integration
- File transfer over LoRa
- Real-time WebSocket updates
- OTA firmware updates
- OLED display support
- Dual hardware version support (v1 and v2)

### Hardware Support
- ESP32-WROOM-32 microcontroller
- SX1276/77/78/79 LoRa modules
- SSD1306 OLED display (128x64)
- Push buttons and LED indicators

### Communication Protocols
- LoRa with custom packet protocol
- HTTP REST API
- WebSocket bidirectional communication
- Server-Sent Events (SSE)
- MQTT publish/subscribe

---

## Legend

- **Added** - New features
- **Changed** - Changes to existing functionality
- **Deprecated** - Soon-to-be removed features
- **Removed** - Removed features
- **Fixed** - Bug fixes
- **Security** - Vulnerability fixes

---

**Repository:** https://github.com/aviralverma-8877/Project-Voyager
**Author:** Aviral Verma (aviral.verma.8877@gmail.com)
