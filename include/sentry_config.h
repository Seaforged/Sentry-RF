#ifndef SENTRY_CONFIG_H
#define SENTRY_CONFIG_H

// =====================================================================
//  SENTRY-RF Configuration — All tunable detection constants
//  Adjust these for field calibration without hunting through code.
//  Hardware constants (pins, SPI, I2C) remain in board_config.h.
//  FreeRTOS task config remains in task_config.h.
// =====================================================================

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
static const int RSSI_SWEEP_INTERVAL     = 3;       // run RSSI sweep every Nth CAD cycle

// ── Rapid-Clear Path ──────────────────────────────────────────────────
static const int RAPID_CLEAR_CLEAN_CYCLES = 4;      // consecutive clean cycles to force CLEAR

// ── CAD Tap Persistence ───────────────────────────────────────────────
static const int   MAX_TAPS              = 32;      // max concurrent CAD taps
static const int   TAP_CONFIRM_HITS      = 3;       // consecutive hits to confirm as drone
static const int   TAP_EXPIRE_MISSES     = 3;       // consecutive misses to deactivate
static const float TAP_FREQ_TOL          = 0.2f;    // MHz — +/-200 kHz to match existing tap

// ── CAD Channel Allocation ────────────────────────────────────────────
static const int CAD_CH_SF6  = 60;   // ELRS 200Hz — covers 80ch in 1.3 cycles
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
static const int   MAX_AMBIENT_TAPS               = 32;     // max ambient tap entries
static const float AMBIENT_FREQ_TOLERANCE         = 0.2f;   // +/-MHz match window
static const unsigned long AMBIENT_AUTOLEARN_MS   = 15000;  // ms before confirmed tap auto-learned (was 60s)

// ── FSK Detection (Crossfire/FrSky) ──────────────────────────────────
static const float FSK_DETECT_THRESHOLD_DBM = -70.0f;  // dBm — field sensitivity (bench: raise to -50)
static const int   FSK_DWELL_US             = 2500;    // microseconds per channel dwell

// ── RSSI-Guided CAD ──────────────────────────────────────────────────
static const float RSSI_GUIDED_THRESH_DB = 8.0f;   // dB above NF for guided CAD targeting
static const int   RSSI_GUIDED_MAX_BINS  = 8;      // max elevated bins to CAD-check

// ── Frequency Diversity (FHSS Detection) ─────────────────────────────
static const unsigned long DIVERSITY_WINDOW_MS = 3000;    // 3-second sliding window
static const int DIVERSITY_WARNING             = 3;       // used by pursuit mode activation
static const int MAX_DIVERSITY_SLOTS           = 32;      // max tracked distinct frequencies
// NOTE: Threat level mapping is now via SCORE_* thresholds, not DIVERSITY_*.

// ── AAD: Persistence Gate + Diversity Velocity ───────────────────────
static const uint8_t PERSISTENCE_MIN_CONSECUTIVE = 5;     // consecutive high-diversity cycles before qualifying (was 3, raised after 28-min soak showed ambient sustaining 4)
static const uint8_t PERSISTENCE_MIN_DIVERSITY   = 3;     // min raw diversity to count as "sustained"
static const uint8_t DIVERSITY_VELOCITY_WINDOW   = 3;     // scan cycles for velocity calculation
static const uint8_t DIVERSITY_VELOCITY_FHSS_MIN = 2;     // min velocity for full FHSS confidence
static const uint8_t DIVERSITY_VELOCITY_BONUS_MIN = 5;    // min velocity for bonus confidence points
static const uint8_t DIVERSITY_VELOCITY_BONUS_PTS = 10;   // bonus confidence points for high velocity

// ── RSSI Ambient Filter ──────────────────────────────────────────────
static const int   AMBIENT_HISTORY_DEPTH   = 10;    // sweeps of history before baseline
static const float AMBIENT_VARIANCE_THRESH = 5.0f;  // dB^2 max variance for baseline lock
static const float AMBIENT_UNLOCK_SHIFT    = 6.0f;  // dB drift to unlock a baseline bin
static const float AMBIENT_MIN_ABOVE_NF    = 5.0f;  // dB above NF to qualify for baseline

// ── GNSS Integrity ────────────────────────────────────────────────────
static const uint8_t GPS_MIN_CNO           = 15;    // dB-Hz minimum (6=indoor bench, 15=field)
static const float CNO_STDDEV_SPOOF_THRESH = 2.0f;  // dB-Hz — below this = spoofing suspected
static const int   MIN_ELEV_FOR_CNO        = 20;    // degrees — exclude low-elevation sats

// ── Confidence Scoring Weights ────────────────────────────────────────
static const int WEIGHT_DIVERSITY_PER_FREQ = 8;    // per distinct frequency in window
static const int WEIGHT_CAD_CONFIRMED      = 15;   // per confirmed CAD tap
static const int WEIGHT_FSK_CONFIRMED      = 12;   // per confirmed FSK tap
static const int WEIGHT_RSSI_PERSISTENT_US = 10;   // persistent RSSI in 902-928 MHz
static const int WEIGHT_RSSI_PERSISTENT_EU = 5;    // persistent RSSI in 860-886 MHz
static const int WEIGHT_BAND_ENERGY        = 5;    // band energy elevated
static const int WEIGHT_GNSS_ANOMALY       = 15;   // GNSS integrity anomaly
static const int WEIGHT_24GHZ_PERSISTENT   = 10;   // 2.4 GHz persistent (LR1121)
static const int WEIGHT_REMOTE_ID          = 20;   // WiFi Remote ID detected
static const int SCORE_ADVISORY            = 8;    // score threshold for ADVISORY
static const int SCORE_WARNING             = 24;   // score threshold for WARNING (div=3 → 3×8)
static const int SCORE_CRITICAL            = 40;   // score threshold for CRITICAL (div=5 → 5×8)

// ── Fast-Detect Scoring Bonus ────────────────────────────────────────
static const uint8_t FAST_DETECT_MIN_DIVERSITY = 5;  // raw diversity threshold for fast path
static const uint8_t FAST_DETECT_MIN_CONF      = 1;  // confirmed CAD taps for fast path
static const uint8_t WEIGHT_FAST_DETECT        = 20; // bonus points when both thresholds met

// ── Buzzer / Alert ────────────────────────────────────────────────────
static const unsigned long MUTE_DURATION_MS  = 300000; // 5 minutes
static const unsigned long REMINDER_INTERVAL = 30000;  // 30 seconds

#endif // SENTRY_CONFIG_H
