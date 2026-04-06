# SENTRY-RF Build Guide & Bill of Materials

## Bill of Materials

### Required Components

| Component | Recommended Part | Approx. Cost | Notes |
|-----------|-----------------|-------------|-------|
| Development Board | LilyGo T3S3 V1.3 (SX1262, 868/915 MHz) | $22-28 | ESP32-S3 + SX1262 LoRa + 0.96" OLED + SD card + JST battery connector |
| Sub-GHz Antenna | 868/915 MHz SMA antenna + U.FL pigtail | $5-8 | Tuned for your regional ISM band. **Never power on without antenna connected** |

**Minimum cost: ~$25-35**

### Optional Components

| Component | Recommended Part | Approx. Cost | Notes |
|-----------|-----------------|-------------|-------|
| GPS Module | u-blox MAX-M10S breakout (SparkFun GPS-18037) or FlyFishRC M10QMC-5883L | $15-25 | Must be u-blox M10 for UBX integrity monitoring. UART (4 wires) |
| Buzzer | KY-006 passive piezo buzzer | $1-2 | GPIO 16 (T3S3) or GPIO 47 (Heltec). 7 alert tone patterns |
| Battery | 18650 Li-Ion cell (3000+ mAh, protected) | $5-10 | JST 1.25mm 2-pin. Built-in TP4056 charger via USB-C. 6-8 hours runtime |
| 18650 Holder | Single-cell holder with JST 1.25mm leads | $2 | Or solder leads directly |
| Compass Module | QMC5883L breakout (GY-271, address 0x0D) | $3-5 | I2C on QWIIC (T3S3 only, SDA=21, SCL=10). Heading + signal bearing |
| SD Card | MicroSD, any size, FAT32 formatted | $5 | Detection event logging (T3S3 only) |
| Enclosure | 3D printed case | $0-5 | Search Thingiverse for "T3S3 18650". Note: adds ~3 dB signal attenuation |

**Full build cost: ~$60-85**

### Alternative Board: Heltec WiFi LoRa 32 V3

| Component | Recommended Part | Approx. Cost | Notes |
|-----------|-----------------|-------------|-------|
| Development Board | Heltec WiFi LoRa 32 V3 | $18-22 | ESP32-S3 + SX1262 + OLED. Smaller, no SD card, no QWIIC. Uses CP2102 USB bridge |
| Sub-GHz Antenna | Same as above | $5-8 | |

The Heltec V3 is a compact alternative with the same detection capabilities. It lacks an SD card slot (uses SPIFFS for logging, rotates at 100KB), has no QWIIC connector for compass, and requires a TCXO init step (handled automatically by the firmware).

### Future Upgrade: LR1121 Dual-Band

| Component | Recommended Part | Approx. Cost | Notes |
|-----------|-----------------|-------------|-------|
| LR1121 Board | LilyGo T3-S3 LR1121 | $24 | Same form factor as T3S3 SX1262, adds 2.4 GHz scanning for DJI/ELRS 2.4. Drop-in firmware replacement |
| 2.4 GHz Antenna | Dual-band or separate 2.4 GHz SMA antenna | $5-8 | |

---

## Wiring

### GPS Module (T3S3)

```
GPS Module          T3S3 Board
----------          ----------
TX  -----------------> GPIO 44 (UART1 RX)
RX  <----------------- GPIO 43 (UART1 TX)
VCC -----------------> 3.3V
GND -----------------> GND
```

### GPS Module (Heltec V3)

```
GPS Module          Heltec V3
----------          ---------
TX  -----------------> GPIO 46 (UART1 RX)
RX  <----------------- GPIO 45 (UART1 TX)
VCC -----------------> 3.3V
GND -----------------> GND
```

### Buzzer

```
T3S3:     GPIO 16 -> Buzzer + / Buzzer - -> GND
Heltec:   GPIO 47 -> Buzzer + / Buzzer - -> GND
```

### Compass (T3S3 only, via QWIIC or bare wires)

```
QMC5883L            T3S3 Board
--------            ----------
SDA -----------------> GPIO 21
SCL -----------------> GPIO 10
VCC -----------------> 3.3V
GND -----------------> GND
```

### Battery (T3S3)

Connect a single 3.7V 18650 cell (protected) to the JST 1.25mm 2-pin battery connector. **Check polarity before connecting.** The board charges at ~500mA via USB-C and runs from battery when USB is disconnected. Battery percentage is shown on the OLED dashboard.

---

## Software Setup

### Prerequisites

1. **PlatformIO** -- VS Code extension or CLI:
   ```bash
   pip install platformio
   ```

2. **Git:**
   ```bash
   git clone https://github.com/Seaforged/Sentry-RF.git
   cd Sentry-RF
   ```

### Build

```bash
pio run -e t3s3          # LilyGo T3S3 (SX1262)
pio run -e heltec_v3     # Heltec WiFi LoRa 32 V3
pio run -e t3s3_lr1121   # LilyGo T3S3 (LR1121 dual-band)
```

PlatformIO automatically downloads all dependencies (RadioLib, SparkFun u-blox GNSS v3, Adafruit SSD1306).

### Flash

1. Connect the board via USB-C
2. **Connect the antenna first**
3. Flash:
   ```bash
   pio run -e t3s3 --target upload
   ```
4. If upload fails on T3S3: hold BOOT, press RESET, release BOOT (enters download mode)
5. Monitor:
   ```bash
   pio device monitor -b 115200
   ```

### Expected Boot Output (v1.5.1)

```
========== SENTRY-RF v1.5.1 ==========
[BOOT] Boot #1
========================
 SENTRY-RF v1.5.1
 Build: Apr  6 2026
 Board: LilyGo T3S3
 Mode:  FreeRTOS dual-core
========================
[OLED] OK
[SCAN] FSK mode ready, 350 bins, 860.0-930.0 MHz
[GPS] Connected at 38400 baud — configuring
[WIFI] Promiscuous scanner active — channel hopping
[COMPASS] Not detected — continuing without compass
[INIT] FreeRTOS tasks launched — LoRa:Core1, GPS+WiFi:Core0
...
[WARMUP] Complete after 20 cycles (51s). 12 ambient taps recorded:
  - 923.1 MHz / SF9 (first seen cycle 2)
  - 917.6 MHz / SF6 (first seen cycle 3)
  ...
[CAD] cycle=21 conf=0 taps=1 div=1 persDiv=0 vel=0 sustainedCycles=0 score=5
```

### First Boot Checklist

- [ ] SENTRY-RF splash logo appears on OLED (~8-10s boot with GPS)
- [ ] Dashboard shows threat level, mini spectrum bars, battery %
- [ ] RF Scan screen shows spectrum with real ambient peaks (not flat)
- [ ] GPS screen shows fix status or "NO GPS" if not connected
- [ ] Serial shows `[WARMUP] Complete` after ~50 seconds
- [ ] After warmup: `persDiv=0`, threat stays CLEAR
- [ ] Short button press cycles through 6 screens
- [ ] 1-second hold: acknowledge active alert
- [ ] 3-second hold: mute buzzer for 5 minutes
- [ ] Buzzer chirps on boot (self-test: 1000/1500/2000 Hz ascending)
- [ ] LED off at CLEAR, blinks on WARNING, solid on CRITICAL

### Serial Output Key

After warmup, the `[CAD]` line is the primary detection status:

```
[CAD] cycle=N conf=N taps=N div=N persDiv=N vel=N sustainedCycles=N score=N
```

| Field | Meaning |
|-------|---------|
| `cycle` | Scan cycle number since boot |
| `conf` | Confirmed CAD taps (3+ consecutive hits on same frequency) |
| `taps` | Total active taps in the tap list |
| `div` | Raw diversity: distinct frequencies with CAD hits in 3s window |
| `persDiv` | Persistent diversity: `div` when sustained >= 3 cycles, else 0 |
| `vel` | Diversity velocity: new persistent frequencies per window |
| `sustainedCycles` | Consecutive cycles with raw div >= 3 |
| `score` | Weighted confidence score (ADVISORY >= 8, WARNING >= 24, CRITICAL >= 40) |

**Baseline (no drone):** `persDiv=0, sustainedCycles=0, score=5`
**Drone detected:** `persDiv=25-32, sustainedCycles=3+, score=100`

---

## Board-Specific Notes

### LilyGo T3S3
- Native USB CDC (`-DARDUINO_USB_CDC_ON_BOOT=1`). Opening serial monitor may trigger a reboot.
- I2C: SDA=18, SCL=17 (OLED). SDA=21, SCL=10 (compass on Wire1).
- Requires `board_build.flash_size = 4MB` in platformio.ini or it boot-loops.

### Heltec WiFi LoRa 32 V3
- Uses CP2102 USB bridge (no reboot on serial connect).
- I2C: SDA=17, SCL=18 (**swapped from T3S3**).
- OLED requires Vext (GPIO 36) LOW + reset pulse on GPIO 21 (handled by firmware).
- Must call `radio.setTCXO(1.8)` before `radio.begin()` (handled by firmware).
- No SD card slot. Logs to SPIFFS (`/log.csv`, rotates at 100KB).
- No QWIIC connector. No compass support.

---

## Project Status

**Current version: v1.5.1** -- AAD detection engine validated, LED alerts active, fast response tuning complete.

Validated against [JUH-MAK-IN JAMMER](https://github.com/Seaforged/Juh-Mak-In-Jammer) test suite (ELRS FHSS, band sweep, Remote ID).

See [SENTRY-RF Known Issues Tracker](SENTRY-RF_Known_Issues_Tracker.md) for current limitations and roadmap.
