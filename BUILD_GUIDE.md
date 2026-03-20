# SENTRY-RF Build Guide & Bill of Materials

## Bill of Materials

### Required Components

| Component | Recommended Part | Approx. Cost | Notes |
|-----------|-----------------|-------------|-------|
| Development Board | LilyGo T3S3 V1.3 (SX1262, 868/915 MHz) | $22-28 | ESP32-S3 + SX1262 LoRa + 0.96" OLED + SD card + JST battery connector |
| GPS Module | u-blox MAX-M10S breakout (SparkFun GPS-18037) or NEO-M10S | $15-25 | Must be u-blox M10 series for UBX integrity monitoring. Connects via UART (4 wires) |
| Sub-GHz Antenna | 868/915 MHz SMA antenna + U.FL pigtail | $5-8 | Tuned for your regional ISM band. Connect to the T3S3's U.FL antenna port |
| Jumper Wires | Male-to-female Dupont wires (4 needed) | $3 | For GPS UART + power connections |

### Optional Components

| Component | Recommended Part | Approx. Cost | Notes |
|-----------|-----------------|-------------|-------|
| Battery | 18650 Li-Ion cell (3000+ mAh, protected) | $5-10 | Connects to T3S3's JST 1.25mm 2-pin battery connector. Board has built-in TP4056 charger (~500mA charge rate via USB-C). **Use a protected cell** — the board handles charging but not deep discharge protection. Expect 6-8 hours runtime. |
| 18650 Holder | Single-cell holder with JST 1.25mm leads | $2 | Or solder leads directly to the battery tabs |
| Compass Module | QMC5883L breakout (GY-271 with 0x0D address) | $3-5 | Connects via QWIIC/I2C on Wire1 (SDA=21, SCL=10). Auto-detected at boot. Enables heading + directional bearing for RF signals |
| SD Card | MicroSD, any size, FAT32 formatted | $5 | For detection event logging. Slot is on the T3S3 board |
| Enclosure | 3D printed case (search Thingiverse for "T3S3 18650") | $0-5 | Several community designs available that fit T3S3 + 18650 |

### Future Upgrade (LR1121 dual-band)

| Component | Recommended Part | Approx. Cost | Notes |
|-----------|-----------------|-------------|-------|
| LR1121 Board | LilyGo T3-S3 LR1121 | $24 | Same form factor as T3S3 SX1262 but adds 2.4 GHz spectrum scanning. Drop-in firmware replacement — same pin mappings |
| 2.4 GHz Antenna | Dual-band or separate 2.4 GHz SMA antenna | $5-8 | For the LR1121's 2.4 GHz band |

**Total cost (basic build): ~$45-65**
**Total cost (full build with battery + compass + SD): ~$60-85**

---

## Wiring Diagram

### GPS Module → T3S3

```
GPS Module          T3S3 Board
──────────          ──────────
TX  ──────────────→ GPIO 44 (UART1 RX)
RX  ←──────────────  GPIO 43 (UART1 TX)
VCC ──────────────→ 3.3V
GND ──────────────→ GND
```

### Compass Module → T3S3 (Optional, via QWIIC)

If using a QWIIC-compatible QMC5883L breakout, connect via the QWIIC port.
If using bare wires:

```
QMC5883L            T3S3 Board
────────            ──────────
SDA ──────────────→ GPIO 21
SCL ──────────────→ GPIO 10
VCC ──────────────→ 3.3V
GND ──────────────→ GND
```

### Battery → T3S3

Connect a single 3.7V 18650 cell (protected) to the JST 1.25mm 2-pin battery connector on the T3S3. **Check polarity before connecting** — the positive wire (red) must match the + marking on the board.

The board charges the battery at ~500mA when USB-C is plugged in. It runs from battery when USB is disconnected. Battery voltage is monitored on GPIO 1 and displayed as a percentage on the OLED dashboard.

**WARNING:** Always connect the antenna before powering on. Transmitting without an antenna can damage the SX1262 radio module.

---

## Software Setup & Flashing

### Prerequisites

1. **Install PlatformIO** — either as a VS Code extension or CLI:
   ```bash
   # VS Code: Install "PlatformIO IDE" extension from the marketplace
   
   # Or CLI only:
   pip install platformio
   ```

2. **Install Git:**
   ```bash
   # Windows: Download from https://git-scm.com/downloads
   # Mac: brew install git
   # Linux: sudo apt install git
   ```

3. **Install USB drivers** (if needed):
   - The T3S3 uses native USB (no CP2102) — most modern OS versions detect it automatically
   - If the board doesn't appear as a COM port, install the ESP32-S3 USB drivers from Espressif

### Clone and Build

```bash
# Clone the repository
git clone https://github.com/Seaforged/SENTRY-RF.git
cd SENTRY-RF

# Build for your board
pio run -e t3s3              # LilyGo T3S3 (SX1262)
pio run -e heltec_v3         # Heltec WiFi LoRa 32 V3
pio run -e t3s3_lr1121       # LilyGo T3S3 (LR1121 dual-band)
```

PlatformIO will automatically download all required dependencies:
- RadioLib v7+ (LoRa radio control)
- SparkFun u-blox GNSS v3 (GPS UBX protocol)
- Adafruit SSD1306 + GFX (OLED display)

### Flash to Device

1. Connect the T3S3 via USB-C
2. **Connect the antenna first** — never power on without an antenna attached
3. Flash:
   ```bash
   pio run -e t3s3 --target upload
   ```
4. If the upload fails, hold BOOT button while pressing RESET, then release BOOT — this puts the ESP32-S3 into download mode
5. Open serial monitor to verify:
   ```bash
   pio device monitor -b 115200
   ```

You should see:
```
[BOOT] Boot #1
========================
 SENTRY-RF v1.1.0
 Build: Mar 20 2026
 Board: LilyGo T3S3
 Mode:  FreeRTOS dual-core
========================
[OLED] OK
[SCAN] FSK mode ready, 700 bins, 860.0–930.0 MHz
[GPS] Connected at 38400 baud — configuring
[COMPASS] Not detected — continuing without compass
[INIT] FreeRTOS tasks launched — LoRa:Core1, GPS+WiFi:Core0
```

### First Boot Checklist

- [ ] SENTRY-RF logo appears on OLED for ~2 seconds
- [ ] Dashboard screen shows GPS fix acquiring, then "3D" with satellite count
- [ ] RF SCAN screen shows spectrum (flat line in quiet environment)
- [ ] Serial monitor shows sweep times (~460ms) and GPS data
- [ ] Battery percentage shows on dashboard (97-100% on USB power)
- [ ] Button press cycles through 6 screens

---

## Transferring Repository to Seaforged Organization

When you're ready to move the repo to the Seaforged GitHub organization:

```bash
# Option 1: Transfer via GitHub web UI (preserves all history, stars, issues)
# Go to repo Settings → Danger Zone → Transfer ownership → Enter "Seaforged"

# Option 2: Change remote URL manually
git remote set-url origin https://github.com/Seaforged/SENTRY-RF.git
git push -u origin main
```

Option 1 (GitHub transfer) is recommended — it redirects the old URL automatically and preserves everything.

---

## Project Status

**Current version: v1.1.0 — Early Development**

This project is in active early development. It has been tested on a single T3S3 V1.3 unit in a controlled indoor environment. Field testing with real drones has not yet been performed.

### What works today
- Sub-GHz spectrum scanning (860-930 MHz) with drone frequency matching
- GNSS integrity monitoring (spoofing detection, C/N0 analysis)
- WiFi promiscuous mode scanning for Remote ID beacons
- 6-screen OLED UI with custom boot splash
- FreeRTOS dual-core parallel processing
- Detection engine with 14 drone protocol signatures
- Threat classification (CLEAR → ADVISORY → WARNING → CRITICAL)

### What needs testing/validation
- LR1121 2.4 GHz dual-band scanning (hardware on order)
- Detection against real drone targets (ELRS, DJI, Crossfire)
- Compass heading and directional bearing
- SD card logging
- Long-duration stability (8+ hours on battery)
- False positive rates in various RF environments
- MON-HW jamming indicator (currently not populating — under investigation)

### Known limitations
- Single antenna — no true direction finding (rotation-based bearing estimation only)
- SX1262 boards cannot scan 2.4 GHz (LR1121 board required)
- Cannot detect 5.8 GHz signals (analog FPV video, DJI video downlink)
- Cannot demodulate drone protocols — detection is based on RF energy + frequency matching
- GPS_MIN_CNO set to 15 for outdoor use — may need adjustment for deep indoor testing
