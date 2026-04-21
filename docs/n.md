Create a field test analysis script at C:\Projects\sentry-rf\Sentry-RF-main\tools\field_analyzer.py.
This script reads SENTRY-RF JSONL log files from SD card and produces a field test report.
Input: One or more .jsonl files passed as arguments (or all *.jsonl files in a specified directory). Each line is a JSON object with these possible fields (some optional per Phase J/L additions):
t (ms), c (cycle), threat (0-3), score, div, conf, taps, peak_mhz, peak_dbm, peak_bw, peak_bins, lat, lon, fix, sv, jam, spoof, cno_sd, and optionally rid_id, rid_dlat, rid_dlon, rid_dalt, rid_olat, rid_olon. Also special event lines with event field: "selftest", "mode_change".
What the script produces:

Console summary — printed to stdout:

Session info: firmware version (from selftest event), boot count, total cycles, session duration
Threat level breakdown: % of time at each level (CLEAR/ADVISORY/WARNING/CRITICAL)
Detection events: count of transitions to WARNING+, average time-to-WARNING from CLEAR, max threat reached
False alarm estimate: WARNING+ events that returned to CLEAR within 30 seconds (likely false positives)
GNSS health: % of time with 3D fix, average SVs, jam events (jam > 50), spoof events (spoof > 0)
Remote ID events: count of decoded RID beacons, unique UAS IDs seen
Peak signal stats: strongest peak seen (freq + dBm), median noise floor estimate


HTML report saved to {input_filename}_report.html:

Threat level over time (line chart)
RSSI peak over time (line chart)
CAD diversity over time (line chart)
GPS fix quality over time (SV count line chart)
C/N0 standard deviation over time (line chart, highlight values below 3.0 in red)
If RID data present: table of unique drones seen (UAS ID, position, altitude)
All charts use matplotlib, saved as base64-embedded PNG in the HTML so the file is self-contained


CSV summary saved to {input_filename}_summary.csv — one row per detection event with: timestamp, threat level reached, peak frequency, peak RSSI, duration of event, whether it resolved to CLEAR or was still active at log end.

Usage:
python tools/field_analyzer.py logs/field_001.jsonl
python tools/field_analyzer.py logs/          # process all .jsonl in directory
python tools/field_analyzer.py logs/field_001.jsonl --no-html  # console only
Requirements: matplotlib, pandas (add to a new tools/requirements_analysis.txt).
Constraints:

Script must handle malformed/truncated lines gracefully (skip with a warning count)
Must work on Python 3.10+
No firmware changes — Python only
Do not add to platformio.ini

After writing the script, run a syntax check: python -m py_compile tools/field_analyzer.py
Commit with message: "Phase N: field test analyzer script (JSONL → HTML/CSV report)"
Report: script location, requirements file location, syntax check result, commit hash.

