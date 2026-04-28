# Hardware reference — LilyGo T3S3 V1.3 with LR1121

This is the dual-band variant of the LilyGo T3S3, with a **Semtech
LR1121** radio module replacing the SX1262 used on the sister board.
The LR1121 covers sub-GHz (868/915 MHz) **and** 2.4 GHz on a single
chip, which is what enables SENTRY-RF's cross-band detection path on
this target.

This doc is a hardware-side reference: pinout, antenna identification,
known quirks. It does not cover firmware operation — see
[FLASHING.md](FLASHING.md) and [`USER_GUIDE.md`](USER_GUIDE.md) for that.

For the SX1262-equipped sister board, see
[HARDWARE_T3S3_SX1262.md](HARDWARE_T3S3_SX1262.md). The two boards
share most of their pin layout but differ on the radio module, the
GPS UART order, and a TCXO bit.

---

## Board overview

- **MCU:** Espressif ESP32-S3 (dual-core Xtensa LX7, 8 MB PSRAM
  populated on this variant).
- **Radio:** Semtech LR1121 dual-band LoRa transceiver. Sub-GHz path
  on 860–930 MHz; 2.4 GHz path on the integrated PA/LNA.
- **GPS:** u-blox SAM-M8Q (or compatible M10) on UART1.
- **Display:** SSD1306 0.96" 128×64 OLED, I2C.
- **Storage:** microSD slot (SPI), separate bus from the radio.
- **Buzzer:** PWM-capable piezo on GPIO 16.
- **Power:** USB-C charging + 18650 / Li-ion connector. Battery is
  the primary power source; USB is for charging + serial access.
- Available from LilyGo and third-party retailers (Amazon, AliExpress
  marketplaces). Confirm the radio module before purchase — silkscreen
  shows `LR1121MB1xxx` for the LR1121 variant.

---

## Pinout summary

All pin assignments come from `include/board_config.h` under the
`#ifdef BOARD_T3S3_LR1121` block. Source is authoritative; this
table is for reference.

| Subsystem | Signal | GPIO | Notes |
|---|---|---|---|
| LoRa SPI | SCK | 5 | shared SPI bus on the LoRa daughterboard |
| LoRa SPI | MISO | 3 | |
| LoRa SPI | MOSI | 6 | |
| LoRa SPI | CS | 7 | active LOW |
| LoRa SPI | RST | 8 | active LOW pulse on init |
| LoRa SPI | BUSY | 34 | input — chip ready signal |
| LoRa SPI | DIO9 (IRQ) | **36** | LR1121 uses DIO9 as the IRQ pin (not DIO1 like SX1262) |
| LoRa | TCXO | internal | LR1121 has an on-package TCXO; firmware sets it to **3.0 V** at radio init |
| OLED I2C | SDA | 18 | I2C bus 0 |
| OLED I2C | SCL | 17 | |
| OLED I2C | RST | — | T3S3 has no OLED reset pin |
| OLED I2C | Vext | — | T3S3 has no Vext control |
| Compass I2C | SDA | 21 | I2C bus 1 (Wire1), QWIIC connector |
| Compass I2C | SCL | 10 | |
| GPS UART | RX (ESP listens) | **43** | reversed vs the SX1262 board — see Hardware quirks below |
| GPS UART | TX (ESP sends) | **44** | |
| SD SPI | CS | 13 | |
| SD SPI | SCK | 14 | |
| SD SPI | MISO | 2 | |
| SD SPI | MOSI | 11 | |
| Status LED | LED | 37 | active HIGH |
| Buttons | BOOT | 0 | active LOW |
| Buzzer | piezo | 16 | LEDC PWM channel 1, 8-bit resolution |

**OLED I2C address:** `0x3C` (common SSD1306 default).

**GPS baud:** factory 9600 → bumped to 38400 by firmware
(`bumpBaudRate()` in `src/gps_manager.cpp`). HGLRC M100 Mini variant
is 115200 from factory.

---

## Antenna connector identification

The T3S3 has three U.FL connectors on the LR1121 variant:

| Position relative to radio module | Carries | Antenna required |
|---|---|---|
| Closest to LR1121 module, **sub-GHz side** | sub-GHz TX/RX (868/915 MHz) | 868/915 MHz dipole or PCB antenna, region-appropriate |
| Closest to LR1121 module, **2.4 GHz side** | 2.4 GHz TX/RX | 2.4 GHz antenna (Wi-Fi grade) |
| On the GPS daughter (next to the SAM-M8Q chip) | GPS L1 (1575.42 MHz) | passive GPS patch antenna |

The two LoRa-side U.FL connectors are physically adjacent and easy to
swap by mistake. **The LR1121 cannot transmit on a band whose antenna
isn't connected** — running 2.4 GHz with the sub-GHz antenna plugged
in (or vice versa) produces high return-loss and weak emissions, but
no firmware error.

If unsure which U.FL is which, check the silkscreen on the LR1121
daughterboard. Most production runs label them `915` and `2.4G` near
the connector pads.

The GPS U.FL is on a separate area of the board (the GPS module sits
below or alongside the LR1121 daughter) and is harder to mistake
for a radio path.

---

## Hardware quirks

### Native USB-CDC reset can be unreliable

The T3S3 V1.3 uses ESP32-S3's native USB-CDC for serial. Most
PlatformIO uploads work cleanly via the standard 1200 bps reset
pattern, but a fraction of upload attempts fail with
`Couldn't find a board on the selected port`. The workaround is
manual download mode (hold BOOT, tap RESET, release BOOT, retry the
upload). Documented in [FLASHING.md](FLASHING.md) under
Troubleshooting.

### TCXO voltage and radio init order

The LR1121's on-package TCXO requires a 3.0 V supply line. Firmware
sets `LR1121_TCXO_VOLTAGE = 3.0f` (board_config.h) and applies
`setTCXO()` before any radio operation. If you re-implement radio
init outside the existing `cad_scanner.cpp` / `rf_scanner.cpp` paths,
remember to apply the TCXO config first or the radio will silently
fail to lock.

### GPS UART pin order is swapped vs the SX1262 sister board

On the SX1262 T3S3:
- `PIN_GPS_RX = 44` (ESP listens; GPS TX → 44)
- `PIN_GPS_TX = 43` (ESP sends; GPS RX ← 43)

On the LR1121 T3S3:
- `PIN_GPS_RX = 43` (ESP listens)
- `PIN_GPS_TX = 44` (ESP sends)

The pins are physically the same, but the GPS daughter's cable
orientation differs between the two production runs. Firmware handles
this transparently via the `BOARD_*` build flag, but if you wire a GPS
module manually you need to know which board variant you have. Per
the comment in `board_config.h`: "white wire = GPS TX → IO43, yellow
wire = GPS RX → IO44" on this LR1121 variant.

### Compass on Wire1 (I2C bus 1)

Both T3S3 variants expose GPIO 21/10 on a QWIIC connector for an
external I2C compass (QMC5883L or similar). The firmware sets
`HAS_COMPASS = true` and probes for the chip on every boot; absence
is logged and continues without compass.

### Antenna self-test threshold

The antenna boot self-test on the LR1121 variant uses a threshold of
**-122 dBm** (vs -100 dBm on SX1262 boards). The LR1121 has a quieter
noise floor (~-127 dBm) but ambient RF pickup in quiet RF environments
only reaches -118 to -120 dBm at this chipset, so the looser threshold
catches genuinely-dead hardware (which clamps at -127 to -128 dBm)
without false-failing in quiet RF. Trade-off: the LR1121 antenna
self-test cannot reliably detect *loose* connections — only fully
dead ones.

### Capability flags

Set in `board_config.h` for this variant:

```c
HAS_PSRAM     = true
HAS_SD_CARD   = true
HAS_COMPASS   = true
HAS_TCXO      = true
HAS_LR1121    = true
HAS_24GHZ     = true
WIFI_ENABLED  = true
HAS_BLE_RID   = 1   (#define)
HAS_BUZZER    = true
HAS_OLED_RST  = false
HAS_OLED_VEXT = false
```

The `HAS_24GHZ = true` flag enables the 2.4 GHz scanner code path in
`src/cad_scanner.cpp` (the `extractPeaks24()` function and the
cross-band attach logic in `detection_engine.cpp`).

---

## Optional accessories

### Compass

A 3-axis I2C magnetometer plugged into the QWIIC connector
(GPIO 21 SDA / GPIO 10 SCL). Firmware probes for a QMC5883L by
default; HMC5883L is also wired-compatible but the driver currently
expects QMC. The compass adds a heading reading to the GPS fix and
populates the OLED's compass arc on the threat screen.

### 2.4 GHz antenna

Required for the LR1121's 2.4 GHz path. A standard Wi-Fi-grade dipole
or PCB chip antenna with U.FL pigtail works. Any 2.4 GHz / Bluetooth
antenna marketed for ESP32 boards is appropriate.

### Sub-GHz antenna selection

For 915 MHz (FCC US): a 1/4-wave whip (~80 mm) or a 915 MHz dipole.
For 868 MHz (EU): scale up proportionally. Tuned-for-band antennas
outperform "wideband" stubs noticeably in the CAD scanner's
sensitivity.

### Battery

Single-cell 18650 or compatible Li-ion via the JST connector on the
T3S3. The board includes charging circuitry; charge over USB. Battery
voltage is exposed on a sense pin (not currently read by the firmware
in the v3 codebase).

### Case / enclosure

There's no project-supplied enclosure. LilyGo sells a transparent
plastic case for the T3S3 V1.3 form factor. For field deployment, a
weather-resistant enclosure with U.FL feedthroughs for the antennas
is recommended; the GPS antenna can be mounted external to the case
via the U.FL cable.

---

## See also

- [FLASHING.md](FLASHING.md) — getting firmware onto the board
- [HARDWARE_T3S3_SX1262.md](HARDWARE_T3S3_SX1262.md) — the SX1262
  sister board
- `include/board_config.h` — authoritative pin/capability source
- [`USER_GUIDE.md`](USER_GUIDE.md) — operating the device
- [`KNOWN_ISSUES.md`](KNOWN_ISSUES.md) — limitations and watch items
