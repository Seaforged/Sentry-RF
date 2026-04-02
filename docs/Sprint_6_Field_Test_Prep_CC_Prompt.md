# Sprint 6 — Field Test Preparation

## What You Need for the Field Test

**Hardware checklist:**
- SENTRY-RF (T3S3 SX1262, COM9) with external antenna if available
- JUH-MAK-IN JAMMER (T3S3, COM6)
- Laptop with PlatformIO, Python 3.10+, USB cables for both boards
- SD card in SENTRY-RF's SD slot (formatted FAT32)
- Fully charged 18650 batteries for both boards (or USB power bank)
- Stopwatch or phone timer (backup for manual timing)

**Test location:**
- Open rural area, away from buildings with LoRaWAN gateways
- Clear sky view for GPS fix (no dense tree canopy overhead)
- Space to walk 10m, 50m, 100m, 250m, 500m from JJ's fixed position
- Near an airport is fine — airport comms are on VHF/UHF, not 900 MHz ISM

**Do NOT fly a drone.** JJ simulates the RF link. You're testing the RF detector, not the drone.

## Part 1: GPS_MIN_CNO Production Value (Sprint 6A)

**CRITICAL — do this before going to the field.**

In `include/sentry_config.h`, change:

```cpp
static const int GPS_MIN_CNO = 15;  // FIELD: 15-20. INDOOR TESTING: set to 6.
```

Also, the C/N0 uniformity check in `gnss_integrity.cpp` should be gated:

```cpp
// Only run C/N0 uniformity spoofing check when GPS_MIN_CNO is high enough.
// At CNO=6 (indoor), attenuated signals cluster naturally → false positives.
if (GPS_MIN_CNO >= 15) {
    // ... existing C/N0 uniformity check ...
}
```

If the C/N0 check doesn't have this guard yet, add it. If it does, verify it's there.

## Part 2: Enhanced SD Card Logging (Sprint 6B)

The current logging needs to capture enough data for post-test analysis. Create or update the data logger to write JSONL (one JSON object per line) to the SD card.

### Log file naming

Each boot session creates a new log file: `SENTRY_{bootNum}_{timestamp}.jsonl`

If no GPS fix is available at boot, use `SENTRY_{bootNum}.jsonl` and add the timestamp once GPS is acquired.

### Per-cycle log entry (every CAD cycle, ~1s)

Write one JSON line per scan cycle:

```json
{
  "t": 12345,
  "cycle": 42,
  "div": 3,
  "conf": 0,
  "strong": 1,
  "fsk": 0,
  "taps": 4,
  "threat": 2,
  "cadMs": 1023,
  "rssiPeak": -72.3,
  "rssiFreq": 915.2,
  "rssiNF": -105.1,
  "gps": {"fix": 3, "sv": 14, "lat": 36.852, "lon": -75.978, "pdop": 1.3},
  "gnss": {"jam": 12, "spoof": 1, "cnoSD": 5.2}
}
```

Field definitions:
- `t`: millis() timestamp
- `cycle`: scan cycle number
- `div`: frequency diversity count (the key metric)
- `conf`: confirmedCadCount
- `strong`: strongPendingCad
- `fsk`: confirmedFskCount
- `taps`: totalActiveTaps
- `threat`: threat level (0=CLEAR, 1=ADVISORY, 2=WARNING, 3=CRITICAL)
- `cadMs`: CAD scan duration in milliseconds
- `rssiPeak/rssiFreq/rssiNF`: RSSI sweep peak info (only when RSSI runs)
- `gps`: GPS fix data (only when GPS is available)
- `gnss`: GNSS integrity data

### On threat level transitions

Add an extra log line with transition details:

```json
{
  "t": 12345,
  "event": "THREAT_CHANGE",
  "from": 1,
  "to": 2,
  "trigger": "diversity",
  "div": 5
}
```

### Implementation notes

- Use `ArduinoJson` library if available, or manual `snprintf` for the JSON formatting
- Buffer writes — don't flush to SD every cycle. Flush every 10 cycles or on threat transitions
- The SD card is on FSPI (separate SPI bus from LoRa on HSPI) — no bus contention
- Keep the log compact — this is a 1Hz log that may run for 30+ minutes
- Add the logger calls in `main.cpp` after the detection engine runs, inside the existing `loggerWrite()` call or alongside it
- Gate detailed GPS/GNSS fields behind having a valid fix (don't log null GPS data)

### Serial output for real-time monitoring

Keep the existing serial output but add `div=N` prominently:

```
[SCAN] CAD:1023ms div=3 | Threat: ADVISORY
```

The operator needs to see `div` in real time during the field test to correlate with what JJ is doing.

## Part 3: Field Test Analysis Script (Sprint 6C)

Create `C:\Projects\sentry-rf\field_test_analyzer.py` — a Python script that reads the JSONL log files from the SD card and produces a field test report.

```python
#!/usr/bin/env python3
"""
SENTRY-RF Field Test Analyzer
Reads JSONL log files from SD card and produces detection performance report.

Usage:
    python field_test_analyzer.py <logfile.jsonl> [--distance <meters>] [--label <test_name>]
    python field_test_analyzer.py --batch <directory>  # Process all .jsonl files
"""

import json
import sys
import os
from datetime import datetime
from collections import defaultdict

def parse_log(filepath):
    """Parse a JSONL log file into a list of entries."""
    entries = []
    with open(filepath, 'r') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                entries.append(json.loads(line))
            except json.JSONDecodeError:
                continue
    return entries

def analyze_baseline(entries):
    """Analyze a no-transmitter baseline period."""
    threats = [e.get('threat', 0) for e in entries if 'threat' in e]
    divs = [e.get('div', 0) for e in entries if 'div' in e]
    
    if not threats:
        return None
    
    return {
        'duration_s': (entries[-1]['t'] - entries[0]['t']) / 1000.0 if len(entries) > 1 else 0,
        'max_threat': max(threats),
        'mean_div': sum(divs) / len(divs) if divs else 0,
        'max_div': max(divs) if divs else 0,
        'false_warning_count': sum(1 for t in threats if t >= 2),
        'false_critical_count': sum(1 for t in threats if t >= 3),
        'pfa_warning': sum(1 for t in threats if t >= 2) / len(threats) if threats else 0,
    }

def analyze_detection(entries, signal_start_idx=None):
    """Analyze detection performance during active transmission."""
    if signal_start_idx is None:
        signal_start_idx = 0
    
    active = entries[signal_start_idx:]
    if not active:
        return None
    
    threats = [e.get('threat', 0) for e in active if 'threat' in e]
    divs = [e.get('div', 0) for e in active if 'div' in e]
    t0 = active[0]['t']
    
    # Time to first WARNING
    time_to_warning = None
    for e in active:
        if e.get('threat', 0) >= 2:
            time_to_warning = (e['t'] - t0) / 1000.0
            break
    
    # Time to first CRITICAL
    time_to_critical = None
    for e in active:
        if e.get('threat', 0) >= 3:
            time_to_critical = (e['t'] - t0) / 1000.0
            break
    
    # Detection probability (fraction of cycles with any detection)
    pd = sum(1 for d in divs if d >= 1) / len(divs) if divs else 0
    
    return {
        'duration_s': (active[-1]['t'] - t0) / 1000.0 if len(active) > 1 else 0,
        'time_to_warning_s': time_to_warning,
        'time_to_critical_s': time_to_critical,
        'max_threat': max(threats) if threats else 0,
        'mean_div': sum(divs) / len(divs) if divs else 0,
        'max_div': max(divs) if divs else 0,
        'pd': pd,
    }

def analyze_clear_time(entries, signal_stop_idx):
    """Analyze how quickly the system returns to CLEAR after signal stops."""
    post = entries[signal_stop_idx:]
    if not post:
        return None
    
    t0 = post[0]['t']
    time_to_clear = None
    rapid_clear_fired = False
    
    for e in post:
        if e.get('threat', 0) == 0:
            time_to_clear = (e['t'] - t0) / 1000.0
            break
        if e.get('event') == 'RAPID_CLEAR':
            rapid_clear_fired = True
    
    return {
        'time_to_clear_s': time_to_clear,
        'rapid_clear': rapid_clear_fired,
    }

def print_report(filepath, baseline, detection, clear_time, label=None, distance=None):
    """Print a formatted test report."""
    print("=" * 70)
    print(f"SENTRY-RF Field Test Report")
    print(f"Log: {os.path.basename(filepath)}")
    if label:
        print(f"Test: {label}")
    if distance:
        print(f"Distance: {distance}m")
    print(f"Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    print("=" * 70)
    
    if baseline:
        print(f"\n--- BASELINE (no transmitter) ---")
        print(f"Duration:         {baseline['duration_s']:.1f}s")
        print(f"Max threat:       {baseline['max_threat']} ({'PASS' if baseline['max_threat'] <= 1 else 'FAIL'})")
        print(f"Max diversity:    {baseline['max_div']}")
        print(f"Mean diversity:   {baseline['mean_div']:.1f}")
        print(f"False WARNINGs:   {baseline['false_warning_count']}")
        print(f"False CRITICALs:  {baseline['false_critical_count']}")
        print(f"Pfa (WARNING):    {baseline['pfa_warning']:.3f}")
    
    if detection:
        print(f"\n--- DETECTION (transmitter active) ---")
        print(f"Duration:         {detection['duration_s']:.1f}s")
        print(f"Time to WARNING:  {detection['time_to_warning_s']:.1f}s" if detection['time_to_warning_s'] else "Time to WARNING:  NEVER")
        print(f"Time to CRITICAL: {detection['time_to_critical_s']:.1f}s" if detection['time_to_critical_s'] else "Time to CRITICAL: NEVER")
        print(f"Max threat:       {detection['max_threat']}")
        print(f"Max diversity:    {detection['max_div']}")
        print(f"Mean diversity:   {detection['mean_div']:.1f}")
        print(f"Pd (any detect):  {detection['pd']:.3f}")
    
    if clear_time:
        print(f"\n--- CLEAR TIME (after transmitter stops) ---")
        print(f"Time to CLEAR:    {clear_time['time_to_clear_s']:.1f}s" if clear_time['time_to_clear_s'] else "Time to CLEAR:    NEVER")
        print(f"Rapid-clear:      {'Yes' if clear_time['rapid_clear'] else 'No'}")
    
    print("\n" + "=" * 70)

def main():
    if len(sys.argv) < 2:
        print("Usage: python field_test_analyzer.py <logfile.jsonl> [--distance N] [--label NAME]")
        print("       python field_test_analyzer.py --batch <directory>")
        sys.exit(1)
    
    # Parse args
    filepath = sys.argv[1]
    distance = None
    label = None
    for i, arg in enumerate(sys.argv):
        if arg == '--distance' and i + 1 < len(sys.argv):
            distance = float(sys.argv[i + 1])
        if arg == '--label' and i + 1 < len(sys.argv):
            label = sys.argv[i + 1]
    
    if filepath == '--batch':
        directory = sys.argv[2] if len(sys.argv) > 2 else '.'
        for f in sorted(os.listdir(directory)):
            if f.endswith('.jsonl'):
                entries = parse_log(os.path.join(directory, f))
                if entries:
                    baseline = analyze_baseline(entries)
                    print_report(f, baseline, None, None)
        sys.exit(0)
    
    entries = parse_log(filepath)
    if not entries:
        print(f"No valid entries in {filepath}")
        sys.exit(1)
    
    print(f"Loaded {len(entries)} entries from {filepath}")
    
    # Simple analysis — treat entire file as one test
    baseline = analyze_baseline(entries)
    detection = analyze_detection(entries)
    
    print_report(filepath, baseline, detection, None, label, distance)

if __name__ == '__main__':
    main()
```

**Note to CC:** Create this script at `C:\Projects\sentry-rf\field_test_analyzer.py`. It should run standalone with no external dependencies beyond the Python standard library. Matplotlib plots are a nice-to-have but not required for the first version — the text report is the priority.

## Part 4: Field Test Protocol Document

Create `C:\Projects\sentry-rf\FIELD_TEST_PROTOCOL.md`:

```markdown
# SENTRY-RF Field Test Protocol

## Equipment
- SENTRY-RF with external antenna, SD card, GPS antenna facing sky
- JUH-MAK-IN JAMMER on a tripod or fixed position
- Laptop for serial monitoring and test control
- USB cables, power

## Setup
1. Place JJ on a tripod or table at the test origin point
2. Connect JJ to laptop via USB (COM6)
3. Power on SENTRY-RF, connect to laptop (COM9)
4. Wait for GPS 3D fix (check serial output)
5. Wait for warmup to complete (check for [WARMUP] message)
6. Verify SD card logging (check for log file creation)

## Test 1: Baseline (No Transmitter)
1. Ensure JJ is OFF or in quiet mode ('q')
2. Record 5 minutes of baseline data
3. Walk around the test area to capture the RF environment
4. **PASS criteria:** Max threat ADVISORY, max div ≤ 2, Pfa < 5%

## Test 2: Detection at Distance (ELRS)
For each distance (10m, 50m, 100m, 250m, 500m, 1000m):
1. Place SENTRY-RF at the measured distance from JJ
2. Start JJ in ELRS mode ('e' on COM6)
3. Record 60 seconds of data
4. Stop JJ ('q')
5. Record 30 seconds of clear-down data
6. Note: time to WARNING, time to CRITICAL, max diversity, rapid-clear time

## Test 3: Power Variation
At 100m distance:
1. JJ at 100mW (default): record 60s
2. JJ at 25mW ('p' then power setting): record 60s
3. JJ at 1W (if available): record 60s

## Test 4: GNSS Baseline
1. Record GPS integrity data for 10 minutes with no transmitter
2. Check jamInd, spoofDetState, C/N0 standard deviation
3. **PASS criteria:** jamInd < 50, spoofDetState = 1 (clean), C/N0 σ > 3 dB-Hz

## Test 5: WiFi Remote ID
1. Start JJ in Remote ID mode ('r')
2. Walk SENTRY-RF to 50m, 100m, 200m, 500m
3. Record whether WiFi RID is detected at each distance

## Data Collection
After each test:
1. Copy the .jsonl file from SD card to laptop
2. Run: python field_test_analyzer.py <file.jsonl> --distance <N> --label <test>
3. Record results in the summary table below

## Summary Table

| Test | Distance | Power | Time-to-WARNING | Time-to-CRITICAL | Clear Time | Max Div | Baseline Div | Pd | Pfa |
|------|----------|-------|-----------------|------------------|------------|---------|-------------|-----|-----|
|      |          |       |                 |                  |            |         |             |     |     |
```

## What NOT to change

- Do NOT modify the detection engine logic (assessThreat, diversity tracker)
- Do NOT modify CAD scan parameters
- Do NOT enable FSK Phase 3
- Do NOT change diversity thresholds (field data will inform changes AFTER testing)
- Do NOT change the RSSI sweep

The ONLY code changes in this sprint are:
1. GPS_MIN_CNO = 15 in sentry_config.h
2. Enhanced JSONL logging in the data logger
3. Field test analyzer Python script
4. Field test protocol document

## Acceptance criteria

1. **PASS:** GPS_MIN_CNO set to 15. C/N0 uniformity check gated behind GPS_MIN_CNO >= 15.
2. **PASS:** SENTRY-RF writes .jsonl log to SD card with per-cycle data including div, threat, GPS, GNSS fields.
3. **PASS:** field_test_analyzer.py parses a .jsonl file and prints a formatted report.
4. **PASS:** FIELD_TEST_PROTOCOL.md created with complete test procedure.
5. **PASS:** All three build targets compile clean.
6. **PASS:** Boot, wait for warmup, verify SD card has a new log file with valid JSON lines.

Build all three targets. Flash to COM9. Verify SD logging works by capturing 30 seconds and checking the file.
