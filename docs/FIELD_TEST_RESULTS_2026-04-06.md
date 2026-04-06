# SENTRY-RF Field Test Results — April 6, 2026

## Suburban Drive Test

**Firmware:** v1.5.3 (persistence gate 5, FSK threshold -70 dBm, GPS_MIN_CNO 15)
**Environment:** Suburban USA — residential neighborhood, houses, trees, parked cars
**Date:** April 6, 2026, late afternoon
**Weather:** Clear

### Equipment

| Device | Role | Config |
|--------|------|--------|
| T3S3 #1 (SENTRY-RF v1.5.3) | Detector, mobile (in vehicle) | Field config: FSK -70 dBm, GPS_MIN_CNO 15 |
| T3S3 #2 (JUH-MAK-IN JAMMER v2.0.0) | Signal source, stationary | ELRS FCC915, 40ch, SF6/BW500, 200 Hz hop, ~10 mW |
| u-blox M10 GPS | Position tracking on SENTRY | UART, 1 Hz nav rate |

### Test Procedure

1. JJ placed at stationary position, transmitting ELRS FCC915 continuously
2. SENTRY-RF driven away from JJ along suburban streets and back
3. All serial output logged with timestamps and GPS coordinates
4. Total track: 2.44 km driven, 11.5 minutes, max 1,009m from JJ

### Summary Results

| Metric | Result |
|--------|--------|
| Total CAD cycles | 282 |
| Scan rate | 24.5 cycles/min (~2.4s per cycle) |
| Max distance from JJ | 1,009m |
| Furthest WARNING+ detection | 842m (outbound) |
| Furthest ADVISORY+ detection | 1,009m (at max range) |
| Peak score | 100 |
| Peak persDiv | 24 |
| Peak sustainedCycles | 6 |
| Peak confirmed taps | 19 |
| Persistence gate activations | 3 (all at legitimate signal positions) |
| False positives | 0 (all detections correlated with JJ signal) |
| GNSS anomalies | 0 (Jam=0, Spoof=0 throughout) |

### GPS Quality

| Metric | Value |
|--------|-------|
| Satellites tracked | 14-23 |
| pDOP range | 1.16-2.23 |
| Horizontal accuracy | 1-5m |
| Fix type | 3D throughout |
| Altitude range | 0-12m |

### Detection by Distance

| Distance from JJ | Score Range | persDiv | Detection Mechanism |
|-------------------|-----------|---------|---------------------|
| 0m (start) | 80 | 5 | Persistence gate passed, full scoring |
| 300-530m | 26-36 | 0 | Confirmed taps at half-weight (conf=2-3 x 7) |
| 560-640m | 33-47 | 0 | Stronger tap accumulation (conf=4-6 x 7) |
| 680-830m | 40-100 | 0-24 | conf=5-19, persistence gate passed at 823m |
| 830-1009m | 5-15 | 0 | Signal fading, ADVISORY from supporting evidence |
| Return 797m | 26 | 0 | Confirmed taps on return pass |

### Threat Level Distribution

| Level | Cycles | Percentage |
|-------|--------|------------|
| CLEAR | 112 | 39.7% |
| ADVISORY | 146 | 51.8% |
| WARNING | 17 | 6.0% |
| CRITICAL | 7 | 2.5% |

### Score Distribution

| Bin | Count | Percentage |
|-----|-------|------------|
| 0-5 | 112 | 39.7% |
| 6-8 | 0 | 0.0% |
| 9-15 | 90 | 31.9% |
| 16-24 | 56 | 19.9% |
| 25-40 | 17 | 6.0% |
| 41+ | 7 | 2.5% |

### Key Observations

**1. Confirmed CAD taps are the primary long-range detection mechanism.**

At 300-700m, every WARNING-level detection came from confirmed taps with persDiv=0. The half-weight tap pathway (conf x 7 pts) provided reliable detection without requiring the persistence gate to activate. This is the intended behavior — confirmed CAD taps are a high-confidence signal that ambient LoRa rarely produces.

**2. The persistence gate works correctly in the field.**

persDiv only activated 3 times total:
- At 0m (close range, strong signal)
- At 811m and 830m (sustained high diversity for 5-6 consecutive cycles)

All three activations were legitimate — no false persistence gate activations during the entire drive. The gate provides score amplification (persDiv x 8 pts + fast-detect bonus) when diversity is genuinely sustained.

**3. Detection range improved 4x over v1.4.0.**

The April 1 rural test (v1.4.0) achieved WARNING at ~200m with 10 mW. This test achieved WARNING at 842m — a 4x improvement. Contributing factors:
- AAD persistence gate eliminates false detections, allowing confirmed taps to drive scoring
- Half-weight tap pathway (conf x 7) provides a sensitive detection mechanism that doesn't require the persistence gate
- FSK threshold lowered from -50 to -70 dBm for field sensitivity

**4. Asymmetric detection on outbound vs return.**

Outbound: WARNING at 842m. Return: WARNING at 797m. The ~5% difference is consistent with orientation effects (antenna pattern, vehicle body shielding) and normal multipath variation in suburban environments.

**5. GNSS integrity clean throughout.**

Zero jamming or spoofing indications across the entire drive. This confirms the GPS subsystem is stable in a mobile suburban environment and the integrity thresholds (GPS_MIN_CNO=15, CNO_STDDEV_SPOOF_THRESH=2.0) are appropriate for outdoor use.

### Detection Timeline

```
17:38:02  Start — 0m from JJ, score=80, persDiv=5 (close range)
17:38:22  Signal drops, CLEAR
17:39:15  306m — WARNING via conf taps (score=29, persDiv=0)
17:39:44  502m — WARNING via conf taps (score=29, persDiv=0)
17:39:57  598m — WARNING via conf taps (score=33, persDiv=0)
17:40:03  638m — CRITICAL via conf taps (score=47, persDiv=0)
17:40:10  684m — CRITICAL edge (score=40, persDiv=0)
17:40:22  811m — CRITICAL, persistence gate activated (score=100, persDiv=24)
17:40:27  830m — CRITICAL (score=100, persDiv=19)
17:42:21  784m — WARNING on return (score=26, persDiv=0)
17:47:29  ~200m — Last WARNING on approach (score=29)
17:49:31  End — back near start
```

### Range Estimation Model

Using the 842m WARNING data point at 10 mW (+10 dBm) and a suburban path loss exponent of n=2.8:

```
PL(842m) = PL(1m) + 10 * n * log10(842)
         = 40.5 + 10 * 2.8 * log10(842)
         = 40.5 + 81.9
         = 122.4 dB

For higher TX power P (in mW):
  Additional range factor = (P / 10)^(1/(2*n))
  WARNING range = 842m * (P / 10)^(1/5.6)
```

| TX Power | Calculated WARNING Range | Notes |
|----------|--------------------------|-------|
| 10 mW | 842m | Field tested |
| 25 mW | 1,170m | Modeled |
| 100 mW | 1,930m | Modeled |
| 250 mW | 2,680m | Modeled |
| 500 mW | 3,430m | Modeled |
| 1000 mW | 4,390m | Modeled |

**Caveat:** Only the 10 mW data point is field-tested. Higher power estimates assume the same path loss exponent and have not been validated. Real-world performance depends on terrain, antenna orientation, multipath, and interference.

### Comparison with April 1 Rural Test

| Metric | April 1 (v1.4.0) | April 6 (v1.5.3) | Change |
|--------|-------------------|-------------------|--------|
| Firmware | v1.4.0 | v1.5.3 | AAD + corroborated scoring |
| Environment | Rural, metal buildings | Suburban, residential | More obstructions |
| TX Power | 10 mW | 10 mW | Same |
| Max WARNING range | ~200m | 842m | **4.2x improvement** |
| Max ADVISORY range | ~400m | 1,009m | **2.5x improvement** |
| False positives | 0 | 0 | Same |
| Detection mechanism | Diversity threshold | Confirmed CAD taps | Tap pathway is more sensitive |

### Test Limitations

- Single test run (not repeated for statistical significance)
- JJ transmitter power not independently verified (nominally 10 mW / +10 dBm)
- Suburban environment only — not tested in dense urban, open field, or wooded terrain
- Antenna inside vehicle — external mount would improve range
- Single outbound/return pass — no dwell time at specific distances

---

*Test conducted with [JUH-MAK-IN JAMMER v2.0.0](https://github.com/Seaforged/Juh-Mak-In-Jammer) as the signal source.*
*Raw log: field_test_20260406_173801.log (not included in repo — contains GPS coordinates)*
