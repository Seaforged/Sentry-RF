# SENTRY-RF OLED Screen Glossary

Reference guide for every data field shown on the OLED screens. Intended for operators, field testers, and new developers who need to understand what the display is telling them at a glance.

**Hardware:** LilyGo T3S3 / Heltec WiFi LoRa 32 V3 with SSD1306 128×64 OLED
**Firmware:** v2.0.0 (Tier 1 ship-ready)
**Navigation:** Short press BOOT button to cycle screens. **8 screens** on SX1262-based boards (T3S3 SX1262, Heltec V3); **9 screens** on the LR1121 board (adds the 2.4 GHz spectrum page). Page dots at the bottom indicate the active screen.

---

## Screen 0 — Dashboard

The at-a-glance summary. Shows a little of everything so you don't have to cycle through other screens during normal operation.

| Field | Example | Meaning |
|---|---|---|
| SENTRY-RF | — | Project name (header, always visible) |
| CLEAR / ADVISORY / WARNING / CRITICAL | CLEAR | Current threat level. CLEAR = no threats detected. ADVISORY = low-confidence detection. WARNING = likely drone activity. CRITICAL = high-confidence drone or active GNSS attack. |
| WiFi [bar chart] | 13 bars | WiFi 2.4 GHz channel activity. 13 bars for channels 1–13. Taller bars = more WiFi management frames captured on that channel in the last second. Log-scaled — a few frames = short bar, dozens = tall bar. Gives quick awareness of your local 2.4 GHz environment. |
| 3D / 2D / -- | 3D | GPS fix type. 3D = full position + altitude (4+ satellites). 2D = position only (3 satellites). "--" = no GPS fix. |
| nnSV | 14SV | Number of satellites used in the GPS position solution. 4 = minimum for 3D fix. 8+ = good. 12+ = excellent. |
| J:n / J:-- | J:0 | GPS jamming indicator (0–255 scale from the u-blox M10 MON-HW message). 0 = no jamming. Higher = more RF interference near GPS L1 frequencies. "--" = MON-HW data not yet received or stale (older than 5 seconds). |
| S:OK / S:! | S:OK | GPS spoofing status (binary summary). OK = u-blox reports signals look legitimate. ! = spoofing suspected (spoofDetState ≥ 2). |
| -nndB @nnnMHz | -94dB @930MHz | Strongest sub-GHz signal detected this scan cycle — frequency and power level of the peak RSSI reading across 860–930 MHz. |
| RF: quiet | — | Displayed instead of the peak when no sub-GHz signals are above the noise floor. |
| Bat:nn% | Bat:90% | Battery charge percentage from the onboard ADC (GPIO 1). |
| WiFi:SCAN | — | WiFi promiscuous scanner status. SCAN = actively monitoring for Remote ID beacons and drone MAC addresses. |

---

## Screen 1 — Sub-GHz Spectrum (RF 860–930 MHz)

Full-width spectrum analyzer view of the sub-GHz ISM band where drone control links operate.

| Field | Example | Meaning |
|---|---|---|
| RF 860–930MHz | — | The frequency range being swept. Covers both US 915 MHz and EU 868 MHz ISM bands. |
| [bar chart] | — | RSSI (signal strength) at each frequency bin across the range. ~350 bins, each ~0.2 MHz wide. Taller bars = stronger signals at that frequency. This is what the radio "hears" in real time. |
| Pk:nnn.n -nndBm | Pk:929.6 -82dBm | The single strongest signal found in the current sweep — its frequency and power level. |

---

## Screen 2 — GPS

Detailed GPS position and accuracy information.

| Field | Example | Meaning |
|---|---|---|
| 3D nnSV pDOP:n.n | 3D 13SV pDOP:2.9 | Fix type, satellite count, and Position Dilution of Precision. |
| pDOP | 2.9 | Position Dilution of Precision — measures how well the satellites are spread across the sky. Lower = better geometry = more accurate position. <2.0 = excellent. 2–5 = good. >5 = degraded (satellites clustered in one part of the sky). |
| nn.nnnnn | 36.86735 | Latitude in decimal degrees. Positive = north, negative = south. |
| -nn.nnnnn | -76.02156 | Longitude in decimal degrees. Positive = east, negative = west. |
| Alt:nnm | Alt:11m | Altitude above mean sea level in meters. |
| hAcc:nm | hAcc:2m | Horizontal accuracy estimate in meters — the GPS module's confidence radius. "I am within 2 meters of this position." 1–3m = excellent for a small module. 5–10m = typical outdoors. >20m = degraded. |

---

## Screen 3 — GNSS Integrity

Monitors whether GPS signals are being jammed (overwhelmed with noise) or spoofed (replaced with fake signals).

| Field | Example | Meaning |
|---|---|---|
| GNSS INTEGRITY | — | Screen title. |
| Jam:--- / Jam:OK / Jam:WARN / Jam:CRIT | Jam:--- | Jamming state category from u-blox MON-HW. OK = no jamming. WARN = interference detected. CRIT = heavy jamming. "---" = module reports state as "unknown/disabled" (common default on u-blox M10). |
| JamI:n | JamI:0 | Raw jamming indicator (0–255). The numeric measurement behind the Jam category. More granular. 0 = clean RF environment. 50+ = noticeable. 100+ = significant interference. 200+ = severe jamming. |
| Spoof: Clean / Indicated / Multiple | Spoof: Clean | u-blox spoofing detection state. Clean = signals look real. Indicated = one spoofing indicator is flagging. Multiple = multiple independent indicators flagging (high confidence of spoofing). Indoors, "Indicated" is common due to multipath reflections and is usually a false positive. |
| C/N0sd:n.n | C/N0sd:8.0 | Carrier-to-Noise ratio standard deviation across tracked satellites, in dB-Hz. Measures how *varied* satellite signal strengths are. Real GPS = varied strengths from satellites at different elevations = C/N0sd of 5–10 dB-Hz. Spoofed GPS = all signals from one transmitter at the same power = C/N0sd drops below 2.0 dB-Hz. This is the primary spoofing detection metric. |

---

## Screen 4 — Threat Detail

Expanded view of the current threat assessment with all contributing factors visible.

| Field | Example | Meaning |
|---|---|---|
| THREAT: CLEAR | — | Current threat level (same as Dashboard). Background inverts to white when at WARNING or above for visibility. |
| RF:nnnMHz -nndBm | RF:930MHz -85dBm | Strongest sub-GHz signal this cycle. |
| 3D nnSV J:xx S:xx | 3D 15SV J:-- S:OK | Combined GPS + jamming + spoofing (same meanings as Dashboard). |
| Buzzer: Armed / Active / MUTED / ACK'd / Off | Buzzer: Armed | Current buzzer state. Armed = ready to alert if threat reaches WARNING. Active = buzzer is currently sounding a tone pattern. MUTED = operator pressed button to silence alerts temporarily. ACK'd = operator acknowledged the alert. Off = no buzzer hardware present. |
| Bearing: nnn° XX / Bearing: -- | Bearing: -- | Direction to the strongest detected signal. Requires compass hardware (QMC5883L) connected via I2C. Shows degrees from north + cardinal direction (e.g., "045° NE") when available. "--" = compass not connected. |

---

## Screen 5 — System

Hardware health and firmware diagnostics.

| Field | Example | Meaning |
|---|---|---|
| SYSTEM | — | Screen title. |
| SENTRY-RF v1.6.0 | — | Firmware name and version. Matches the boot banner printed to serial. |
| Up: 0h 21m 9s | — | Time since last boot or reset. Useful for confirming the device hasn't crashed and rebooted. |
| Heap: 193120 | — | Free RAM in bytes. ESP32-S3 has ~327,680 bytes total. If this number drops steadily over hours, there's a memory leak. Stable around 190,000–200,000 is healthy for the current firmware. |
| Buz:Armed | — | Buzzer state (abbreviated, same meanings as Threat screen). |
| Cmp:-- / Cmp:OK | Cmp:-- | Compass (QMC5883L magnetometer) connection status. "--" = not detected on I2C bus. "OK" = responding and providing heading data. |

---

## Screen 6 — Remote ID

Latest decoded ASTM F3411 Remote ID broadcast (over WiFi or BLE). Drone-broadcast UAS-ID, position, and pilot location when reported by a compliant emitter.

| Field | Example | Meaning |
|---|---|---|
| RID | — | Screen title. |
| UAS-ID | XYZ-12345 | Decoded UAS identifier from the RID payload. Empty if no decoded RID has been received this session. |
| Drone position | 36.708 / -76.241 | Drone's broadcast position (lat / lon). Empty until a RID with location data is received. |
| Pilot position | 36.706 / -76.243 | Operator's broadcast position, when the drone reports it. |
| Distance | 412 m | Distance from the SENTRY-RF unit to the drone, calculated when both have valid 3D fixes. Triggers the proximity-CRITICAL escalation when under 500 m. |
| OUI matches | 19 | Count of frames where the RID OUI (`FA:0B:BC` Open Drone ID) was matched but the inner pack didn't decode. Logged at INFO level only. |
| No data | — | Shown when no RID frames have been received this session. |

The screen appears on every board (WiFi RID via the ESP32-S3, BLE RID via the integrated BLE 5.0 stack — both enabled on T3S3 SX1262, T3S3 LR1121, and Heltec V3).

---

## Screen 7 *(LR1121 only)* — 2.4 GHz Spectrum (2400–2500 MHz)

Full-width spectrum analyzer for the 2.4 GHz ISM band. Only available on LR1121 boards (`HAS_24GHZ = true`). Shows where ELRS 2.4 GHz, DJI OcuSync, and WiFi signals are operating.

On SX1262-based boards (T3S3 SX1262, Heltec V3) this screen is absent — Env Mode (below) takes index 7 instead.

| Field | Example | Meaning |
|---|---|---|
| 2.4GHz 2400–2500MHz | — | Frequency range being swept by the LR1121's second radio path. |
| [bar chart] | — | RSSI at each frequency bin across 2400–2500 MHz. Same concept as the sub-GHz spectrum screen. |
| Pk:nnnn -nnndBm | Pk:2411 -104dBm | Peak signal in the 2.4 GHz band. 2411 MHz = WiFi channel 2. Common WiFi channels: 2412 (ch1), 2437 (ch6), 2462 (ch11). ELRS 2.4 GHz operates across 2400–2480 MHz. |

---

## Screen 7 *(SX1262 / Heltec)* / Screen 8 *(LR1121)* — Env Mode

The runtime environment-mode page. Displays the active operator mode and its derived thresholds; long-pressing BOOT here cycles the mode.

| Field | Example | Meaning |
|---|---|---|
| ENV MODE | — | Screen title. |
| `[<mode>]` | `[SUBURBAN]` | Currently active environment mode: `URBAN`, `SUBURBAN` (default), or `RURAL`. Renders in size-2 (large) text, centered. |
| Tap NN dB | Tap 10dB | The peak threshold above noise floor that the RSSI sweep uses to gate CAD probing. URBAN=12, SUBURBAN=10, RURAL=6. |
| Skip N min | Skip 3min | The TTL for newly-learned WiFi skip-list entries. URBAN=5, SUBURBAN=3, RURAL=1. **Note:** TTL change applies to *newly-learned* skip entries only; existing entries keep their captured TTL until expiry. |
| Hold 3+ s to change | — | Operator instruction. The button FSM cycles the mode on any hold ≥ 3000 ms; there is no upper bound on the hold time. |

Mode persists in NVS flash across power cycles. The active mode also appears in the boot banner on serial: `[ENV-MODE] loaded from NVS: <mode> (tap=N.N skip=Nms)`.

---

## General dBm Reference

dBm (decibels relative to one milliwatt) measures signal power. More negative = weaker signal.

| dBm | What it means in SENTRY-RF context |
|---|---|
| -40 to -60 | Very strong — transmitter is very close (within 10–50m) |
| -60 to -80 | Strong — transmitter nearby (50–200m typical for drone TX) |
| -80 to -100 | Moderate — transmitter at range (200–800m) |
| -100 to -110 | Weak — near the edge of SX1262 detection (~1km for 100mW TX) |
| -110 to -120 | Very weak — approaching noise floor on SX1262 |
| -120 to -127 | At or near noise floor on LR1121 (LR1121 can hear quieter signals than SX1262) |

---

## Threat Level Progression

The four threat levels (`CLEAR`, `ADVISORY`, `WARNING`, `CRITICAL`) are gated on the engine's `fast` and `confirm` corroborator scores rather than a single composite score. The load-bearing gate is `confirm ≥ POLICY_WARNING_CONFIRM=15` for WARNING. For the operator-facing description of each level, what triggers escalation, and the buzzer behavior at each level, see [`USER_GUIDE.md`](USER_GUIDE.md#threat-levels).

For the per-`[CAD]` log-line field reference (`fast`, `confirm`, `fastConf`, `subConf`, `sub24Conf`, etc.) used during firmware-side debugging, see [`BUILD_GUIDE.md`](BUILD_GUIDE.md#serial-output-key).

---

*Last updated: 2026-04-27 — v2.0.0 (Tier 1 ship-ready)*
*Review and update this document when new screens or data fields are added.*
