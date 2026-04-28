# Flashing SENTRY-RF

This guide gets SENTRY-RF firmware onto a supported board. It assumes
you're comfortable with a terminal and have flashed an ESP32 board
before. If something goes sideways, jump to
[Troubleshooting](#troubleshooting) — most issues are documented.

## Quick-start

```bash
# Prereq: PlatformIO Core (or PlatformIO IDE in VS Code)
git clone https://github.com/Seaforged/Sentry-RF.git
cd Sentry-RF
pio device list                                        # find your port
pio run -e <target> -t upload --upload-port <port>     # see targets below
pio device monitor -p <port> -b 115200                 # verify boot banner
```

If the boot banner's `Board:` line matches your physical hardware, the
flash is good. If not, see [Important: match the firmware target to
your board variant](#important-match-the-firmware-target-to-your-board-variant).

---

## Hardware prerequisites

SENTRY-RF supports three board variants, each with its own PlatformIO
target. Pick the variant you have, flash the matching target, and use
the antennas it expects.

| Board variant | Target | Sub-GHz | 2.4 GHz | GPS | Reference |
|---|---|---|---|---|---|
| LilyGo T3S3 V1.3 with **SX1262** | `t3s3` | yes (single-band) | no | yes (u-blox) | [`HARDWARE_T3S3_SX1262.md`](HARDWARE_T3S3_SX1262.md) |
| LilyGo T3S3 V1.3 with **LR1121** | `t3s3_lr1121` | yes | yes (dual-band) | yes (u-blox) | [`HARDWARE_T3S3_LR1121.md`](HARDWARE_T3S3_LR1121.md) |
| Heltec WiFi LoRa 32 V3 | `heltec_v3` | yes | no | **no** | external (Heltec docs) |

The two T3S3 variants are physically similar but ship with different
radio chips on the daughterboard. **You cannot tell them apart by
looking at the board outline** — check the silkscreen on the radio
module itself, or read the `Board:` line in the boot banner after
flashing.

### Antennas

- **Sub-GHz:** 868/915 MHz antenna (band per your region's regulatory
  domain). The U.FL connector on the LoRa daughterboard.
- **2.4 GHz** *(LR1121 only)*: a separate 2.4 GHz antenna on the
  second U.FL connector. The LR1121 cannot transmit/receive on
  2.4 GHz without it.
- **GPS** *(T3S3 boards)*: a passive GPS patch antenna on the dedicated
  GPS U.FL connector. Without it, GPS will not acquire a fix and
  proximity-aware features (RID alerting, WiFi skip-list invalidation)
  fall back to fail-closed behavior.
- Heltec V3 has only the sub-GHz radio path; no 2.4 GHz, no GPS.

### Cabling and power

- A USB cable that carries data, not just charge. Many "charge-only"
  cables look identical to data cables and will silently fail to
  enumerate the board's serial port.
- T3S3 boards are battery-powered when a Li-ion is attached; USB
  provides serial + charging. Heltec V3 runs from USB only by default.

---

## Software prerequisites

### PlatformIO Core or IDE

This project builds with PlatformIO. Either flavor works:

- **PlatformIO Core (CLI):**
  ```bash
  pip install platformio
  ```
  Adds `pio` to your `PATH`. Test with `pio --version`.

- **PlatformIO IDE (VS Code extension):**
  Install from the VS Code Marketplace. It bundles Core and exposes
  `pio` in the integrated terminal. The CLI commands below work
  identically inside that terminal.

### USB-to-serial driver

The board exposes a serial port over USB. Driver setup depends on
your OS:

- **Windows:** the T3S3's ESP32-S3 native USB enumerates without
  drivers on Windows 10/11. Older boards (or the Heltec V3 over its
  CP2102 bridge) may need the Silicon Labs CP210x VCP driver. If
  Windows Update doesn't grab it automatically, download from
  Silicon Labs. The CH340 driver from WCH covers boards with that
  bridge instead.
- **macOS:** ESP32-S3 native USB-CDC works built-in on recent macOS.
  Older boards with CP210x require the Silicon Labs driver from
  silabs.com.
- **Linux:** typically just works. If `/dev/ttyACM*` or `/dev/ttyUSB*`
  appears but PlatformIO can't open it, add your user to the
  `dialout` (Debian/Ubuntu) or `uucp` (Arch) group and re-login.

### Optional: pio device monitor

Comes with PlatformIO. Used to read the boot banner and verify the
flash:

```bash
pio device monitor -p <port> -b 115200
```

`Ctrl+C` to exit. Doesn't reset the device on connect (useful for
mid-stream observation).

---

## Important: match the firmware target to your board variant

Read this section before flashing.

The three PlatformIO targets compile to different binaries that drive
different radio chips. Flashing the wrong target to a board produces
a non-obvious failure: the firmware boots, the OLED initializes, but
radio init fails because the firmware tries to talk to a chip that
isn't there. The OLED shows:

```
FATAL ERROR
Scanner init
Radio chip not found
```

…and the serial console emits:

```
[INIT] Scanner init failed: -2
```

(`-2` is RadioLib's `RADIOLIB_ERR_CHIP_NOT_FOUND`.)

**This is not a hardware failure. It is a target mismatch.** A
two-hour debugging arc on this exact failure is part of why this
section exists.

### How to check

After flashing, the boot banner emits a `Board:` line. The expected
value depends on the target you flashed:

| Target | Boot banner `Board:` line |
|---|---|
| `t3s3` | `Board: LilyGo T3S3` |
| `t3s3_lr1121` | `Board: T3S3 LR1121` |
| `heltec_v3` | `Board: Heltec V3` |

If the banner shows the *wrong* target's string for your physical
hardware, re-flash with the correct `-e` flag. If the banner shows
the right string but radio init still fails, see
[Troubleshooting](#troubleshooting).

---

## First-time flash

### Find your port

```bash
pio device list
```

Note the port for your board. On Windows it's a `COMxx`; on macOS/Linux
it's `/dev/cu.usbmodemXXXX` or `/dev/ttyACM0`.

### t3s3 — LilyGo T3S3 V1.3 with SX1262

Single-band sub-GHz radio. Use this target only if your board has the
**SX1262** radio module. If you're unsure which radio your board has,
check the silkscreen on the LoRa daughterboard. If you can't tell,
you can flash either target and check the boot banner — a target
mismatch produces the `Scanner init failed: -2` error and is
non-destructive.

```bash
pio run -e t3s3 -t upload --upload-port <port>
pio device monitor -p <port> -b 115200
```

Expected boot banner (excerpt):

```
========== SENTRY-RF v2.0.0 ==========
[BOOT] Boot #N
[ENV-MODE] loaded from NVS: SUBURBAN (tap=10.0 skip=180000ms)
========================
 SENTRY-RF v2.0.0
 Build: <date>
 Board: LilyGo T3S3
 Mode:  FreeRTOS dual-core
========================
[OLED] OK
[SELFTEST] Radio: OK (api_ok=1 range=N.NdB)
[SELFTEST] Antenna: OK
[SELFTEST] GPS: Acquiring... (async)
```

If `Board: LilyGo T3S3` is present and all three `[SELFTEST]` lines
are `OK` (GPS will say `Acquiring...` for up to ~120 s; that's
expected), you're done.

### t3s3_lr1121 — LilyGo T3S3 V1.3 with LR1121

Dual-band radio: sub-GHz + 2.4 GHz on the same chip. Use this target
only if your board has the **LR1121** radio module.

```bash
pio run -e t3s3_lr1121 -t upload --upload-port <port>
pio device monitor -p <port> -b 115200
```

Expected boot banner (excerpt):

```
========== SENTRY-RF v2.0.0 ==========
[BOOT] Boot #N
[ENV-MODE] loaded from NVS: SUBURBAN (tap=10.0 skip=180000ms)
========================
 SENTRY-RF v2.0.0
 Build: <date>
 Board: T3S3 LR1121
 Mode:  FreeRTOS dual-core
========================
[OLED] OK
[SCAN] GFSK mode ready (LR1121), 350 bins, 860.0-930.0 MHz
[SELFTEST] Radio: OK (api_ok=1 range=N.NdB)
[SELFTEST] Antenna: OK
[SELFTEST] GPS: Acquiring... (async)
```

The `[SCAN] GFSK mode ready (LR1121), 350 bins, 860.0-930.0 MHz` line
is unique to the LR1121 build and confirms the dual-band scanner
initialized.

### heltec_v3 — Heltec WiFi LoRa 32 V3

Sub-GHz only, no GPS. Use this target on the Heltec WiFi LoRa 32 V3
board. Heltec V3 typically uses a CP2102 USB-to-serial bridge, but
specific batches may vary — check your board's silkscreen or vendor
documentation if the default driver doesn't work.

```bash
pio run -e heltec_v3 -t upload --upload-port <port>
pio device monitor -p <port> -b 115200
```

Expected boot banner (excerpt):

```
========== SENTRY-RF v2.0.0 ==========
[BOOT] Boot #N
[ENV-MODE] loaded from NVS: SUBURBAN (tap=10.0 skip=180000ms)
========================
 SENTRY-RF v2.0.0
 Build: <date>
 Board: Heltec V3
 Mode:  FreeRTOS dual-core
========================
[OLED] OK
[SELFTEST] Radio: OK (api_ok=1 range=N.NdB)
[SELFTEST] Antenna: OK
```

The Heltec V3 has no GPS hardware, so no `[SELFTEST] GPS:` line will
appear. GPS-aware features degrade gracefully on this target (proximity
gates fail closed, behaving as if GPS were continuously not-acquired).

---

## Verifying the flash

If you didn't open the monitor as part of flashing, do it now:

```bash
pio device monitor -p <port> -b 115200
```

Inside the monitor, check four things in order:

1. **Boot banner emits.** You should see the `========== SENTRY-RF v2.0.0
   ==========` header within a few seconds of pressing reset (or
   replugging USB). If nothing appears, the board may not have reset
   on monitor open — press the physical RESET button.
2. **`Board:` line matches your hardware.** See the table in
   [Important: match the firmware target to your board variant](#important-match-the-firmware-target-to-your-board-variant).
3. **`[SELFTEST] Radio: OK`** and **`[SELFTEST] Antenna: OK`** appear.
   If either fails, see Troubleshooting.
4. **GPS** *(T3S3 only)*: `[SELFTEST] GPS: Acquiring... (async)`
   appears at boot, followed by either `[SELFTEST] GPS: OK (N SVs)`
   when a 3D fix is acquired (typically 30 s outdoors, longer indoors)
   or `[SELFTEST] GPS: NO FIX after 120s` if the GPS module never
   reaches a 3D fix.

`Ctrl+C` exits the monitor.

---

## Troubleshooting

### `Couldn't find a board on the selected port` / 1200 bps reset fails

The T3S3's ESP32-S3 native USB-CDC reset is not always reliable.
Manual download mode is the workaround:

1. Hold `BOOT` on the board.
2. Tap `RESET` (or `RST`) once and release.
3. Release `BOOT`.
4. Re-run the upload command immediately:
   ```bash
   pio run -e <target> -t upload --upload-port <port>
   ```

The port may briefly disappear and re-enumerate; that's normal.

### Boot banner shows wrong `Board:` line

Target mismatch — see
[Important: match the firmware target to your board variant](#important-match-the-firmware-target-to-your-board-variant).
Re-flash with the correct `-e` target.

### `Scanner init failed: -2` / `FATAL ERROR — Radio chip not found`

In order of likelihood:

1. **Target mismatch** (almost always the cause). Verify the `Board:`
   line in the banner against the table above. If you flashed `t3s3`
   on an LR1121 board (or vice versa), the radio chip isn't where the
   firmware expects to find it. Re-flash with the matching target.
2. **Radio module not seated** (rare). After significant physical
   shock or a re-soldered LoRa daughterboard, the SPI bus to the radio
   module may be flaky. Cold-cycle the USB (full unplug, 5 s wait,
   replug). If the issue persists across multiple cold cycles, inspect
   the daughterboard solder joints — the LR1121/SX1262 module mounts
   on a small breakout that occasionally lifts on one corner.

### GPS never acquires fix

- Confirm the GPS antenna is plugged into the correct U.FL connector
  on the T3S3 board (not the sub-GHz or 2.4 GHz connector — they look
  identical).
- Indoor first-acquisition can take 10+ minutes if the GPS module
  has cached almanac data from a previous fix; a fresh module with
  no almanac history may not acquire indoors at all. Take the unit
  outside for first acquisition.
- Heltec V3 has no GPS hardware. The absence of GPS messages on that
  target is correct.

### USB port not enumerating after upload

T3S3 native USB-CDC sometimes loses enumeration after a firmware
crash or after a long capture session under load. Cold-cycle: full
USB unplug, wait 5 s, replug. The port reappears under a fresh
enumeration. This recovers cleanly without a re-flash.

### PlatformIO can't find the port (Linux/macOS)

If `pio device list` shows no ports but the board is plugged in:

- **Linux:** ensure you're in the `dialout` (or distribution-specific)
  group: `groups | grep dialout`. If not, `sudo usermod -aG dialout
  $USER`, then log out and back in.
- **macOS:** check System Settings → Privacy & Security for any
  blocked driver. CP210x driver requires kernel-extension allow on
  some macOS versions.

### Diagnostic build flags

The firmware exposes optional build flags for diagnostic builds. These
are off by default in production. Use them only if you're debugging a
specific issue and know what you're looking for.

- `ENABLE_ATTACH_TRACE=1`: enables verbose `[ATTACH]` log lines
  showing per-candidate evidence and persistence-factor classification.
  Useful for understanding why a particular signal did or didn't
  escalate. Increases serial log volume substantially.
- `ENABLE_RID_MOCK=1`: enables a synthetic Remote ID test harness
  that emits canned WiFi/BLE RID frames at boot for validating the
  detection path without an external RID emitter. Used by
  `validate_mock_rid.py`.

Build with a flag set:

```bash
PLATFORMIO_BUILD_FLAGS="-DENABLE_ATTACH_TRACE=1" pio run -e <target> -t upload --upload-port <port>
```

After diagnostic work, re-flash production firmware (no flags) so the
unit returns to ship-ready state. See `include/sentry_config.h` for
the full list of build-time options and their default values.

---

## Where to go next

Once flashed, see [`USER_GUIDE.md`](USER_GUIDE.md) for operating the
device — what the OLED screens mean, how to switch environment
modes, how to interpret threat alerts, and how to read the Remote ID
output.

For known limitations and watch items, see
[`KNOWN_ISSUES.md`](KNOWN_ISSUES.md).
