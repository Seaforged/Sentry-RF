# SENTRY-RF Field Test Results
**Date:** April 1, 2026
**Location:** Rural NC (34.80N, -79.36W)
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

## Test 3: ELRS Detection (Long Range, JJ at 22 dBm)

JJ stationary at 22 dBm (158 mW). SENTRY driving on rural road, 2.8 to 3.3 km away, then returning.

### Distance vs Detection (COM8)

| Range | Windows | Pd (div>=3) | Avg div | Max div |
|-------|---------|------------|---------|---------|
| 2.8-2.9 km | 14 | 43% | 2.5 | 8 |
| 2.9-3.0 km | 13 | **77%** | 2.9 | 5 |
| 3.0-3.1 km | 12 | **58%** | 3.1 | 9 |
| 3.1-3.2 km | 4 | 25% | 1.5 | 3 |
| 3.2-3.3 km | 10 | 50% | 2.5 | 4 |
| 3.3+ km | 3 | 33% | 1.7 | 3 |

**Return trip (2.8 km, driving back toward JJ):** 93% Pd, avg div=5.0, max div=14

**Maximum detection distance:** 3,305 meters (div=3)

### Link Budget

| Distance | FSPL | Rx Power | Margin over sensitivity |
|----------|------|----------|----------------------|
| 2.8 km | 100.6 dB | -78.6 dBm | 29.4 dB |
| 3.0 km | 101.2 dB | -79.2 dBm | 28.8 dB |
| 3.3 km | 102.0 dB | -80.0 dBm | 28.0 dB |

SX1262 CAD sensitivity at SF6/BW500: approximately -108 dBm.
Signal at max detection distance is 28 dB above sensitivity — the limiting factor is scan probability (catching 1 of 80 FHSS channels), not receiver sensitivity.

## Key Findings

### 1. Frequency Diversity Architecture Validated
- Field baseline: div = 0-2 (rural, no transmitter)
- ELRS active: div = 3-14 (bare board), div = 1-11 (cased)
- Clean separation enables WARNING=3, CRITICAL=5 thresholds
- Zero false alarms during 120-second baseline

### 2. Detection Range Exceeds Expectations
- 3.3 km detection at 158 mW (22 dBm)
- 100% signal presence at 2-200m through metal obstacles at 10 mW
- At typical ELRS power (250 mW-1W), range would extend to 5+ km

### 3. 3D Printed Case Attenuates Signal
- COM8 (bare): 89% Pd at short range
- COM9 (cased): 41% Pd at short range
- ~3 dB effective loss from the case
- **Fix: Add external SMA antenna connector to case**

### 4. NLOS Detection Works
- Metal buildings and vehicles did not prevent detection at 2-200m
- Signal drops during NLOS segments at long range but recovers
- The system is suitable for urban/compound patrol scenarios

### 5. GPS Integrity Monitoring Working
- 25 satellites, 3D fix, 0m horizontal accuracy
- Jamming indicator: 0 (no interference)
- Spoofing detection: 0 (no anomaly)
- C/N0 standard deviation: 4.6 dBHz (healthy)

## Predicted Detection Ranges (Pd = 50%)

Based on field-measured Rx power threshold of approximately -80 dBm at Pd=50%:

| TX Power | Typical Use | Predicted Range |
|----------|-----------|-----------------|
| 10 mW (10 dBm) | Low-power ELRS | ~300m |
| 50 mW (17 dBm) | Standard ELRS | ~900m |
| 158 mW (22 dBm) | ELRS (validated) | ~3.1 km |
| 500 mW (27 dBm) | High-power ELRS | ~5.5 km |
| 1 W (30 dBm) | Crossfire max | ~9.8 km |

Note: These are conservative estimates based on actual field data, not free-space theoretical calculations.
