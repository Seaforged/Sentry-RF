# SENTRY-RF Field Test Protocol
**Version:** 1.0 — April 2026
**Equipment:** SENTRY-RF (T3S3 + SX1262), JUH-MAK-IN JAMMER (T3S3 + SX1262)

---

## Pre-Test Checklist

- [ ] SENTRY-RF flashed with v1.4.0+ firmware
- [ ] JJ flashed with latest ELRS FHSS firmware (130 Hz, SF6/BW500)
- [ ] SD card inserted in SENTRY-RF (FAT32 formatted)
- [ ] GPS antenna connected, achieving 3D fix (check serial: `fix=3`)
- [ ] Both devices have fresh batteries / USB power
- [ ] Laptop with `tools/analyze_field_test.py` ready
- [ ] Pen and paper backup for manual `div=N` readings

## Test Location Requirements

- Open area with clear sky (for GPS)
- Minimal buildings within 200m (reduces multipath)
- Rural preferred — less ambient LoRa/ISM interference
- Known distances measurable (use GPS waypoints or measuring tape for <100m)

---

## Test 1: Baseline (THE MOST IMPORTANT TEST)

**Purpose:** Measure ambient frequency diversity with no drone signal.
**Duration:** 120 seconds after warmup
**JJ:** OFF

### Procedure
1. Power on SENTRY-RF. Wait for `[WARMUP] Complete` on serial (~50s).
2. Note the time. Let it run for 120 seconds.
3. Watch serial for `div=N` values.
4. Record max `div` seen during the 120 seconds.

### Expected Results
| Environment | Expected max div | Action |
|-------------|-----------------|--------|
| Rural open field | 0-1 | Lower DIVERSITY_WARNING to 3, DIVERSITY_CRITICAL to 5 |
| Suburban | 1-3 | Keep WARNING=5, CRITICAL=8 or lower to 4/6 |
| Urban/bench | 3-7 | Current thresholds may be needed |

### What This Tells You
If `max div <= 2` for the entire 120 seconds, you can immediately:
1. Edit `sentry_config.h`: set `DIVERSITY_WARNING = 3`, `DIVERSITY_CRITICAL = 5`
2. Reflash SENTRY-RF
3. Proceed to detection tests with the fast thresholds

**Write down:** max div = _____, threat level = _____

---

## Test 2: ELRS Detection at Range

**Purpose:** Measure detection time vs distance.
**JJ Mode:** ELRS (send `e` on COM6 at 115200)

### Distance Table

Run each distance for 60 seconds after warmup stabilizes. Record the first WARNING and CRITICAL times by watching serial output.

| Distance | JJ Power | First div>=3 | WARNING time | CRITICAL time | Max div | Notes |
|----------|----------|-------------|-------------|---------------|---------|-------|
| 10m | 10 dBm | | | | | |
| 25m | 10 dBm | | | | | |
| 50m | 10 dBm | | | | | |
| 100m | 10 dBm | | | | | |
| 200m | 10 dBm | | | | | |
| 500m | 10 dBm | | | | | |
| 100m | 22 dBm | | | | | |
| 500m | 22 dBm | | | | | |
| 1000m | 22 dBm | | | | | |

**JJ power cycling:** JJ cycles through power levels. Use `p` on COM6 serial to cycle to the desired power. The current power is shown in the serial output.

### Procedure per Distance
1. Place JJ at the test distance with antenna vertical
2. SENTRY-RF at the observation point
3. Wait for warmup if just powered on
4. Send `e` to JJ to start ELRS
5. Start timer
6. Record first `div>=3` time, first WARNING, first CRITICAL
7. After 60s, send `q` to JJ to stop
8. Record time to CLEAR (rapid-clear)
9. Move to next distance

---

## Test 3: Rapid-Clear Timing

**Purpose:** Verify threat clears quickly after drone leaves.
**Run at each distance after Test 2.**

| Distance | Time to WARNING | Time to CLEAR after stop | Rapid-clear fired? |
|----------|----------------|--------------------------|-------------------|
| 10m | | | |
| 50m | | | |
| 100m | | | |

Watch for `[RAPID-CLEAR]` in serial output.

---

## Test 4: CW Tone Detection

**Purpose:** Verify continuous wave detection.
**JJ Mode:** CW Tone (send `c` on COM6)
**Distance:** 50m

| Frequency | Power | Detected? | Peak RSSI | Threat Level |
|-----------|-------|-----------|-----------|-------------|
| 915.0 MHz | 10 dBm | | | |
| 915.0 MHz | 22 dBm | | | |

---

## Test 5: Baseline with GPS Integrity

**Purpose:** Verify GNSS monitoring outdoors.
**JJ:** OFF
**Duration:** 60 seconds

Record from serial:
- `jam_ind`: _____ (should be 0)
- `spoof_state`: _____ (should be 0)
- `cno_sd`: _____ dB-Hz (should be 3-8 for healthy sky)
- `num_sv`: _____ (should be 10+)
- `fix`: _____ (should be 3 = 3D fix)

---

## Post-Test Analysis

1. Remove SD card from SENTRY-RF
2. Copy `field_NNNN.jsonl` to laptop
3. Run analysis:

```bash
# Baseline only
python tools/analyze_field_test.py field_0001.jsonl --baseline-end 120

# Full test with JAMMER segments
python tools/analyze_field_test.py field_0002.jsonl --jammer-start 60 --jammer-stop 120

# Adjust warmup skip if needed (default 55s)
python tools/analyze_field_test.py field_0001.jsonl --warmup 55
```

---

## Decision Matrix After Test 1

| Baseline max div | Action |
|-----------------|--------|
| 0-1 | Set WARNING=3, CRITICAL=5. Detection will be fast (<5s WARNING). |
| 2-3 | Set WARNING=4, CRITICAL=6. Good balance. |
| 4+ | Keep WARNING=5, CRITICAL=8. Environment too noisy for fast thresholds. |

**The single most important number from the entire field test is the baseline max diversity.** Everything else depends on it.
