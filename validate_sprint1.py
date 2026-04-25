"""Sprint 1 (v3 Tier 1) validation: warmup graduation fix.

Two scenarios:

Scenario A — "JJ-during-warmup":
    Start JJ ELRS FCC915 200 Hz before resetting SENTRY-RF, capture the boot
    + warmup serial window, and verify:
      (a) `[WARMUP] suppressed graduation` appears at least once
      (b) no drone-band sub-GHz freq (902-928 MHz, ELRS_FCC915 channel grid)
          shows up in any `[WARMUP] progressive ambient tag` line after
          warmup completion.

Scenario B — "post-warmup regression + improvement":
    Run two standard characterize.py tests:
      A04 (e4, ELRS FCC915 25 Hz) — must still reach WARNING (regression)
      A01 (e1, ELRS FCC915 200 Hz) — must reach >= ADVISORY (was CLEAR)

Run with hardware connected: COM14 = SENTRY-RF, COM6 = JJ.
"""
import json, time, re, traceback
from pathlib import Path

import characterize as ch

ART = ch.ART
SCEN_A_LOG = ART / "sprint1_scenario_a_warmup.log"

# Tracked warmup window after RTS reset (covers 50 s warmup + margin).
SCENARIO_A_CAPTURE_S = 75.0
# JJ pre-roll: how long JJ should be transmitting before we reset SENTRY,
# so its FHSS sequence is well underway when warmup begins.
JJ_PREROLL_S = 5.0


def scenario_a_jj_during_warmup():
    """Boot SENTRY-RF with JJ ELRS 200 Hz already transmitting; capture
    boot+warmup window; verify suppression message and absence of drone
    anchors in post-warmup ambient tags."""
    ch.glog("=== SCENARIO A: JJ ELRS 200 Hz during SENTRY-RF warmup ===")

    # Open both readers (used to drive JJ from main thread; sentry reader
    # captures the warmup window).
    ch.set_log(ch._sentry_log, SCEN_A_LOG)
    ch.set_log(ch._jj_log, ART / "sprint1_scenario_a_jj.log")
    ch.start_readers()
    time.sleep(0.5)

    # Pre-roll: start JJ ELRS FCC915 200 Hz. ELRS_FCC915 channels are
    # 903.5-926.9 MHz, 40 channels. JJ will print its protocol banner on
    # start.
    with ch._lock: ch._jj_buf.clear()
    ch.jj_send(b"e1\r\n", label="ScenA 'e1' (pre-roll)")
    time.sleep(JJ_PREROLL_S)

    # Hard reset SENTRY-RF via RTS while JJ is mid-FHSS. We must stop the
    # sentry reader first because reset_sentry opens its own Serial(COM14).
    ch.stop_readers(timeout=1.2)
    ch.reset_sentry()
    t_reset = time.time()
    ch.start_readers()

    # Capture the full warmup window.
    ch.glog(f"[ScenA] capturing {SCENARIO_A_CAPTURE_S} s of SENTRY-RF serial after reset")
    t_end = t_reset + SCENARIO_A_CAPTURE_S
    while time.time() < t_end:
        time.sleep(0.5)

    # Stop JJ.
    ch.jj_send(b"q\r\n", label="ScenA 'q'")
    time.sleep(2.0)

    # Pull captured lines.
    sentry_lines = [(ts, ln) for ts, ln in ch.snap(ch._sentry_buf)
                    if t_reset <= ts < t_end + 5.0]

    ch.set_log(ch._sentry_log, None)
    ch.set_log(ch._jj_log, None)
    ch.stop_readers(timeout=1.2)

    # ── Analyze ──
    suppressed_lines = [ln for _, ln in sentry_lines
                        if "[WARMUP] suppressed graduation" in ln]
    progressive_tag_lines = [ln for _, ln in sentry_lines
                             if "[WARMUP] progressive ambient tag" in ln]
    warmup_complete_seen = any("[WARMUP] Complete" in ln or
                               "[BOOT] Warmup complete" in ln
                               for _, ln in sentry_lines)

    # Drone-band sub-GHz freqs are 902.0-928.0 MHz (ELRS_FCC915 lives at
    # 903.5-926.9). Helium PoC, LoRaWAN gateways, Meshtastic also live in
    # 902-928 — but with JJ actively driving ELRS 200 Hz at 10 dBm the
    # dominant signal during warmup is JJ. If a 902-928 MHz freq shows up
    # in a progressive ambient tag, the fix is failing.
    drone_band_tags = []
    freq_re = re.compile(r"progressive ambient tag:\s*([\d.]+)\s*MHz")
    for ln in progressive_tag_lines:
        m = freq_re.search(ln)
        if not m: continue
        f = float(m.group(1))
        if 902.0 <= f <= 928.0:
            drone_band_tags.append(f)

    print()
    print("-" * 60)
    print("SCENARIO A RESULTS")
    print("-" * 60)
    print(f"  warmup_complete_seen           : {warmup_complete_seen}")
    print(f"  [WARMUP] suppressed graduation : {len(suppressed_lines)} occurrence(s)")
    for ln in suppressed_lines[:5]:
        print(f"      {ln}")
    print(f"  progressive ambient tag (any)  : {len(progressive_tag_lines)} occurrence(s)")
    print(f"  drone-band tags (902-928 MHz)  : {len(drone_band_tags)} -> {drone_band_tags[:8]}")

    crit1_pass = (len(drone_band_tags) == 0)
    crit2_pass = (len(suppressed_lines) >= 1)
    print()
    print(f"  Criterion 1 (no drone freq in post-warmup ambient): {'PASS' if crit1_pass else 'FAIL'}")
    print(f"  Criterion 2 ([WARMUP] suppressed graduation seen) : {'PASS' if crit2_pass else 'FAIL'}")
    print(f"  warmup_complete_seen sanity gate                  : {'OK' if warmup_complete_seen else 'NO BANNER (test invalid)'}")

    return {
        "criterion_1_pass": crit1_pass,
        "criterion_2_pass": crit2_pass,
        "warmup_complete_seen": warmup_complete_seen,
        "suppressed_count": len(suppressed_lines),
        "progressive_tag_count": len(progressive_tag_lines),
        "drone_band_tags": drone_band_tags,
    }


def scenario_b_post_warmup():
    """Run A04 (regression) then A01 (improvement) via the standard
    characterize.py per-test flow."""
    ch.glog("=== SCENARIO B: post-warmup A04 regression + A01 improvement ===")

    # phase 0: discover JJ command surface and warm up readers.
    try:
        unsupported = ch.phase0_discovery()
    except Exception as e:
        ch.glog(f"PHASE 0 FAILED: {e}\n{traceback.format_exc()}")
        unsupported = set()

    targets = [
        {"test_id": "A04", "label": "ELRS_FCC915_25Hz",  "cmd": "e4", "group": "A"},
        {"test_id": "A01", "label": "ELRS_FCC915_200Hz", "cmd": "e1", "group": "A"},
    ]

    results = []
    for i, t in enumerate(targets, 1):
        ch.glog(f"[ScenB {i}/{len(targets)}] {t['test_id']} starting")
        r = ch.run_one_test(t, unsupported)
        peak = (r.get("sentry_tx_window") or {}).get("peak_threat", "-")
        ch.glog(f"[ScenB {i}/{len(targets)}] {t['test_id']} -> peak={peak} status={r.get('status')}")
        results.append((t, r))
        time.sleep(ch.COOLDOWN_S)

    ch.stop_readers(timeout=2)

    # ── Analyze ──
    a04 = next((r for (t, r) in results if t["test_id"] == "A04"), None)
    a01 = next((r for (t, r) in results if t["test_id"] == "A01"), None)
    a04_peak = (a04.get("sentry_tx_window") or {}).get("peak_threat") if a04 else None
    a01_peak = (a01.get("sentry_tx_window") or {}).get("peak_threat") if a01 else None

    SEV = ch.SEV
    crit3_pass = (a04_peak == "WARNING" or
                  (a04_peak in SEV and SEV[a04_peak] >= SEV["WARNING"]))
    crit4_pass = (a01_peak in SEV and SEV[a01_peak] >= SEV["ADVISORY"])

    print()
    print("-" * 60)
    print("SCENARIO B RESULTS")
    print("-" * 60)
    print(f"  A04 (regression) peak_threat : {a04_peak}  expected WARNING")
    print(f"  A01 (improvement) peak_threat: {a01_peak}  expected >= ADVISORY")
    print()
    print(f"  Criterion 3 (A04 still WARNING)          : {'PASS' if crit3_pass else 'FAIL'}")
    print(f"  Criterion 4 (A01 reaches >= ADVISORY)    : {'PASS' if crit4_pass else 'FAIL'}")

    return {
        "criterion_3_pass": crit3_pass,
        "criterion_4_pass": crit4_pass,
        "a04_peak": a04_peak,
        "a01_peak": a01_peak,
    }


def main():
    ch.glog("=== SPRINT 1 VALIDATION ===")
    a = scenario_a_jj_during_warmup()
    time.sleep(ch.COOLDOWN_S)
    b = scenario_b_post_warmup()

    overall = {**a, **b}
    print()
    print("=" * 60)
    print("SPRINT 1 OVERALL")
    print("=" * 60)
    crits = [
        ("1: no drone freq in post-warmup ambient", a["criterion_1_pass"]),
        ("2: [WARMUP] suppressed graduation seen ", a["criterion_2_pass"]),
        ("3: A04 still WARNING                   ", b["criterion_3_pass"]),
        ("4: A01 reaches >= ADVISORY             ", b["criterion_4_pass"]),
    ]
    for name, ok in crits:
        print(f"  Criterion {name}: {'PASS' if ok else 'FAIL'}")
    all_pass = all(ok for _, ok in crits)
    print()
    print(f"  SPRINT 1 OVERALL: {'PASS' if all_pass else 'FAIL'}")
    print("=" * 60)

    (ART / "sprint1_validation.json").write_text(json.dumps(overall, indent=2))
    return 0 if all_pass else 1


if __name__ == "__main__":
    raise SystemExit(main())
