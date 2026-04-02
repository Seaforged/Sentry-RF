# SENTRY-RF Field Test Results
**Date:** April 1, 2026
**Location:** Rural North Carolina, USA
**Firmware:** v1.4.0 with field-calibrated diversity thresholds (WARNING=3, CRITICAL=5)

## Equipment
- SENTRY-RF COM8: LilyGo T3S3 v1.3 + SX1262, bare board, whip antenna
- SENTRY-RF COM9: LilyGo T3S3 v1.3 + SX1262, 3D printed case, GPS (u-blox M10, 25 SVs, Fix 3D)
- JUH-MAK-IN JAMMER: LilyGo T3S3 v1.3 + SX1262, ELRS mode (SF6/BW500, 80ch FHSS, 130 Hz)

## Test 1: Baseline (No Transmitter)

Both boards running, JJ off, 120 seconds after warmup.

| Board | Max div | Avg div | Max Threat | Cycles |
|-------|---------|---------|------------|--------|
| COM8 (bare) | 2 | 0.8 | ADVISORY | 30 |
| COM9 (cased) | 1 | 0.3 | ADVISORY | 6 |

**Result:** Clean. Max diversity = 2 across both boards. Zero false WARNING/CRITICAL.
This validated lowering thresholds from WARNING=5/CRITICAL=8 to WARNING=3/CRITICAL=5.

## Test 2: ELRS Detection (Close Range, JJ at 10 dBm)

JJ stationary at 10 dBm (10 mW). SENTRY walking 2-200m around compound with metal buildings and vehicles as obstacles. Two full laps, ~10 minutes.

### COM8 (bare board)
| Metric | Value |
|--------|-------|
| Detection probability (div >= 3) | **89%** |
| Signal presence (div >= 1) | **100%** |
| CRITICAL rate | 76% |
| Average diversity | 4.7 |
| Max diversity | 9 |

### COM9 (3D printed case)
| Metric | Value |
|--------|-------|
| Detection probability (div >= 3) | **41%** |
| Average diversity | 2.5 |
| Max diversity | 11 |

**Case attenuation:** COM8 89% vs COM9 41% — the 3D printed case reduces detection performance by approximately half, equivalent to ~3 dB signal loss.

## Test 3: ELRS Detection (Driving, JJ at 22 dBm)

JJ stationary at 22 dBm (158 mW). SENTRY driving on rural road, 0 to ~660m away, then returning. Duration ~14 minutes.

**Note:** Initial analysis incorrectly placed JJ at the compound location (2.8 km away). Corrected after confirming JJ was positioned near the drive starting point.

### Distance vs Detection (COM8, outbound)

| Range | Windows | Pd (div>=3) | Avg div | Max div |
|-------|---------|------------|---------|---------|
| 0-99m | 14 | 36% | 2.1 | 8 |
| 100-199m | 8 | **75%** | 3.2 | 5 |
| 200-299m | 9 | 56% | 2.3 | 3 |
| 300-399m | 10 | **70%** | 3.5 | 9 |
| 400-499m | 3 | 33% | 1.3 | 3 |
| 500-599m | 7 | 43% | 2.6 | 4 |
| 600-699m | 6 | 50% | 2.0 | 3 |

**Return trip (back to 0m):** 93% Pd, avg div=5.0, max div=14

**Maximum detection distance:** 637 meters (div=3)

### Notes on 0-99m low Pd
The 0-99m bin shows only 36% Pd because the first ~100 seconds include the warmup period where diversity recording is disabled (div=0 during warmup). Post-warmup close-range performance matches Site 1 (89%+ Pd).

### Link Budget

| Distance | FSPL | Rx Power | Margin over sensitivity |
|----------|------|----------|----------------------|
| 100m | 71.7 dB | -49.7 dBm | 58.3 dB |
| 300m | 81.2 dB | -59.2 dBm | 48.8 dB |
| 637m | 87.8 dB | -65.8 dBm | 42.2 dB |

At all test distances, the signal was 42+ dB above the SX1262 sensitivity floor (-108 dBm). The limiting factor for Pd is scan probability (catching FHSS hops), not signal strength.

## Key Findings

### 1. Frequency Diversity Architecture Validated
- Field baseline: div = 0-2 (rural, no transmitter)
- ELRS active: div = 3-14 (bare board), div = 1-11 (cased)
- Clean separation enables WARNING=3, CRITICAL=5 thresholds
- Zero false alarms during 120-second baseline

### 2. Detection at Range
- 637m detection at 158 mW (22 dBm) while driving
- 89% Pd at 2-200m through metal obstacles at 10 mW
- Signal remains 42+ dB above sensitivity at 637m
- Scan probability (not sensitivity) is the detection bottleneck

### 3. 3D Printed Case Attenuates Signal
- COM8 (bare): 89% Pd at short range
- COM9 (cased): 41% Pd at short range
- ~3 dB effective loss from the case
- **Fix: Add external SMA antenna connector to case**

### 4. NLOS Detection Works
- Metal buildings and vehicles did not prevent detection at 2-200m
- Signal drops during NLOS segments but recovers
- The system is suitable for urban/compound patrol scenarios

### 5. GPS Integrity Monitoring Working
- 25 satellites, 3D fix, sub-meter horizontal accuracy
- Jamming indicator: 0 (no interference)
- Spoofing detection: 0 (no anomaly)
- C/N0 standard deviation: 4.6 dBHz (healthy)

## Predicted Detection Ranges

Based on the field observation that Pd is limited by scan probability (not sensitivity) at these power levels, with 42 dB of margin remaining at 637m:

| TX Power | Typical Use | Estimated Range (Pd=50%) |
|----------|-----------|--------------------------|
| 10 mW (10 dBm) | Low-power ELRS | 200-400m (validated at 200m NLOS) |
| 158 mW (22 dBm) | ELRS | 500-700m (validated at 637m) |
| 500 mW (27 dBm) | High-power ELRS | 1-2 km (extrapolated) |
| 1 W (30 dBm) | Crossfire max | 2-4 km (extrapolated) |

Note: Range predictions beyond validated distances are extrapolations. The scan probability bottleneck means range scales slower than free-space path loss alone would predict. Field testing at longer distances with higher TX power is needed.
