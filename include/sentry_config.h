#ifndef SENTRY_CONFIG_H
#define SENTRY_CONFIG_H

#include <stdint.h>

// =====================================================================
//  SENTRY-RF Configuration — All tunable detection constants
//  Adjust these for field calibration without hunting through code.
//  Hardware constants (pins, SPI, I2C) remain in board_config.h.
//  FreeRTOS task config remains in task_config.h.
// =====================================================================

// ── Phase H: Operational modes ────────────────────────────────────────
// STANDARD:   default — full CAD + RSSI + WiFi + OLED + buzzer + LED
// COVERT:     RF scanning only; WiFi deinitialized, OLED off, buzzer
//             and LED suppressed. Serial + SD logging stay active.
// HIGH_ALERT: CAD every cycle; RSSI sweep skipped except every
//             HIGH_ALERT_RSSI_INTERVAL_MS. Buzzer fires immediately on
//             any WARNING/CRITICAL event (no reminder debounce).
enum OperatingMode : uint8_t {
    MODE_STANDARD   = 0,
    MODE_COVERT     = 1,
    MODE_HIGH_ALERT = 2
};

// RSSI sweep period while in HIGH_ALERT (ms). Still runs periodically as
// a sanity check; the normal RSSI_SWEEP_INTERVAL_MS (8000) is replaced by
// this larger value to free scan budget for back-to-back CAD cycles.
#define HIGH_ALERT_RSSI_INTERVAL_MS  10000

// ── RSSI Detection Thresholds ─────────────────────────────────────────
static const float PEAK_THRESHOLD_DB     = 10.0f;  // dB above noise floor for peak extraction
static const float PEAK_ABS_FLOOR_DBM    = -85.0f; // dBm minimum for RSSI peak
static const int   MAX_PEAKS             = 8;       // max peaks extracted per sweep

// NOTE: ANTENNA_CHECK_THRESHOLD_DBM lives in board_config.h — noise floor
// differs per chipset (SX1262 ~-110 dBm, LR1121 ~-127 dBm) so the threshold
// must be board-specific to avoid false positives.

// Variance floor for the antenna self-test. This is a SANITY gate, not the
// primary discriminator — bare SMA stubs in strong RF can still show 15-20 dB
// variance due to frequency-selective pickup. 3 dB just catches completely
// dead reception. The per-board ANTENNA_CHECK_THRESHOLD_DBM in board_config.h
// is the real discriminator. Variance is logged for operator diagnostics.
static const float ANTENNA_CHECK_VARIANCE_DB = 3.0f;

// ── Band Energy Trending (902-928 MHz) ────────────────────────────────
static const int   BAND_ENERGY_HISTORY   = 10;      // sweeps in rolling average
static const float BAND_ENERGY_THRESH_DB = 8.0f;    // dB above baseline (5.0 caused bench false WARNING)

// ── RSSI Persistence ──────────────────────────────────────────────────
static const int   MAX_TRACKED           = 8;       // max tracked persistent signals
static const float TRACK_FREQ_TOLERANCE  = 0.2f;    // MHz — +/-200 kHz for matching
static const int   PERSIST_THRESHOLD     = 3;       // consecutive sweeps to confirm
static const int   MAX_PROTO_TRACKED     = 14;      // max tracked protocol signatures

// ── Threat Timing ─────────────────────────────────────────────────────
static const unsigned long COOLDOWN_MS   = 5000;    // ms before threat decays one level (was 15000)

// ── Scan Pacing (Phase F: wall-clock timers) ──────────────────────────
// Replaces the cycle-count gates used pre-Phase-F. Each period must be
// strictly greater than that board's short-cycle wall time for the
// timer to actually skip cycles — otherwise the gate fires every
// iteration and degrades into "no gate".
//
// LR24_CAD_PERIOD_MS is only consumed inside #ifdef BOARD_T3S3_LR1121
// blocks in cad_scanner.cpp, so it does NOT need a per-board branch
// here. SX1262 targets never reach the 2.4 GHz code path.
#define LR24_CAD_PERIOD_MS       6000

// RSSI_SWEEP_INTERVAL_MS is consumed in shared loRaScanTask code in
// main.cpp. Both SX1262 and LR1121 need a period strictly greater than
// their long-cycle time (cycle-with-sweep), otherwise the timer expires
// during the same iteration and the gate degrades to "fire every cycle".
// Measured long cycles: LR1121 ~4.3 s, SX1262 ~3.9 s. 8000 ms leaves
// comfortable headroom on both boards and produces roughly the old
// every-3rd-cycle cadence at each board's native tempo. Also: the timer
// is advanced AFTER scannerSweep() completes (see main.cpp) so the
// period measures sweep-end → next-fire-start, not start → start.
#define RSSI_SWEEP_INTERVAL_MS   8000

// ── Rapid-Clear Path ──────────────────────────────────────────────────
// Rapid-clear: force CLEAR after this many ms of candidate-CLEAR silence.
// Ported to the candidate FSM in Phase G.1b — previously owned by the
// legacy assessThreat() path. See detection_engine.cpp rapidClearSinceMs.
static const unsigned long RAPID_CLEAR_CLEAN_MS = 5000;

// ── CAD Tap Persistence ───────────────────────────────────────────────
static const int   MAX_TAPS              = 32;      // max concurrent CAD taps
static const int   TAP_CONFIRM_HITS      = 3;       // consecutive hits to confirm as drone
static const int   TAP_EXPIRE_MISSES     = 3;       // consecutive misses to deactivate
static const float TAP_FREQ_TOL          = 0.2f;    // MHz — +/-200 kHz to match existing tap

// ── CAD Channel Allocation ────────────────────────────────────────────
static const int CAD_CH_SF6  = 80;   // ELRS 200Hz — full 80ch coverage every cycle (v1.6.1 coverage-gap fix)
static const int CAD_CH_SF7  = 40;   // ELRS 150Hz — covers 80ch in 2 cycles
static const int CAD_CH_SF8  = 20;   // ELRS 100Hz
static const int CAD_CH_SF9  = 10;
static const int CAD_CH_SF10 = 4;
static const int CAD_CH_SF11 = 2;
static const int CAD_CH_SF12 = 2;
// Total: ~138 normal, up to 160 in pursuit mode
static const int FSK_CH      = 4;

// ── ELRS/Crossfire Channel Plans ──────────────────────────────────────
static const int ELRS_915_CHANNELS = 80;    // US: 902-928 MHz, 325 kHz spacing
static const int ELRS_868_CHANNELS = 50;    // EU: 860-886 MHz, 520 kHz spacing
static const int CRSF_CHANNELS     = 100;   // 260 kHz spacing

// ── Ambient Warmup Filter ─────────────────────────────────────────────
static const unsigned long AMBIENT_WARMUP_MS      = 50000;  // ms warmup time
static const unsigned long AMBIENT_EARLY_EXIT_MS  = 20000;  // ms minimum before early exit
static const int   MAX_AMBIENT_TAPS               = 64;     // max ambient tap entries
static const float AMBIENT_FREQ_TOLERANCE         = 0.2f;   // +/-MHz match window
static const unsigned long AMBIENT_AUTOLEARN_MS   = 15000;  // ms before confirmed tap auto-learned

// ── FSK Detection (Crossfire/FrSky) ──────────────────────────────────
static const float FSK_DETECT_THRESHOLD_DBM = -70.0f;  // dBm — field sensitivity (bench: raise to -50)
static const int   FSK_DWELL_US             = 2500;    // microseconds per channel dwell

// ── RSSI-Guided CAD ──────────────────────────────────────────────────
static const float RSSI_GUIDED_THRESH_DB = 8.0f;   // dB above NF for guided CAD targeting
static const int   RSSI_GUIDED_MAX_BINS  = 8;      // max elevated bins to CAD-check

// ── Frequency Diversity (FHSS Detection) ─────────────────────────────
static const unsigned long DIVERSITY_WINDOW_MS = 8000;    // 8s window — covers ~3 CAD cycles at 2.7s each (v1.6.1 FHSS fix)
static const int DIVERSITY_WARNING             = 3;       // used by pursuit mode activation
static const int MAX_DIVERSITY_SLOTS           = 32;      // max tracked distinct frequencies
// NOTE: Threat level mapping is now via SCORE_* thresholds, not DIVERSITY_*.

// ── AAD: Persistence Gate + Diversity Velocity ───────────────────────
static const uint8_t PERSISTENCE_MIN_CONSECUTIVE = 5;     // consecutive high-diversity cycles (legacy cycle-based gate)
static const unsigned long PERSISTENCE_MIN_MS    = 5000;  // min wall-clock time for persistence (board-parity gate — 5s filters infrastructure spikes while allowing fast escalation)
static const uint8_t PERSISTENCE_MIN_DIVERSITY   = 2;     // min raw diversity to count as "sustained" (v1.6.1: lowered from 3 because ambient filter consumes most of the frequency space)
static const uint8_t DIVERSITY_VELOCITY_WINDOW   = 3;     // legacy (kept for array sizing in build)
static const unsigned long DIVERSITY_VELOCITY_WINDOW_MS = 5000; // 5s wall-clock window for velocity
static const uint8_t DIVERSITY_VELOCITY_FHSS_MIN = 2;     // min velocity for full FHSS confidence
static const uint8_t DIVERSITY_VELOCITY_BONUS_MIN = 5;    // min velocity for bonus confidence points
static const uint8_t DIVERSITY_VELOCITY_BONUS_PTS = 10;   // bonus confidence points for high velocity

// ── FHSS Frequency-Spread Tracker (v1.6.1) ───────────────────────────
// Second diversity path that does NOT require consecutiveHits>=2 on the
// same freq. Counts UNIQUE (freq,sf) CAD hits across a sliding ring of
// cycles. When spread >= threshold, each unique freq is pushed into the
// existing recordDiversityHit() path so normal persistence/velocity/
// escalation logic applies. Addresses FHSS-invisibility regression found
// during JJ v2.0.0 real-signal testing 2026-04-11.
static const uint8_t FHSS_WINDOW_CYCLES    = 3;     // legacy (kept for array sizing in build)
static const unsigned long FHSS_WINDOW_MS  = 5000;  // 5s wall-clock window for FHSS spread tracker
static const uint8_t FHSS_UNIQUE_THRESHOLD = 3;     // min unique freqs to fire (lowered from 4 for narrow-hop protocols like mLRS)

// ── RSSI Ambient Filter ──────────────────────────────────────────────
static const int   AMBIENT_HISTORY_DEPTH   = 10;    // sweeps of history before baseline
static const float AMBIENT_VARIANCE_THRESH = 5.0f;  // dB^2 max variance for baseline lock
static const float AMBIENT_UNLOCK_SHIFT    = 6.0f;  // dB drift to unlock a baseline bin
static const float AMBIENT_MIN_ABOVE_NF    = 5.0f;  // dB above NF to qualify for baseline

// ── GNSS Integrity ────────────────────────────────────────────────────
// dB-Hz minimum satellite signal strength. INDOOR TESTING: 6. FIELD/PRODUCTION: 15-20.
static const uint8_t GPS_MIN_CNO           = 15;
static const float CNO_STDDEV_SPOOF_THRESH = 2.0f;  // dB-Hz — below this = spoofing suspected
static const int   MIN_ELEV_FOR_CNO        = 20;    // degrees — exclude low-elevation sats
// Phase K: the C/N0 uniformity spoofing detector is suppressed when
// GPS_MIN_CNO < 15. Indoor/attenuated constellations naturally cluster
// within ±2 dB-Hz and trip the uniformity check as false-positive spoofing.
// Field thresholds (15+) spread the real distribution wide enough to let
// the check fire legitimately.

// Position jump spoofing detector — >100 m step with hAcc <10 m = teleport.
// Only counts when the fix is otherwise high-confidence, so natural drift
// during poor-fix conditions doesn't flag.
#define GNSS_POSITION_JUMP_THRESHOLD_M  100.0f
#define GNSS_POSITION_JUMP_HACC_MAX_M   10.0f

// RF-GNSS temporal correlation window. GNSS anomaly evidence only attaches
// to a candidate when an RF-side ADVISORY crossing happened in the last 30s
// — prevents a standalone GNSS anomaly (e.g. driving under a bridge) from
// promoting an unrelated infrastructure-seeded candidate.
#define GNSS_RF_CORRELATION_WINDOW_MS  30000

// ── Confidence Scoring Weights (Phase G: candidate engine only) ──────
// The legacy two-layer scorer was removed in Phase G along with
// assessThreat(). The constants below are the ones still consumed by
// the candidate engine. Candidate engine scoring weights for CAD/FHSS
// evidence live in the CANDIDATE ENGINE WEIGHTS section further down
// (FAST_SCORE_* and POLICY_* macros).
static const int WEIGHT_CONFIRM_PEAK           = 5;   // RSSI peak above floor
static const int WEIGHT_CONFIRM_PROTO          = 15;  // protocol signature matched
static const int WEIGHT_CONFIRM_REMOTE_ID      = 30;  // WiFi Remote ID detected
static const int WEIGHT_CONFIRM_BAND_ENERGY    = 10;  // band energy elevated
static const int WEIGHT_CONFIRM_GNSS_TEMPORAL  = 25;  // GNSS anomaly temporally correlated

// Phase G: the legacy per-source weights (WEIGHT_DIVERSITY_PER_FREQ,
// WEIGHT_CAD_CONFIRMED, WEIGHT_FSK_CONFIRMED, WEIGHT_RSSI_PERSISTENT_US/EU,
// WEIGHT_BAND_ENERGY, WEIGHT_GNSS_ANOMALY, WEIGHT_24GHZ_PERSISTENT,
// WEIGHT_REMOTE_ID, WEIGHT_FAST_PERSISTENT_DIV, WEIGHT_FAST_FHSS_VELOCITY,
// WEIGHT_FAST_DETECT, SCORE_ADVISORY/WARNING/CRITICAL,
// FAST_DETECT_MIN_DIVERSITY/MIN_CONF, FAST_ADVISORY_THRESH,
// FAST_WARNING_THRESH, CONFIRM_WARNING_THRESH, CONFIRM_CRITICAL_THRESH,
// CONFIRM_ADVISORY_THRESH) were removed with the assessThreat() scorer.
// The candidate engine's POLICY_ADVISORY_FAST / POLICY_WARNING_* /
// POLICY_CRITICAL_* thresholds defined below replace them.

// ── Buzzer / Alert ────────────────────────────────────────────────────
static const unsigned long MUTE_DURATION_MS  = 300000; // 5 minutes
static const unsigned long REMINDER_INTERVAL = 30000;  // 30 seconds

// ── Phase A.4 / Phase C: Candidate Engine (Detection Engine v2.0) ────
// Spec: docs/SENTRY-RF_Detection_Engine_v2_SPEC.md Part 4

// --- Candidate engine sizing ---
#define MAX_CANDIDATES              6
#define CAND_AGE_OUT_MS             12000   // Evict if all evidence dead > 12s
#define CAND_INFRA_PROBATION_MS     AMBIENT_AUTOLEARN_MS
// Treat new sub-GHz candidates as infrastructure-like until they either pick
// up stronger corroboration or survive the ambient auto-learn window. 4 MHz is
// loose enough to allow modest anchor drift while still distinguishing a
// narrow/fixed emitter from wide FHSS spread.
#define CAND_INFRA_NARROW_SPAN_MHZ  4.0f

// --- Association windows ---
#define CAND_ASSOC_SUB_MHZ          1.0f    // Sub-GHz evidence must be within ±1 MHz of anchor
#define CAND_ASSOC_24_MHZ           4.0f    // 2.4 GHz evidence association window ±4 MHz
#define CAND_ASSOC_SUB_TTL_MS       6000    // Sub-GHz evidence attaches only if candidate seen within 6s
#define CAND_ASSOC_24_TTL_MS        5000    // 2.4 GHz evidence attaches only if candidate seen within 5s

// --- Evidence TTLs (milliseconds) ---
#define TTL_CAD_CONFIRMED_MS        2500
// Phase D.1 correction: extended from 2500ms to bridge the timing gap
// between a pending-tap candidate aging out and FHSS diversity arriving.
// On marginal SX1262 acquisitions the FHSS burst can lag the last pending
// tap by 3-5 scan cycles (~5-10s). 6000ms keeps the candidate alive long
// enough for fhssSub evidence to attach and add the diversity bonus.
// Still well below CAND_AGE_OUT_MS=12000 so dead candidates evict on time.
#define TTL_CAD_PENDING_MS          6000
#define TTL_FSK_CONFIRMED_MS        2500
#define TTL_FHSS_SUB_MS             3000
#define TTL_SWEEP_SUB_MS            4500
#define TTL_PROTO_SUB_MS            6000
#define TTL_CAD24_MS                5000
#define TTL_PROTO24_MS              5000
#define TTL_RID_MS                  10000
#define TTL_GNSS_MS                 8000

// --- Scoring policy thresholds ---
#define POLICY_ADVISORY_FAST        15
#define POLICY_WARNING_FAST         40
#define POLICY_WARNING_CONFIRM      5
#define POLICY_CRITICAL_FAST        40
#define POLICY_CRITICAL_CONFIRM     30
#define POLICY_RID_ONLY_ADVISORY    30      // RID confirm score for ADVISORY without RF candidate

// --- Fast score component caps ---
#define FAST_SCORE_CAD_PER_TAP      10
#define FAST_SCORE_CAD_CAP          40
#define FAST_SCORE_FSK_PER_TAP      15
#define FAST_SCORE_FSK_CAP          30
#define FAST_SCORE_DIVERSITY_BONUS  20
// FAST_SCORE_FHSS_BONUS removed — was orphaned (defined but never read).
// FAST_SCORE_DIVERSITY_BONUS is the live FHSS-bonus weight inside computeFastScore().

// ── Phase E: LR1121 fast-FHSS detection ─────────────────────────────────────
// Spec: docs/SENTRY-RF_Detection_Engine_v2_SPEC.md Part 7
// Phase E.1 (cad24Scan extraction + LR24_CAD_PERIOD_MS timer) was attempted
// but reverted — the radio mode tracking issues introduced were not worth the
// 2.4 GHz off-critical-path optimization. That split is deferred to Phase F.

// FHSS cluster evidence TTL — short, refreshes every cycle the cluster signal
// shape is present (anchor + spread + fast-confirmed taps).
#define TTL_FHSS_CLUSTER_MS         3000

// FHSS cluster evidence score (added to fast score). 15 closes the LR1121
// 10-point gap from one confirmed sub-GHz tap (10) + diversity bonus (20) = 30
// to ADVISORY+WARNING (40+) on fast-FHSS signals like ELRS 200Hz.
#define FAST_SCORE_FHSS_CLUSTER     15

// ── Issue 6: 2.4 GHz OFDM plateau detector ───────────────────────────
// OcuSync / Autel C2 broadband video links emit flat-top OFDM across
// ~10 MHz in the 2.4 GHz band, often sitting inside standard WiFi
// channels where isWiFiChannel()'s peak-finder reject drops them. The
// plateau detector scans for contiguous runs of elevated bins on the
// 2.4 GHz sweep (100 bins × 1 MHz) and emits bwWide evidence when the
// OFDM signature is seen persistently across multiple sweeps.
#define OFDM_PLATEAU_THRESHOLD_DB      8.0f   // dB above adaptive noise floor
#define OFDM_PLATEAU_MIN_BINS          10     // ≈10 MHz contiguous at 1 MHz step
#define OFDM_PLATEAU_PERSIST_CYCLES    3      // sweeps in a row before firing

// ── Phase I: Bandwidth discrimination (DJI OcuSync detection) ───────────────
// Applied to the per-peak adjacent-bin count returned by
// countElevatedAdjacentBins(). Bin spacing is 200 kHz on the sub-GHz sweep.
//   count >= BW_WIDE_BIN_THRESHOLD   → BW_WIDE   (DJI OcuSync OFDM, 10+ bins)
//   count >= BW_MEDIUM_BIN_THRESHOLD → BW_MEDIUM (unknown/other, 4-9 bins)
//   otherwise                        → BW_NARROW (ELRS/Crossfire/FrSky, 1-3 bins)
#define BW_WIDE_BIN_THRESHOLD       10
#define BW_MEDIUM_BIN_THRESHOLD      4

// Wide-band candidate evidence — attached only as a confirmer, never seeds
// a new candidate. Weight matches WEIGHT_CONFIRM_BAND_ENERGY since both are
// "elevated RSSI over a wide span" signals of different flavors. TTL matches
// the sweep-derived evidence lifetime.
static const int WEIGHT_CONFIRM_BW_WIDE = 10;
#define TTL_BW_WIDE_MS              4500

// ── Phase K: Boot self-test + scan watchdog ──────────────────────────
// Antenna self-test RSSI floor. If all 10 probe frequencies across 860-930
// MHz read below this, the antenna self-test emits a WARN (boot continues).
// -130 is below the LR1121 hardware clamp so a genuinely disconnected path
// (returning ~-127 dBm sentinel) fails while any real antenna pickup passes.
#define ANTENNA_FLOOR_DBM           -130.0f

// GPS fix acquisition timeout (ms). The boot self-test sets a pending flag;
// if gpsReadTask hasn't cleared it by this deadline, a one-shot [SELFTEST]
// GPS: NO FIX line is logged to serial. Boot is never blocked on GPS.
#define GPS_FIX_TIMEOUT_MS          120000

// Scan-cycle watchdog deadline (ms). loRaScanTask logs a [WATCHDOG] line
// when a single cycle exceeds this wall-clock budget. Log-only — we do NOT
// reset the task, because aborting mid-SPI leaves the radio in an unknown
// state. Raise this if legitimate slow cycles trip the alarm in the field.
#define SCAN_WATCHDOG_MS            5000

// ── Phase L: ZMQ/DragonSync JSON serial output ───────────────────────
// emitZmqJson() prints structured [ZMQ] JSON lines on threat transitions
// and decoded Remote ID events. A companion Python bridge running on the
// host (tools/zmq_bridge.py) re-publishes them as real ZMQ PUB messages
// on tcp://*:4227. Disabling compiles the function to an empty body so
// the call sites become no-ops (LTO elides the call).
#define ENABLE_ZMQ_OUTPUT           1

// ── Phase H: mode accessor API ───────────────────────────────────────
// Defined in main.cpp. Always access via these wrappers — they take
// stateMutex internally so cross-core reads stay consistent.
#ifdef __cplusplus
extern "C" {
#endif
OperatingMode modeGet(void);
void          modeSet(OperatingMode m);
const char*   modeShortLabel(OperatingMode m);  // "STD" / "COV" / "HI-ALT"
const char*   modeLongLabel(OperatingMode m);   // "STANDARD" etc.
#ifdef __cplusplus
}
#endif

#endif // SENTRY_CONFIG_H
