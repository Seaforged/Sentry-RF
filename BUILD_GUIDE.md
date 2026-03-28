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
========== SENTRY-RF v1.2.1 ==========
[BOOT] Boot #1
[OLED] OK
[SCAN] FSK mode ready, 350 bins, 860.0–930.0 MHz
[GPS] FAIL: no response at 9600 or 38400    ← normal if no GPS connected
[INIT] GPS init failed — continuing without GPS
[WIFI] Promiscuous scanner active — channel hopping
[COMPASS] Not detected — continuing without compass
[INIT] FreeRTOS tasks launched — LoRa:Core1, GPS+WiFi:Core0
[SCAN] Peak: 881.2 MHz @ -72.5 dBm (2407 ms) | Threat: CLEAR
```

**Important build notes (v1.2.1):**
- The scanner uses `beginFSK(915.0, ...)` for correct SX1262 image calibration and `startReceive()` per bin for real RSSI values — this is why you see 350 bins at 200kHz step (~2.4s sweep) instead of the original 700 bins
- The T3S3 requires `board_build.flash_size = 4MB` in platformio.ini — without this, the ESP32-S3 boot-loops with a flash size mismatch error
- GPS and compass modules are **optional** — the system runs fine without them, showing "NO GPS" on the GPS/Integrity screens

### First Boot Checklist

- [ ] SENTRY-RF splash logo appears on OLED during boot (~8-10s with GPS timeout)
- [ ] Dashboard screen shows threat level, mini spectrum bars, battery percentage
- [ ] RF SCAN screen shows spectrum with real ambient peaks (not flat)
- [ ] GPS screen shows "NO GPS" if no module connected (not garbage numbers)
- [ ] Serial monitor shows sweep times (~2400ms) and threat level per sweep
- [ ] Short button press cycles through 6 screens
- [ ] LED stays off (disabled in v1.2.1 pending field testing)

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

**Current version: v1.2.1**

Validated against [JUH-MAK-IN JAMMER](https://github.com/Seaforged/Juh-Mak-In-Jammer) test suite — 8/8 modes passing (CW, ELRS FHSS, band sweep, Remote ID, mixed false positive, combined, drone swarm, baseline).

### What works today
- Sub-GHz spectrum scanning (860-930 MHz) with per-bin RX mode and 200kHz resolution
- Multi-peak 902-928 MHz ELRS band detection with dual threshold (Pfa < 8%)
- WiFi Remote ID detection via ASTM F3411 vendor-specific IE parsing (any drone MAC)
- GNSS integrity monitoring (spoofing detection, C/N0 analysis) — requires GPS module
- Audible buzzer alert system with 7 tone patterns, ACK, mute (requires buzzer on GPIO 16)
- 6-screen OLED UI with custom boot splash, display on Core 0 for clean rendering
- FreeRTOS dual-core: LoRa scanning (Core 1), GPS + WiFi + Display (Core 0)
- Detection engine with 14 drone protocol signatures and persistence tracking
- Automated dual-device testing via Python scripts against JAMMER test suite

### What needs field testing
- Detection thresholds vs real drones (current thresholds escalate on ambient ISM traffic)
- LED alert behavior (disabled in v1.2.1 — needs thresholds that don't false-alarm)
- LR1121 2.4 GHz dual-band scanning (hardware on order)
- Compass heading and directional bearing
- Long-duration stability (8+ hours on battery)

### Known limitations
- LED and buzzer alerts disabled pending field-calibrated detection thresholds
- Ambient 868/915 MHz ISM traffic causes false escalation to WARNING/CRITICAL on bench
- Single antenna — no true direction finding
- SX1262 boards cannot scan 2.4 GHz (LR1121 board required)
- Cannot detect 5.8 GHz signals
- GPS_MIN_CNO set to 15 for outdoor use — may need adjustment for indoor testing
