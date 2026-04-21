Paste this into your SENTRY-RF CC session:


Read the following files completely before writing any code: src/rf_scanner.cpp, include/rf_scanner.h, include/sentry_config.h, src/detection_engine.cpp, include/detection_types.h, docs/SENTRY-RF_UPDATED_ROADMAP.md.
Then implement bandwidth discrimination via adjacent-bin counting.
Background (read this carefully):
The current RSSI sweep produces 350 bins across 860–930 MHz (sub-GHz) and a separate 2.4 GHz sweep on the LR1121. Peak extraction currently uses a 3-bin local maximum test — it finds the single highest bin but has no concept of how wide a signal is. DJI OcuSync 2/3/4 uses OFDM that occupies 10+ MHz of spectrum simultaneously, meaning it elevates many adjacent bins at once. A narrowband drone control link (ELRS, Crossfire) elevates only 1–3 bins. We need to count how many consecutive bins are elevated to discriminate wide-band signals from narrow-band ones.
Implementation:

Add a function countElevatedAdjacentBins(const float* rssi, int numBins, int peakIdx, float threshold) to src/rf_scanner.cpp and declare it in include/rf_scanner.h. It should:

Starting from peakIdx, walk left and right counting consecutive bins where rssi[i] > threshold
Return the total count of consecutive elevated bins centered on the peak (including the peak itself)
Stop counting in each direction as soon as a bin drops below threshold


Add a BandwidthClass enum to include/detection_types.h:
BW_NARROW   = 0   // 1–3 bins  (~0–600 kHz) — ELRS, Crossfire, FrSky
BW_MEDIUM   = 1   // 4–9 bins  (~600 kHz–1.8 MHz) — unknown/other
BW_WIDE     = 2   // 10+ bins  (~1.8 MHz+) — DJI OcuSync OFDM
Bin spacing is 200 kHz on the sub-GHz sweep.
Add int adjacentBinCount and BandwidthClass bwClass fields to the RFPeak struct (or whatever struct currently holds peak data — check the actual struct name in the code).
After each RSSI sweep, for every detected peak, call countElevatedAdjacentBins() and populate adjacentBinCount and bwClass on the peak. The threshold to use is noiseFloor + PEAK_THRESHOLD_DB — same threshold already used for peak extraction.
Add two constants to sentry_config.h:
BW_WIDE_BIN_THRESHOLD    10   // bins — above this = BW_WIDE (DJI OcuSync)
BW_MEDIUM_BIN_THRESHOLD   4   // bins — above this = BW_MEDIUM

In detection_engine.cpp, where RSSI peaks are consumed, add logic: if any peak has bwClass == BW_WIDE, emit a serial log line [BW] Wide-band signal detected: {freq} MHz, {count} bins elevated (~{count*0.2} MHz) and attach it as an additional evidence flag on any active candidate that overlaps that frequency band. Do not create a new candidate from bandwidth alone — it is a confirmer, not a primary detector, same as 2.4 GHz evidence.
Add bwClass to the JSONL log output in data_logger.cpp if peak data is currently logged there.

Constraints:

Do not change the peak extraction logic itself — only add the bin count on top of existing peaks
countElevatedAdjacentBins must be O(n) and must not allocate heap memory
The function must work correctly on edge cases: peak at bin 0, peak at bin 349, all bins elevated, no adjacent bins elevated
Do not touch CAD scanner code — this is RSSI-only

Acceptance criteria:

countElevatedAdjacentBins() implemented and declared
BandwidthClass enum added to detection_types.h
adjacentBinCount and bwClass populated on every peak after each sweep
[BW] serial line appears when a wide-band signal is detected
With no transmitter present, no [BW] lines appear (ambient noise stays narrow)
Compile clean for all three targets: pio run -e t3s3 -e heltec_v3 -e t3s3_lr1121

Build all three targets after implementation. Report:

Every file modified and what changed
The exact implementation of countElevatedAdjacentBins() with line numbers
Build output for all three targets
Commit with message: "Phase I: bandwidth discrimination (adjacent-bin counting for DJI OcuSync detection)"