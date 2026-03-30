# SENTRY-RF — Developer Context

## What is this project?
SENTRY-RF is an open-source, ESP32-based passive drone RF detector and GNSS jamming/spoofing detector. It combines sub-GHz spectrum scanning (via SX1262 LoRa radio) with GNSS integrity monitoring (via u-blox M10 GPS with UBX binary protocol) to detect drone activity and electronic warfare threats in the field.

This is a personal open-source project (MIT license) hosted under the Seaforged GitHub org. It is NOT a Seaforged commercial product — it's a learning project and community tool.

## Hardware targets
Two ESP32-S3 boards are supported via PlatformIO multi-environment builds:

### LilyGo T3S3 (primary dev board)
- **PlatformIO board:** `lilygo-t3-s3`
- **Build flag:** `-DBOARD_T3S3 -DARDUINO_USB_CDC_ON_BOOT=1`
- **LoRa SX1262 SPI:** SCK=5, MISO=3, MOSI=6, CS=7, RST=8, DIO1=33, BUSY=34
- **OLED SSD1306 I2C:** SDA=18, SCL=17, addr 0x3C (no reset pin needed)
- **GPS UART1:** RX=44, TX=43
- **LED:** GPIO 37
- **Has:** SD card slot, 2MB PSRAM, QWIIC connector, native USB (no CP2102)

### Heltec WiFi LoRa 32 V3 (compact alternative)
- **PlatformIO board:** `heltec_wifi_lora_32_V3`
- **Build flag:** `-DBOARD_HELTEC_V3`
- **LoRa SX1262 SPI:** SCK=9, MISO=11, MOSI=10, CS=8, RST=12, DIO1=14, BUSY=13
- **OLED SSD1306 I2C:** SDA=17, SCL=18, addr 0x3C, **RST=21** (must pulse), **Vext=36** (set LOW to power OLED)
- **GPS UART1:** RX=46, TX=45
- **LED:** GPIO 35
- **Has:** 8MB flash, CP2102 USB bridge, no PSRAM, no SD card
- **CRITICAL:** Must call `radio.setTCXO(1.8)` before `radio.begin()` or SX1262 returns error -2

### GPS Module: u-blox M10 (MAX-M10S or NEO-M10S)
- Connected via UART (4 wires: TX, RX, VCC 3.3V, GND)
- Default baud: 9600, reconfigure to 38400 via UBX-CFG-VALSET
- Key UBX messages: NAV-PVT (position), NAV-SAT (per-satellite C/N0), NAV-STATUS (spoofDetState), MON-RF (jamming/AGC)
- Uses SparkFun u-blox GNSS v3 library (M10 requires v3, not v2)

## Key libraries
- **RadioLib** (jgromes, v7+): SX1262 LoRa/FSK radio control, spectrum scanning via RSSI reads
- **SparkFun u-blox GNSS v3**: UBX protocol parsing, M10 configuration via VALSET/VALGET
- **Adafruit SSD1306 + GFX**: OLED display rendering

## Architecture overview
- **FreeRTOS dual-core:** LoRa scanning on Core 1, GPS reading on Core 0
- **Shared state:** `SystemState` struct protected by FreeRTOS mutex
- **Detection queue:** FreeRTOS queue (depth 10) carries events from sensor tasks to alert handler
- **Board abstraction:** `board_config.h` uses `#ifdef BOARD_T3S3` / `#elif defined(BOARD_HELTEC_V3)` for pin mapping

## Sprint structure
The project is built in 8 sprints (see SPRINT_ROADMAP.md in project knowledge):
1. Project skeleton + hardware validation
2. Sub-GHz spectrum scanner
3. GPS UART + UBX basics
4. GNSS integrity monitoring (jamming/spoofing detection)
5. FreeRTOS dual-core refactor
6. Detection engine + threat classification
7. Multi-screen UI, SD logging, WiFi dashboard
8. (Stretch) WiFi-based 2.4 GHz DJI Remote ID detection

## Development rules
- All code must compile for BOTH board targets: `pio run -e t3s3` and `pio run -e heltec_v3`
- Never hardcode pin numbers in source files — always use symbols from `board_config.h`
- Use `#include "board_config.h"` in every file that touches hardware
- Prefer `const` over `#define` for typed constants
- Keep functions short — if a function exceeds ~40 lines, consider splitting it
- Comment the "why", not the "what" — the code should be readable on its own
- Every new feature gets tested on serial monitor before touching the OLED display
- Commit after each acceptance criterion passes, not at the end of a sprint

## Common gotchas
- T3S3 uses native USB: if serial monitor doesn't connect, add `-DARDUINO_USB_CDC_ON_BOOT=1`
- Heltec V3 OLED won't display anything unless Vext (GPIO 36) is set LOW first
- Heltec V3 OLED needs a reset pulse: GPIO 21 LOW for 50ms, then HIGH, then wait 100ms
- SX1262 on Heltec V3 requires TCXO 1.8V or `begin()` fails with error -2
- SparkFun GNSS v3 `setVal` functions use `VAL_LAYER_RAM_BBR` to survive GPS sleep but not full power cycle
- `radio.getRSSI(false)` returns instantaneous RSSI; `true` returns last-packet RSSI (we want `false` for scanning)
- T3S3 I2C is SDA=18/SCL=17; Heltec V3 is SDA=17/SCL=18 — they're SWAPPED between boards

## The developer
The developer is experienced with RF hardware, embedded systems, FPV drones, and amateur radio, but is relatively early in formal software engineering education (starting MS in Computer Science April 2026). Explain CS concepts when introducing them (design patterns, data structures, algorithms) but don't over-explain hardware or RF fundamentals — they already know those well. When in doubt, teach the software side.
