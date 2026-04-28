# SENTRY-RF user guide

This guide covers operating SENTRY-RF after firmware is flashed.
For getting the firmware onto your board, see
[FLASHING.md](FLASHING.md). For board-specific hardware details,
see the relevant
[HARDWARE_T3S3_LR1121.md](HARDWARE_T3S3_LR1121.md) /
[HARDWARE_T3S3_SX1262.md](HARDWARE_T3S3_SX1262.md) document.

---

## What SENTRY-RF does

SENTRY-RF is a passive radio detector. It listens for RF emissions
typical of small drones — control links (ELRS, Crossfire, LoRa),
2.4 GHz video and telemetry (LR1121 board only), and Remote ID
broadcasts (ASTM F3411 over WiFi and BLE) — and raises a threat
level when it sees evidence of a drone in the area.

It does **not** transmit. It does not jam. It does not classify drone
type beyond what's visible in the RF emissions. It is a passive
sensor that gives an operator situational awareness.

**Read [What SENTRY-RF can and can't detect](#what-sentry-rf-can-and-cant-detect) before relying on it for any safety-of-life decision.** This guide is honest about the gaps.

---

## Power-on and self-test

### What you should see at boot

Power the device on (USB or battery). Within ~2 seconds the OLED
displays a brief banner, then the device runs three self-tests in
sequence. On the serial console, you'll see lines like:

```
========== SENTRY-RF v2.0.0 ==========
[BOOT] Boot #N
[ENV-MODE] loaded from NVS: SUBURBAN (tap=10.0 skip=180000ms)
========================
 SENTRY-RF v2.0.0
 Build: <date>
 Board: <your-board-string>
 Mode:  FreeRTOS dual-core
========================
[OLED] OK
[SELFTEST] Radio: OK (api_ok=1 range=N.NdB)
[SELFTEST] Antenna: OK
[SELFTEST] GPS: Acquiring... (async)
```

The four green-flag indicators are:
- **`[OLED] OK`** — display initialized
- **`[SELFTEST] Radio: OK`** — radio chip responsive
- **`[SELFTEST] Antenna: OK`** — antenna self-test reads ambient RF
  above the noise-only threshold, indicating a connected antenna
- **`[SELFTEST] GPS: Acquiring... (async)`** — GPS module talking,
  fix not yet acquired

### After ~30–120 seconds (T3S3 only)

The GPS reports either `[SELFTEST] GPS: OK (N SVs)` once a 3D fix
is acquired, or `[SELFTEST] GPS: NO FIX after 120s` if it can't
acquire. Indoor reception is typically poor — see
[GPS and positioning](#gps-and-positioning) below.

The Heltec V3 has no GPS hardware; the GPS line never appears on
that target.

### After ~50–60 seconds — ambient warmup

The detection engine spends the first ~50 seconds at boot
characterizing the local RF environment, tagging persistent
emitters as "ambient" so they don't false-trigger detection.
You'll see lines like:

```
[WARMUP] progressive ambient tag: 902.3MHz
[WARMUP] progressive ambient tag: 906.5MHz
...
```

This is normal. Once warmup completes, the device is fully
operational.

### If something fails

If the OLED shows `FATAL ERROR`, see the
[Troubleshooting section in FLASHING.md](FLASHING.md#troubleshooting).
The most common cause is a target/board mismatch.

If the antenna self-test fails (`[SELFTEST] Antenna: FAIL`),
confirm the antenna is connected and that the U.FL connector is
seated. The threshold is set to detect a missing antenna while
passing a connected one — see your board's HARDWARE doc for the
specific value.

---

## The OLED screens

SENTRY-RF cycles through several status screens. Press the **BOOT
button** briefly to advance to the next screen.

### Dashboard *(first / default)*

The summary screen. Shows the current threat level, active mode,
GPS status, and recent activity at a glance.

### Spectrum

A real-time view of the sub-GHz RSSI sweep. The y-axis is signal
strength; the x-axis is frequency across the 860–930 MHz band.
Useful for spotting strong nearby emitters.

### GPS

Latitude, longitude, altitude, satellite count, and fix type.
"3D fix" with at least 5 satellites is considered solid for
proximity calculations.

### Integrity

GNSS integrity indicators: jamming detection level, spoofing
detection level, and C/N0 standard deviation across satellites.
The values come from the u-blox module's own integrity outputs;
high jam-indicator values suggest an external GNSS jammer is
active.

### Threat

The current threat detail: the anchor frequency, modulation type,
threat level, and how long the candidate has been tracked.
Compass heading (if a compass is connected) is shown as an arc
toward the source — note that anchor frequency is approximate
in noisy RF environments and shouldn't be treated as precise
emitter localization (see
[Threat levels](#threat-levels) below).

### System

Boot count, uptime, free heap, and which scanner subsystems are
active. Useful for triage if something seems off.

### RID

The latest decoded Remote ID broadcast. Shows the drone's
broadcast UAS-ID, drone position (if reported), pilot position
(if reported), and proximity to the SENTRY-RF unit. If only
undecoded OUI matches are visible (no decoded ID), see
[Remote ID detection](#remote-id-detection) below.

### Spectrum24 *(LR1121 boards only)*

A 2.4 GHz RSSI sweep across the WiFi channel band. Shows nearby
2.4 GHz emitters — WiFi access points, BLE devices, drone control
links operating in the 2.4 GHz band.

### Env Mode

Shows the active environment mode (URBAN / SUBURBAN / RURAL) and
the threshold values it implies. The bottom of the screen shows
"Hold 3-5s to change" — long-pressing BOOT here cycles to the
next mode (see [Environment modes](#environment-modes) below).

---

## Threat levels

The detection engine emits one of four threat levels at any
given time:

| Level | What it means | Buzzer |
|---|---|---|
| **CLEAR** | No active candidate. RF environment is consistent with no drone present. | silent |
| **ADVISORY** | A possible-drone candidate is forming. Not yet enough corroborating evidence to confirm. May reflect a real drone, infrastructure noise, or background RF in dense environments. | quiet single chirp on entry |
| **WARNING** | Likely drone present. Multiple corroborating evidence sources (modulation pattern, persistence, channel diversity) all align with drone-class emissions. | repeating mid-tone |
| **CRITICAL** | High-confidence drone, OR a Remote ID broadcast within ~500 m of the SENTRY-RF unit (proximity-CRITICAL — both unit and drone need a 3D GPS fix for this to fire). | repeating high-tone |

### How to interpret the levels

CLEAR is the absence of active candidates. It does not mean "no
drones could possibly be present" — it means SENTRY-RF doesn't see
RF evidence of one. A drone in receive-only mode, or an emission
type outside SENTRY-RF's scope, will register as CLEAR.

ADVISORY is intentionally low-stakes. The engine flags
"something interesting is forming" without committing to a drone
classification. In dense RF environments (cities, near LoRaWAN
gateways, near WiFi-heavy buildings), ADVISORY may fire from
infrastructure-class noise rather than drones — this is documented
behavior, not a fault. WARNING and CRITICAL require additional
corroborating evidence and are less prone to environmental noise.

WARNING and CRITICAL are the levels worth acting on. These reflect
multiple aligning signals (a control-link modulation pattern,
persistence across multiple scan cycles, channel-hopping diversity
above ambient baseline) and are designed to be specific to drone
emissions.

### Buzzer behavior

The buzzer plays distinct tones per level. Operators learn these
quickly. Press-and-hold BOOT for ~1 second to acknowledge the
current alert (silences buzzer until threat level changes).
Press-and-hold BOOT for 3+ seconds on **any non-Env-Mode page** to
toggle global mute (silences all alerts for 5 minutes).

### Honest caveat

Threat-level escalation depends on the detection engine's
characterization of the local RF environment, which varies by
location and time. The same CLEAR-vs-ADVISORY boundary that's
right in a suburban backyard may be wrong on a busy boardwalk
with dozens of WiFi APs and LoRaWAN gateways. The
[Environment modes](#environment-modes) provide an operator-tunable
adjustment, but no setting is universally optimal. Validate
against your specific environment when you can.

---

## The BOOT button

SENTRY-RF uses a single button for all operator interaction. The
gesture vocabulary is:

| Gesture | Action |
|---|---|
| **Single press** (< 1 s) | Advance to the next OLED screen |
| **Hold 1–3 s** | Acknowledge current alert (silences buzzer for the active threat) |
| **Hold 3+ s on Env Mode page** | Cycle environment mode (URBAN → SUBURBAN → RURAL → URBAN) |
| **Hold 3+ s on any other page** | Toggle global mute (5-minute silence) |

The 3+ second long-press behavior depends on which screen is
active — that's the **screen-context discriminator**. Holding while
viewing the Env Mode page cycles the mode; holding from any other
screen mutes the buzzer. There's no upper time limit on the hold,
so if you start holding and then realize you want to abort, just
keep holding (it'll cycle / mute once at the 3 s mark and not
again).

A future Tier 2 release may add a multi-button or rotary-encoder
UI option. The single-button gesture set is the current
operational interface.

---

## Environment modes

The environment mode is a runtime knob that adjusts two
detection-pipeline thresholds at once:

| Mode | Peak threshold (above NF) | WiFi skip-list TTL |
|---|---|---|
| **URBAN** | 12 dB | 5 minutes |
| **SUBURBAN** *(default)* | 10 dB | 3 minutes |
| **RURAL** | 6 dB | 1 minute |

The peak threshold gates which RSSI peaks the CAD scanner
considers worth probing for LoRa preambles. A higher threshold
(URBAN) demands a stronger signal-above-ambient before a peak is
considered; a lower threshold (RURAL) is more permissive.

The WiFi skip-list TTL controls how long the WiFi RID scanner
"skips" channels that have produced only undecoded-OUI matches —
freeing scanner attention for channels with potential drone
activity. Longer TTLs in URBAN reflect the assumption that more
WiFi clutter is around.

### Honest hedge on env-mode

The contribution of env-mode threshold differences to detection
performance in real-world environments has not been fully
characterized. Bench testing showed the runtime knob works
correctly (modes switch, NVS persists across power cycles, and
threshold values are applied) but did not isolate env-mode's
standalone effect from environmental factors like RF drift over
time. Field testing in varied environments is planned.

**Operator-practical guidance:** start with SUBURBAN (the
default). If you see frequent ADVISORY-level alerts that resolve
without becoming WARNING — and you're in a dense RF environment
(city, near LoRaWAN infrastructure) — try URBAN. If you're in a
quiet rural area and seem to be missing real drones, try RURAL.
The runtime cost of switching is zero; the modes are designed to
be tweaked.

### Persistence

Mode is stored in NVS flash and survives power cycles. To verify
the active mode after a power cycle, either look at the boot
banner (`[ENV-MODE] loaded from NVS: <mode>`) or navigate to the
Env Mode OLED page.

### Switching modes

Navigate to the Env Mode page (single-press through the screens
until you see "ENV MODE / [<current>]"). Hold BOOT for 3–4
seconds. The mode advances URBAN → SUBURBAN → RURAL → URBAN. The
new mode is written to NVS immediately and takes partial effect:

- **Peak threshold** (the dB-above-NF gate consumed by the RSSI
  sweep): takes effect on the next scan cycle. Immediate.
- **WiFi skip-list TTL:** applies to **newly-learned** skip
  entries only. Existing skip entries keep the TTL captured at
  learn time as an absolute deadline; switching modes does not
  retroactively shorten or extend them. To reset the skip-list
  state entirely, power-cycle the device, or wait for current
  entries to expire on their original timer.

---

## Remote ID detection

ASTM F3411 Remote ID is the FAA-mandated drone broadcast format.
A compliant drone broadcasts its UAS-ID, position, altitude,
operator location, and other metadata over WiFi (and/or BLE).
SENTRY-RF passively scans for these broadcasts on both transports.

### Decoded vs undecoded

Every received frame goes through the same path: scanner picks up
the beacon, checks for a recognized RID OUI (Open Drone ID OUI
`FA:0B:BC` is the canonical one), and tries to decode the inner
ASTM F3411 message pack.

- **Decoded RID:** the message pack decodes cleanly; UAS-ID,
  position, etc. populate the RID OLED page. If proximity is
  under 500 m and both the SENTRY-RF unit and the drone have a
  3D GPS fix, the threat level escalates to CRITICAL.

- **Undecoded OUI:** the OUI matches but the inner pack format
  doesn't decode. Logged at INFO level only, no escalation.
  This may happen with non-standard test emitters, malformed
  beacons, or future RID format extensions the firmware doesn't
  yet recognize.

If you're testing with the JJ companion device's `y1` command,
expect undecoded-OUI frames rather than decoded RID — the JJ test
emitter ships beacons with the correct OUI but uses a
test-friendly inner pack format that this firmware version
doesn't fully decode. This is a known test-harness mismatch and
not a real-RID detection failure.

### Proximity-CRITICAL

The proximity-CRITICAL escalation requires:
1. A decoded RID broadcast (not just an undecoded OUI match)
2. The drone's broadcast position
3. A valid 3D GPS fix on the SENTRY-RF unit
4. Distance between unit and drone under 500 m

If any of those is missing, the alert stays at WARNING (the
lower-confidence "drone present" level). This is fail-closed —
without confirmed positions on both ends, the proximity assertion
is not safe to make.

---

## GPS and positioning

GPS serves two functions in the firmware:

1. **Proximity calculations** for the RID-CRITICAL escalation
   (above).
2. **WiFi skip-list invalidation:** if the device moves more than
   100 m from where it last learned a WiFi skip entry, OR sustains
   a velocity above 5 km/h for 30 seconds, the skip list is
   cleared. The reasoning: the WiFi clutter pattern at a new
   location is unrelated to the previous location's clutter.

### Fail-closed on no-fix

Both functions fail closed when GPS lacks a 3D fix:
- The proximity-CRITICAL escalation cannot fire (no position to
  compare against the drone's broadcast).
- The WiFi skip-list does not learn new entries (anchor-location
  invalid). Any pre-existing skip entries are invalidated until
  fix returns.

This means GPS-aware features simply don't operate without a
fix. Operating without GPS is supported (the device still scans
for RF emissions and emits ADVISORY/WARNING based on signature
evidence) — you just lose the proximity refinement.

### Heltec V3

The Heltec V3 board has no GPS hardware. Proximity-CRITICAL never
fires on this target; the WiFi skip-list operates as if GPS were
permanently not-acquired (it doesn't learn new skip entries —
fail-closed). All other detection paths function normally.

### Indoor reception

Indoor GPS is unreliable on the T3S3's u-blox module — see the
GPS section in [FLASHING.md](FLASHING.md#gps-never-acquires-fix).
For first-time-out-of-the-box acquisition, take the unit outside
or near a window for 5–10 minutes.

---

## What SENTRY-RF can and can't detect

This is the most important section in this guide. **Read it
before relying on SENTRY-RF for any safety-of-life decision.**

### What SENTRY-RF detects

- **ELRS (ExpressLRS) control links** on FCC 915 MHz: 200 Hz,
  100 Hz, 50 Hz, 25 Hz packet rates, F1000 / F500 modes
- **TBS Crossfire** control links on 868/915 MHz
- **Generic LoRa** transmissions in the 860–930 MHz band, with
  channel-activity-detection (CAD) probing for LoRa preambles
- **Generic FHSS** patterns (channel-hopping diversity above
  ambient baseline)
- **Remote ID** ASTM F3411 broadcasts over WiFi (Open Drone ID OUI
  `FA:0B:BC`)
- **Remote ID** ASTM F3411 broadcasts over BLE (BLE 4 advertising
  packets)
- **2.4 GHz radio activity** *(LR1121 boards only)*: ELRS 2.4 GHz,
  generic 2.4 GHz FHSS, DJI energy footprints
- **LoRaWAN US915 infrastructure** (recognized as infrastructure-
  class noise; suppressed below WARNING)

### What SENTRY-RF does NOT detect

This is not exhaustive, but covers the most operationally
significant gaps:

- **DJI OcuSync / O3 / O4** — DJI's proprietary OFDM-based control
  link. SENTRY-RF's Tier 1 detection pipeline is FHSS- and
  LoRa-aware; OFDM detection is not implemented in this release.
  A significant fraction of DJI consumer drones use OcuSync — they
  will be largely invisible to this device. **OFDM detection is on
  the roadmap for a future tier; it is not in the current build.**

- **5.8 GHz band** — SENTRY-RF has no 5.8 GHz radio path. Drones
  operating exclusively on 5.8 GHz video downlinks or control
  links will not be detected. The board hardware does not include
  a 5.8 GHz receiver and adding one is not on the Tier 1 roadmap.

- **Analog video downlinks** (5.8 GHz analog FPV) — same as
  above. No 5.8 GHz radio path, no detection.

- **Receive-only drones / pre-takeoff drones** — drones that are
  not actively transmitting (powered down, in passive autonomy,
  using cellular C2 only) emit no RF in the bands SENTRY-RF
  covers. They are not detectable by passive RF.

- **Cellular C2** (LTE or 5G command-and-control) — not in the
  scanned bands. SENTRY-RF cannot see drones that use cellular
  links for control rather than dedicated radio.

- **High-altitude or distant drones** — RF detection range
  depends on TX power, receiver sensitivity, and path loss.
  Detection range for typical hobbyist drones is roughly under
  1 km; specialty long-range drones may emit at higher power and
  be visible further out, but small commercial drones at 500 m+
  altitude with directional ground links may be invisible.

### Treat SENTRY-RF as one input, not the input

The honest framing: SENTRY-RF is a passive RF sensor with
specific scope. It catches a meaningful slice of drone activity
(control links, RID broadcasts, generic LoRa/FHSS) but not all
of it. Combined with visual observation, acoustic detection, or
other sensors, it adds situational awareness. Used alone as a
"drone alarm," it will miss certain classes of drones —
particularly DJI OcuSync — and operators relying on it should
know that.

For known limitations and operational watch items, see
[KNOWN_ISSUES.md](KNOWN_ISSUES.md).

---

## ZMQ output (for integration)

SENTRY-RF emits structured threat events as ZeroMQ PUB messages on
TCP port `4227`. Each threat-state change publishes a JSON-
formatted message with the threat level, anchor frequency, RSSI,
GPS position (if available), CAD confirmation count, and operator
mode.

Subscribers like `DragonSync` or `RFTrack` can consume this stream
for upstream integration. See `src/zmq_publisher.cpp` for the
exact message schema.

---

## See also

- [FLASHING.md](FLASHING.md) — getting firmware on the board
- [HARDWARE_T3S3_LR1121.md](HARDWARE_T3S3_LR1121.md) /
  [HARDWARE_T3S3_SX1262.md](HARDWARE_T3S3_SX1262.md) — board-
  specific hardware details
- [KNOWN_ISSUES.md](KNOWN_ISSUES.md) — current Tier 1 limitations
  and watch items
- [BUILD_GUIDE.md](BUILD_GUIDE.md) — build options and
  compile-time flags
