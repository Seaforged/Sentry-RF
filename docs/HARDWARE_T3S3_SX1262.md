# Hardware reference — LilyGo T3S3 V1.3 with SX1262

This is the single-band variant of the LilyGo T3S3, with a
**Semtech SX1262** radio module. The SX1262 covers sub-GHz only
(868/915 MHz). The dual-band 2.4 GHz path is **not available** on
this variant — for cross-band detection you need the LR1121 sister
board.

This doc is a hardware-side reference: pinout, antenna identification,
known quirks. It does not cover firmware operation — see
[FLASHING.md](FLASHING.md) and [`USER_GUIDE.md`](USER_GUIDE.md) for that.

For the LR1121-equipped sister board, see
[HARDWARE_T3S3_LR1121.md](HARDWARE_T3S3_LR1121.md). The two boards
share most of their pin layout but differ on the radio module and
the GPS UART order.

---

## Board overview

- **MCU:** Espressif ESP32-S3 (dual-core Xtensa LX7, 8 MB PSRAM
  populated on this variant).
- **Radio:** Semtech SX1262 sub-GHz LoRa transceiver. 860–930 MHz.
- **GPS:** u-blox SAM-M8Q (or compatible M10) on UART1.
- **Display:** SSD1306 0.96" 128×64 OLED, I2C.
- **Storage:** microSD slot (SPI), separate bus from the radio.
- **Buzzer:** PWM-capable piezo on GPIO 16.
- **Power:** USB-C charging + 18650 / Li-ion connector. Battery is
  the primary power source; USB is for charging + serial access.
- Available from LilyGo and third-party retailers (Amazon, AliExpress
  marketplaces). Confirm the radio module before purchase — silkscreen
  shows `SX1262` near the daughterboard for the SX1262 variant.

---

## Pinout summary

All pin assignments come from `include/board_config.h` under the
`#ifdef BOARD_T3S3` block. Source is authoritative; this table is
for reference.

| Subsystem | Signal | GPIO | Notes |
|---|---|---|---|
| LoRa SPI | SCK | 5 | shared SPI bus on the LoRa daughterboard |
| LoRa SPI | MISO | 3 | |
| LoRa SPI | MOSI | 6 | |
| LoRa SPI | CS | 7 | active LOW |
| LoRa SPI | RST | 8 | active LOW pulse on init |
| LoRa SPI | BUSY | 34 | input — chip ready signal |
| LoRa SPI | DIO1 (IRQ) | **33** | SX1262 uses DIO1 as the IRQ pin (LR1121 uses DIO9 on GPIO 36) |
| LoRa | TCXO | — | T3S3 SX1262 has **no TCXO** (`HAS_TCXO = false`); RC oscillator only |
| OLED I2C | SDA | 18 | I2C bus 0 |
| OLED I2C | SCL | 17 | |
| OLED I2C | RST | — | T3S3 has no OLED reset pin |
| OLED I2C | Vext | — | T3S3 has no Vext control |
| Compass I2C | SDA | 21 | I2C bus 1 (Wire1), QWIIC connector |
| Compass I2C | SCL | 10 | |
| GPS UART | RX (ESP listens) | **44** | order is reversed vs the LR1121 board — see Hardware quirks below |
| GPS UART | TX (ESP sends) | **43** | |
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

The SX1262 T3S3 has two U.FL connectors:

| Position relative to radio module | Carries | Antenna required |
|---|---|---|
| On the LoRa daughterboard, next to the SX1262 chip | sub-GHz TX/RX (868/915 MHz) | 868/915 MHz dipole or PCB antenna, region-appropriate |
| On the GPS daughter (next to the SAM-M8Q chip) | GPS L1 (1575.42 MHz) | passive GPS patch antenna |

Unlike the LR1121 variant, there is **no 2.4 GHz U.FL** on the
SX1262 board — the chip simply doesn't have a 2.4 GHz radio path.
WiFi/BLE on the ESP32-S3 main chip is handled by the chip's internal
antenna (or external if the board has been modified).

The two U.FL connectors are physically distant enough to be hard to
mistake — the LoRa one sits directly next to the SX1262 module on the
daughterboard, the GPS one sits next to the GPS module on the main
PCB.

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

### No TCXO — RC oscillator only

`HAS_TCXO = false` on this variant. The SX1262's internal RC
oscillator handles frequency reference. Some other SX1262 boards
(Heltec V3, for example) do have a 1.8 V external TCXO; this
specific T3S3 production run does not. Firmware does not call
`setTCXO()` on this target. If you re-implement radio init, do not
introduce a TCXO setup call — it will fail or misconfigure.

### GPS UART pin order is swapped vs the LR1121 sister board

On the SX1262 T3S3:
- `PIN_GPS_RX = 44` (ESP listens; GPS TX → 44)
- `PIN_GPS_TX = 43` (ESP sends; GPS RX ← 43)

On the LR1121 T3S3:
- `PIN_GPS_RX = 43` (ESP listens)
- `PIN_GPS_TX = 44` (ESP sends)

The pins are physically the same, but the GPS daughter's cable
orientation differs between the two production runs. Firmware handles
this transparently via the `BOARD_*` build flag. If you wire a GPS
module manually, you need to know which board variant you have.

### Compass on Wire1 (I2C bus 1)

Both T3S3 variants expose GPIO 21/10 on a QWIIC connector for an
external I2C compass (QMC5883L or similar). The firmware sets
`HAS_COMPASS = true` and probes for the chip on every boot; absence
is logged and continues without compass.

### Antenna self-test threshold

The antenna boot self-test on the SX1262 variant uses a threshold of
**-100 dBm**. The SX1262 has typical ambient 915 MHz pickup above
-95 dBm in any populated area; bare SMA stubs top out around -105
to -110 dBm even near strong emitters. -100 gives ~8 dB margin below
worst-case stub pickup, which is enough to detect a missing or
broken antenna without false-failing in quiet RF.

### Capability flags

Set in `board_config.h` for this variant:

```c
HAS_PSRAM     = true
HAS_SD_CARD   = true
HAS_COMPASS   = true
HAS_TCXO      = false   ← key difference vs LR1121 + Heltec
HAS_LR1121    = false
HAS_24GHZ     = false   ← no 2.4 GHz radio path
WIFI_ENABLED  = true
HAS_BLE_RID   = 1       (#define)
HAS_BUZZER    = true
HAS_OLED_RST  = false
HAS_OLED_VEXT = false
```

`HAS_24GHZ = false` disables the 2.4 GHz scanner code paths
(`extractPeaks24()` and the `cad24` corroborator). Cross-band attach
in `detection_engine.cpp` is gated on this flag; on this target it
silently no-ops, and the FSM relies on sub-GHz CAD evidence only.

---

## Optional accessories

### Compass

A 3-axis I2C magnetometer plugged into the QWIIC connector
(GPIO 21 SDA / GPIO 10 SCL). Firmware probes for a QMC5883L by
default; HMC5883L is also wired-compatible but the driver currently
expects QMC. The compass adds a heading reading to the GPS fix and
populates the OLED's compass arc on the threat screen.

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
weather-resistant enclosure with U.FL feedthroughs for the antenna
is recommended; the GPS antenna can be mounted external to the case
via the U.FL cable.

---

## See also

- [FLASHING.md](FLASHING.md) — getting firmware onto the board
- [HARDWARE_T3S3_LR1121.md](HARDWARE_T3S3_LR1121.md) — the LR1121
  dual-band sister board
- `include/board_config.h` — authoritative pin/capability source
- [`USER_GUIDE.md`](USER_GUIDE.md) — operating the device
- [`KNOWN_ISSUES.md`](KNOWN_ISSUES.md) — limitations and watch items
