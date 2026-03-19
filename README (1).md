# SENTRY-RF

**Passive drone RF detection + GNSS jamming/spoofing monitor for ESP32-S3**

SENTRY-RF is an open-source field tool that combines sub-GHz spectrum scanning with GNSS integrity monitoring on low-cost ESP32 hardware. It detects drone control links in the 868/915 MHz ISM band and monitors GPS signals for signs of jamming or spoofing attacks.

## What it does

- **RF Spectrum Scanning** — Sweeps 860–930 MHz using the SX1262 LoRa radio, measuring signal energy at each frequency. Detects ExpressLRS, TBS Crossfire, and other sub-GHz drone control links by matching detected signals against known frequency plans.
- **GNSS Integrity Monitoring** — Reads u-blox M10 jamming indicators (UBX-MON-RF), built-in spoofing detection (UBX-NAV-STATUS), and per-satellite signal quality (UBX-NAV-SAT). Runs host-side algorithms for C/N0 anomaly detection and position consistency checks.
- **Threat Classification** — Correlates RF detections with GNSS anomalies to produce actionable threat levels: CLEAR → ADVISORY → WARNING → CRITICAL.
- **Field-Ready UI** — Multi-screen OLED display with spectrum view, GPS status, integrity dashboard, and threat alerts. SD card logging and WiFi dashboard for remote monitoring.

## Supported hardware

| Board | Chip | LoRa | Display | Extras |
|-------|------|------|---------|--------|
| **LilyGo T3S3** | ESP32-S3 | SX1262 (868/915 MHz) | SSD1306 128×64 OLED | SD card, PSRAM, QWIIC |
| **Heltec WiFi LoRa 32 V3** | ESP32-S3 | SX1262 (868/915 MHz) | SSD1306 128×64 OLED | Compact form factor |

**GPS:** u-blox M10 module (MAX-M10S or NEO-M10S) connected via UART.

## Quick start

### Prerequisites
- [PlatformIO](https://platformio.org/) (CLI or VS Code extension)
- One of the supported boards
- u-blox M10 GPS module + 4 jumper wires

### Build and flash
```bash
# Clone the repo
git clone https://github.com/Seaforged/SENTRY-RF.git
cd SENTRY-RF

# Build for your board
pio run -e t3s3          # LilyGo T3S3
pio run -e heltec_v3     # Heltec WiFi LoRa 32 V3

# Upload
pio run -e t3s3 --target upload

# Open serial monitor
pio device monitor
```

### Wiring the GPS module

| GPS Pin | T3S3 GPIO | Heltec V3 GPIO |
|---------|-----------|----------------|
| TX | 44 | 46 |
| RX | 43 | 45 |
| VCC | 3.3V | 3.3V |
| GND | GND | GND |

## Project status

🚧 **Under active development** — See the sprint roadmap in project docs for current progress.

## License

[MIT](LICENSE) — do whatever you want with it.

## Contributing

Issues and PRs welcome. This is a learning project built in public — if you see something that could be better, open an issue or submit a fix.
