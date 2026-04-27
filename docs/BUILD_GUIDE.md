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

Heltec V3 supports sub-GHz CAD scanning and FHSS detection but lacks 2.4 GHz, GPS hardware, and the WiFi skip-list learning feature (which depends on a 3D GPS fix). Proximity-CRITICAL escalation is unavailable on this board. The Heltec V3 also has no SD card slot (logs to SPIFFS at `/log.csv`, rotates at 100KB), no QWIIC connector for an external compass, and requires a 1.8V TCXO init step (handled automatically by the firmware).

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

### GPS Module (Heltec V3) — advanced / unsupported configuration

The Heltec V3 board does not ship with a GPS module. The firmware
defines GPS UART pins for this board (`PIN_GPS_RX = 46`,
`PIN_GPS_TX = 45` in `include/board_config.h`), so an external
u-blox M10 GPS module can be wired in if you want proximity-aware
features on Heltec hardware. This is an advanced configuration —
not validated as part of standard testing — and you're on your
own for the physical mounting.

```
GPS Module          Heltec V3
----------          ---------
TX  -----------------> GPIO 46 (UART1 RX)
RX  <----------------- GPIO 45 (UART1 TX)
VCC -----------------> 3.3V
GND -----------------> GND
```

If you don't add an external GPS module, GPS-aware features
(proximity-CRITICAL escalation, WiFi skip-list location
invalidation) fail closed on Heltec V3. All other detection paths
continue to function.

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

### Expected Boot Output (v2.0.0)

```
========== SENTRY-RF v2.0.0 ==========
[BOOT] Boot #N
[ENV-MODE] loaded from NVS: SUBURBAN (tap=10.0 skip=180000ms)
========================
 SENTRY-RF v2.0.0
 Build: <date>
 Board: <LilyGo T3S3 | T3S3 LR1121 | Heltec V3>
 Mode:  FreeRTOS dual-core
========================
[OLED] OK
[SCAN] GFSK mode ready (LR1121), 350 bins, 860.0-930.0 MHz
[SELFTEST] Radio: OK (api_ok=1 range=N.NdB)
[SELFTEST] Antenna: OK
[SELFTEST] GPS: Acquiring... (async)
[GPS] Connected at 38400 baud — configuring
[WIFI] Promiscuous scanner active — channel hopping
[BLE] Scanner init — window=50ms interval=500ms (passive)
[COMPASS] Not detected — continuing without compass
[INIT] FreeRTOS tasks launched
...
[WARMUP] progressive ambient tag: 902.3MHz
[WARMUP] progressive ambient tag: 906.5MHz
...
[CAD] cycle=N subConf=N sub24Conf=N fastConf=N taps=N div=N persDiv=N vel=N sustainedCycles=N score=N fast=N confirm=N anchor=<freq>MHz
```

The exact `Board:` line depends on which build target you flashed.
See [`FLASHING.md`](FLASHING.md#important-match-the-firmware-target-to-your-board-variant)
for the target-vs-board matching rules.

### First Boot Checklist

- [ ] SENTRY-RF splash logo appears on OLED (~8-10s boot with GPS)
- [ ] Dashboard shows threat level, mini spectrum bars, battery %
- [ ] Spectrum screen shows ambient RF activity (not flat)
- [ ] GPS screen shows fix status or "NO GPS" if not connected
- [ ] Serial shows `[WARMUP] progressive ambient tag` lines, completing after ~50 seconds
- [ ] After warmup: threat stays at CLEAR (or drifts into ADVISORY in dense RF — see [`KNOWN_ISSUES.md`](KNOWN_ISSUES.md))
- [ ] Short button press cycles through 8 screens (9 on LR1121 — adds 2.4 GHz Spectrum24 page)
- [ ] 1-second hold: acknowledge active alert
- [ ] 3-second hold on Env Mode page: cycles environment mode
- [ ] 3-second hold on any other page: toggles global mute (5-minute silence)
- [ ] Buzzer chirps on boot self-test
- [ ] LED off at CLEAR, blinks on WARNING, solid on CRITICAL

For full operational details (screen-by-screen field reference,
threat-level interpretation, env-mode switching), see
[`USER_GUIDE.md`](USER_GUIDE.md).

### Serial Output Key

After warmup, the `[CAD]` line is the primary detection status:

```
[CAD] cycle=N subConf=N sub24Conf=N fastConf=N taps=N div=N persDiv=N vel=N sustainedCycles=N score=N fast=N confirm=N anchor=<freq>MHz
```

| Field | Meaning |
|-------|---------|
| `cycle` | Scan cycle number since boot |
| `subConf` | Sub-GHz CAD-confirmed corroborator score |
| `sub24Conf` | 2.4 GHz CAD-confirmed corroborator score (LR1121 only; 0 elsewhere) |
| `fastConf` | Fast-path detection score |
| `taps` | Total active CAD taps in the tap list |
| `div` | Raw diversity: distinct frequencies with CAD hits in 3s window |
| `persDiv` | Persistent diversity: `div` when sustained ≥ 5 cycles, else 0 |
| `vel` | Diversity velocity: new persistent frequencies per window |
| `sustainedCycles` | Consecutive cycles with raw `div` ≥ 3 |
| `score` | Composite confidence score |
| `fast` | Fast-path component of score |
| `confirm` | Corroborator-confirmed component (the load-bearing input to the WARNING gate; threshold = 15) |
| `anchor` | Frequency the candidate is currently locked to |

The threat level is gated on `confirm` (the corroborator-side score)
rather than the composite `score`. WARNING fires when `fast >= 40`
AND `confirm >= 15` are sustained across multiple cycles. For
operator-facing threat-level interpretation, see
[`USER_GUIDE.md`](USER_GUIDE.md#threat-levels).

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

**Current version: v2.0.0** — Tier 1 detection pipeline complete and field-ready. The detection engine has been validated against the [JUH-MAK-IN JAMMER](https://github.com/Seaforged/Juh-Mak-In-Jammer) test suite (ELRS FHSS, generic LoRa, Remote ID over WiFi/BLE, LoRaWAN infrastructure suppression).

**Note:** GPS status prints every 5 seconds (rate-limited to prevent serial buffer overflow during long monitoring sessions).

See [`KNOWN_ISSUES.md`](KNOWN_ISSUES.md) for current limitations and operational watch items, including detection-scope gaps (DJI OcuSync OFDM, 5.8 GHz) that are out of Tier 1 scope.
