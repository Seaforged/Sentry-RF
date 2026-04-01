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

// ── Band Energy Trending (902-928 MHz) ────────────────────────────────
static const int   BAND_ENERGY_HISTORY   = 10;      // sweeps in rolling average
static const float BAND_ENERGY_THRESH_DB = 5.0f;    // dB above baseline (3.0 causes baseline false WARNING)

// ── RSSI Persistence ──────────────────────────────────────────────────
static const int   MAX_TRACKED           = 8;       // max tracked persistent signals
static const float TRACK_FREQ_TOLERANCE  = 0.2f;    // MHz — +/-200 kHz for matching
static const int   PERSIST_THRESHOLD     = 3;       // consecutive sweeps to confirm
static const int   MAX_PROTO_TRACKED     = 14;      // max tracked protocol signatures

// ── Threat Timing ─────────────────────────────────────────────────────
static const unsigned long COOLDOWN_MS   = 15000;   // ms before threat decays one level (was 30000)
static const int RSSI_SWEEP_INTERVAL     = 3;       // run RSSI sweep every Nth CAD cycle

// ── Rapid-Clear Path ──────────────────────────────────────────────────
static const int RAPID_CLEAR_CLEAN_CYCLES = 4;      // consecutive clean cycles to force CLEAR

// ── CAD Tap Persistence ───────────────────────────────────────────────
static const int   MAX_TAPS              = 32;      // max concurrent CAD taps
static const int   TAP_CONFIRM_HITS      = 3;       // consecutive hits to confirm as drone
static const int   TAP_EXPIRE_MISSES     = 3;       // consecutive misses to deactivate
static const float TAP_FREQ_TOL          = 0.2f;    // MHz — +/-200 kHz to match existing tap

// ── CAD Channel Allocation ────────────────────────────────────────────
static const int CAD_CH_SF6  = 60;   // ELRS 200Hz — highest priority
static const int CAD_CH_SF7  = 30;   // ELRS 150Hz
static const int CAD_CH_SF8  = 15;   // ELRS 100Hz
static const int CAD_CH_SF9  = 8;
static const int CAD_CH_SF10 = 4;
static const int CAD_CH_SF11 = 2;
static const int CAD_CH_SF12 = 2;
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
static const unsigned long AMBIENT_AUTOLEARN_MS   = 60000;  // ms before confirmed tap auto-learned (was 10s)

// ── RSSI-Guided CAD ──────────────────────────────────────────────────
static const float RSSI_GUIDED_THRESH_DB = 8.0f;   // dB above NF for guided CAD targeting
static const int   RSSI_GUIDED_MAX_BINS  = 8;      // max elevated bins to CAD-check

// ── Rolling Hit Counter ───────────────────────────────────────────────
static const unsigned long RECENT_HIT_WINDOW_MS = 30000; // time window for rolling counter
static const int   RECENT_HIT_BUF_SIZE          = 64;    // ring buffer entries

// ── RSSI Ambient Filter ──────────────────────────────────────────────
static const int   AMBIENT_HISTORY_DEPTH   = 10;    // sweeps of history before baseline
static const float AMBIENT_VARIANCE_THRESH = 5.0f;  // dB^2 max variance for baseline lock
static const float AMBIENT_UNLOCK_SHIFT    = 6.0f;  // dB drift to unlock a baseline bin
static const float AMBIENT_MIN_ABOVE_NF    = 5.0f;  // dB above NF to qualify for baseline

// ── GNSS Integrity ────────────────────────────────────────────────────
static const float CNO_STDDEV_SPOOF_THRESH = 2.0f;  // dB-Hz — below this = spoofing suspected
static const int   MIN_ELEV_FOR_CNO        = 20;    // degrees — exclude low-elevation sats

// ── Buzzer / Alert ────────────────────────────────────────────────────
static const unsigned long MUTE_DURATION_MS  = 300000; // 5 minutes
static const unsigned long REMINDER_INTERVAL = 30000;  // 30 seconds

#endif // SENTRY_CONFIG_H
