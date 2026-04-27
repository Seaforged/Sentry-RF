"""Run a single test by id against the currently-flashed firmware.

Usage: python run_one.py <A01|A04|...>
"""
import sys, time, traceback
import characterize as ch

SPECS = {
    "A01": {"test_id": "A01", "label": "ELRS_FCC915_200Hz", "cmd": "e1", "group": "A"},
    "A04": {"test_id": "A04", "label": "ELRS_FCC915_25Hz",  "cmd": "e4", "group": "A"},
    "J01": {"test_id": "J01", "label": "LoRaWAN_US915_infrastructure", "cmd": "i", "group": "J"},
    "I01": {"test_id": "I01", "label": "WiFi_ODID_only", "cmd": "y1", "group": "I"},
}

def main(tid):
    t = SPECS[tid]
    ch.glog(f"=== {tid}-ONLY RUN ===")
    try:
        unsupported = ch.phase0_discovery()
    except Exception as e:
        ch.glog(f"PHASE 0 FAILED: {e}\n{traceback.format_exc()}")
        unsupported = set()

    r = ch.run_one_test(t, unsupported)
    sw = r.get("sentry_tx_window") or {}
    peak = sw.get("peak_threat", "-")
    transitions = sw.get("threat_transitions", [])
    ch.stop_readers(timeout=2)

    print()
    print("-" * 60)
    print(f"{tid} RESULT")
    print("-" * 60)
    print(f"  peak_threat          : {peak}")
    print(f"  time_to_advisory_s   : {sw.get('time_to_advisory_s')}")
    print(f"  time_to_warning_s    : {sw.get('time_to_warning_s')}")
    print(f"  cad_at_peak          : {sw.get('cad_at_peak')}")
    print(f"  transitions          : {len(transitions)}")
    for tr in transitions[:8]:
        print(f"    {tr['from']} -> {tr['to']} at {tr['time_from_tx_start_s']}s")
    print()
    return 0

if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1] if len(sys.argv) > 1 else "A04"))
