# SENTRY-RF Updated Sprint Roadmap (Post Sprint 7)

## Current Status
Sprints 1-7 complete. The system has:
- Sub-GHz spectrum scanning (860-930 MHz) via SX1262
- GNSS integrity monitoring (jamming/spoofing detection) via u-blox M10
- FreeRTOS dual-core architecture (GPS on Core 0, LoRa on Core 1)
- Detection engine with drone frequency matching and threat FSM
- 5-screen OLED UI with button navigation
- SD/SPIFFS logging
- WiFi AP dashboard (toggle-able)
- Firmware v0.7.0

## Hardware Targets (Updated)

### Existing boards (Sprints 1-7)
| Board | Radio | Bands |
|-------|-------|-------|
| LilyGo T3S3 (SX1262) | SX1262 | 860-930 MHz only |
| Heltec WiFi LoRa 32 V3 | SX1262 | 860-930 MHz only |

### New primary board (Sprint 8+)
| Board | Radio | Bands |
|-------|-------|-------|
| **LilyGo T3-S3 LR1121** | LR1121 | 150-960 MHz + 2400-2500 MHz (band-switching) |

The T3-S3 LR1121 has the same ESP32-S3, same OLED (SSD1306 128x64, SDA=18, SCL=17), same SD card, same QWIIC, same form factor as the current T3S3. The LR1121 is pin-compatible with RadioLib's LR11x0 class and supports `GetRssiInst` on both sub-GHz and 2.4 GHz — enabling the exact same RSSI sweep technique used on sub-GHz today.

**Key LR1121 capability:** The radio can switch between sub-GHz and 2.4 GHz bands in software. It cannot operate both bands simultaneously, but alternating sweeps (one sub-GHz, one 2.4 GHz) gives full dual-band coverage at ~1 second cycle time.

### External compass module (QWIIC)
QMC5883L or QMC6310 magnetometer connected via QWIIC/I2C on the T3S3's second I2C bus (Wire1, SDA=21, SCL=10). Provides heading data for directional indication of RF signal source when combined with RSSI-based bearing estimation.

---

## Sprint 8: LR1121 Dual-Band Scanning + 2.4 GHz Drone Detection

### Goal
Add LR1121 board support with dual-band spectrum scanning (sub-GHz + 2.4 GHz), detect drone C2 links on both bands using RSSI energy detection, and add 2.4 GHz drone channel signatures to the detection engine.

### Stage 1 — LR1121 board support
- Add `BOARD_T3S3_LR1121` build environment to `platformio.ini`
- Add LR1121 pin definitions to `board_config.h` (SPI pins may differ — check LilyGo schematic)
- RadioLib supports LR1121 via the `LR1121` class (inherits from `LR11x0`)
- The LR1121 requires firmware to be loaded on first use — RadioLib handles this, but verify
- `scannerInit()` must detect which radio chip is present and initialize accordingly
- Acceptance: `pio run -e t3s3_lr1121` compiles clean, radio initializes on both sub-GHz and 2.4 GHz

### Stage 2 — Dual-band sweep
- Modify `rf_scanner.cpp` to support alternating band sweeps:
  - Sweep 1: 860-930 MHz (70 MHz, 100 kHz steps = 700 bins) — same as today
  - Sweep 2: 2400-2500 MHz (100 MHz, 1 MHz steps = 100 bins) — coarser resolution but covers the full ISM band
- The LR1121 band switch requires: `radio.setFrequency(freq)` — RadioLib handles the internal band switching automatically when frequency crosses the sub-GHz/2.4 GHz boundary
- Store both sweep results in `SystemState` — add `ScanResult spectrum24` alongside existing `spectrum`
- On SX1262 boards, the 2.4 GHz sweep is skipped (SX1262 can't do 2.4 GHz)
- Acceptance: Serial output shows alternating sub-GHz and 2.4 GHz sweeps with real RSSI data

### Stage 3 — 2.4 GHz drone channel database
Add to `drone_signatures.cpp`:
- **ELRS 2.4 GHz:** 80 channels, 2400-2480 MHz, 1 MHz spacing, SX1280 LoRa modulation
- **DJI OcuSync 2.0/O3/O4:** Uses channels centered around 2.4 GHz (specific channels vary by region, but energy detection across 2400-2480 MHz catches them)
- **FrSky ACCESS 2.4 GHz:** FHSS across 2400-2480 MHz
- **TBS Tracer 2.4 GHz:** FHSS, 2400-2480 MHz
- **Common WiFi channels:** Ch 1 (2412), Ch 6 (2437), Ch 11 (2462) — for distinguishing WiFi APs from drone signals

The matching algorithm flags any 2.4 GHz energy that does NOT correspond to a known WiFi AP beacon (from the ESP32's WiFi scan list) as a potential drone signal.

### Stage 4 — WiFi promiscuous mode for Remote ID
Running simultaneously with the LR1121 2.4 GHz sweep (different hardware — ESP32 internal WiFi vs external LR1121 SPI radio):
- WiFi in promiscuous mode captures 802.11 management frames
- Parse Open Drone ID (ASTM F3411) from WiFi beacon vendor-specific IEs and NaN action frames
- Use `opendroneid-core-c` library (add as lib dependency)
- MAC OUI fingerprinting for known drone controllers (DJI 60:60:1F, Autel, Parrot, Skydio)
- Channel hop across 1-13, 100ms dwell per channel
- Runs as `WiFiScanTask` on Core 0

### Stage 5 — Dashboard mode toggle
- Default boot: Scanner mode (promiscuous WiFi + LR1121 dual-band sweep)
- Hold BOOT button 5 seconds: Switch to Dashboard mode
  - `esp_wifi_set_promiscuous(false)`
  - `WiFi.softAP("SENTRY-RF", "sentryrf1")`
  - Web server with JSON API + auto-refresh HTML
  - OLED shows "DASHBOARD MODE" + IP
- Visual countdown on OLED while holding button: "Dashboard in 5... 4... 3..."
- Power cycle returns to scanner mode

### Acceptance criteria
- [ ] LR1121 board compiles and initializes radio on both bands
- [ ] Dual-band sweep alternates sub-GHz and 2.4 GHz with real RSSI data
- [ ] 2.4 GHz drone signatures matched by detection engine
- [ ] WiFi Remote ID parsed via opendroneid-core-c
- [ ] Dashboard mode activates on 5-second BOOT hold
- [ ] All three existing boards still compile (SX1262 boards skip 2.4 GHz sweep)

---

## Sprint 9: Compass Integration + Directional Bearing

### Goal
Integrate the QMC5883L/QMC6310 magnetometer via QWIIC I2C, compute magnetic heading, and provide directional bearing estimation for detected RF signals and GNSS anomalies.

### Approach
- Read magnetometer on Wire1 (SDA=21, SCL=10) — already initialized as `initCompassBus()` from Sprint 4
- Library: QMC5883LCompass (Arduino) or direct I2C register reads
- Compute heading from X/Y magnetometer axes (declination correction for local area)
- **Directional RF bearing:** When a signal is detected on the sub-GHz or 2.4 GHz scan, log the heading at time of detection. If the user physically rotates the device, RSSI changes correlate with heading. The device can suggest "strongest signal at heading 270° (West)" by tracking peak RSSI vs heading over time. This is manual direction-finding — rotate the device and watch the RSSI peak indicator.
- **GNSS jamming bearing:** If jamming is detected, log heading during the jamming event. Rotating the device toward/away from the jammer changes the AGC/jamInd values. Same rotate-and-peak technique.
- Store heading in `GpsData` struct (or a new `CompassData` struct) and include in shared SystemState

### Acceptance criteria
- [ ] Compass heading printed to serial: "Heading: 247°"
- [ ] Heading displayed on GPS screen and Threat screen
- [ ] Peak RF signal bearing tracked: "Strongest at 270° W"
- [ ] Compass calibration routine (rotate device 360°) accessible via long-press or serial command

---

## Sprint 10: UI Overhaul + Custom Splash Screen

### Goal
Redesign the OLED UI for field use with information-dense screens, add a custom SENTRY-RF logo splash screen, and create new dashboard-style summary screens.

### New screen layout (128x64 OLED, 7 screens)

**Screen 0 — Boot Splash:**
Custom SENTRY-RF logo bitmap (user-designed). Displayed for 2 seconds on boot. Stored as a `const uint8_t PROGMEM` bitmap array in a header file. Convert the logo PNG to a 128x64 monochrome bitmap using image2cpp or similar tool.

**Screen 1 — Dashboard Summary (new):**
Single-screen overview of the entire system state:
```
┌────────────────────────────┐
│ SENTRY-RF    ██ CLEAR ██   │  (threat level, inverted bar for WARNING+)
│ Sub: ▁▂▃▅▇▅▃▂▁  2.4: ▂▅▇▅▂│  (mini spectrum bars for both bands)
│ GPS:3D 9SV  Jam:OK Spf:OK │  (GPS + integrity one-liner)
│ Batt:78% WiFi:SCAN HDG:247│  (battery + wifi mode + compass heading)
└────────────────────────────┘
```

**Screen 2 — Sub-GHz Spectrum (existing, refined):**
Bar chart 860-930 MHz with peak annotation and threat level.

**Screen 3 — 2.4 GHz Spectrum (new):**
Bar chart 2400-2500 MHz. Same rendering as sub-GHz but for the 2.4 GHz sweep. Only shown on LR1121 boards. On SX1262 boards, this screen shows "2.4 GHz: No radio" with a note to upgrade.

**Screen 4 — GPS + Compass:**
Position, fix, SVs, plus compass heading with a visual bearing indicator (small compass rose or arrow glyph).

**Screen 5 — GNSS Integrity:**
Jamming/spoofing status, C/N0 histogram, bearing to suspected jammer if rotating.

**Screen 6 — Threat Detail:**
Active detections with protocol match, frequency, RSSI, persistence count, bearing. Scrolls if multiple detections.

**Screen 7 — System Info:**
Version, uptime, free heap, battery voltage (ADC on GPIO 1), board name, WiFi mode (SCAN/AP/OFF), SD card status.

### Battery monitoring
- T3S3 has battery voltage on GPIO 1 via voltage divider
- Read ADC, scale to battery voltage (3.0-4.2V for LiPo)
- Display as percentage (3.0V=0%, 4.2V=100%, linear approximation is fine)

### Page dots + button navigation
- Same as Sprint 7: BOOT button cycles screens, auto-rotate after 5 seconds
- Page dots at bottom center

### Custom splash screen workflow
1. User designs logo as PNG
2. Convert to 128x64 monochrome bitmap using image2cpp (https://javl.github.io/image2cpp/)
3. Paste resulting `const uint8_t` array into `include/splash_logo.h`
4. `displayBootSplash()` draws the bitmap with `drawBitmap()` for 2 seconds, then transitions to normal screens

### Acceptance criteria
- [ ] Custom splash logo displays on boot
- [ ] Dashboard summary screen shows all key data in one view
- [ ] 2.4 GHz spectrum screen renders on LR1121, gracefully degrades on SX1262
- [ ] Battery voltage displayed as percentage
- [ ] All 8 screens render correctly and cycle via button

---

## Sprint 11: Field Hardening + Production Readiness

### Goal
Harden the system for real field deployment. Fix edge cases, optimize power, validate detection accuracy.

### Tasks
- **Raise GPS_MIN_CNO to 15-20** for outdoor field use (currently 6 for indoor testing)
- **Power management:** Deep sleep mode when idle, wake on button press or timer
- **SD card log rotation:** Prevent filling the SD card on long deployments
- **Watchdog monitoring:** Ensure all FreeRTOS tasks have proper watchdog feeding
- **Detection accuracy validation:** Test with real drones (ELRS 900, ELRS 2.4, DJI) and document detection ranges
- **False positive tuning:** Adjust noise floor thresholds, persistence counts, and cooldown timers based on real-world data
- **README update:** Full documentation with photos, wiring diagrams, detection capabilities, limitations

### Acceptance criteria
- [ ] GPS_MIN_CNO raised to production value
- [ ] System runs 8+ hours on battery without crash or watchdog reset
- [ ] Detection validated against at least 2 real drone types
- [ ] README complete with honest capability description

---

## Key Technical Decisions

### LR1121 band switching in the scan loop
```
for (;;) {
    // Sub-GHz sweep: 860-930 MHz, 700 bins, ~500ms
    radio.setFrequency(860.0);  // triggers band switch to sub-GHz internally
    sweepSubGHz(radio, subGhzResult);
    
    // 2.4 GHz sweep: 2400-2500 MHz, 100 bins, ~100ms
    radio.setFrequency(2400.0);  // triggers band switch to 2.4 GHz internally
    sweep24GHz(radio, result24);
    
    // Copy both results to shared state under mutex
    // Run detection engine on both
}
```

### WiFi dual-mode architecture
```
Boot → Scanner Mode (default)
  ├── ESP32 WiFi: promiscuous mode, channel hopping, Remote ID capture
  ├── LR1121: alternating sub-GHz + 2.4 GHz RSSI sweeps
  └── GPS: continuous GNSS integrity monitoring

Hold BOOT 5s → Dashboard Mode
  ├── ESP32 WiFi: AP mode, web server
  ├── LR1121: continues scanning (independent SPI hardware)
  └── GPS: continues monitoring
```

### Compass bearing estimation
The device has a single omnidirectional antenna. True direction-finding requires multiple antennas or a rotating directional antenna. What we CAN do is **rotation-based bearing**: the user physically rotates the device while watching RSSI on the OLED. The compass tracks which heading corresponds to the strongest signal. This is the same technique used by amateur radio fox hunters with a handheld yagi — but with an omnidirectional antenna, the resolution is coarser (~±45°). Still useful for narrowing down "the signal is coming from the west" vs "the signal is coming from the east."

---

## Updated File Structure (Post Sprint 11)
```
SENTRY-RF/
├── platformio.ini                 # 4 build envs: t3s3, heltec_v3, t3s3_lr1121, (heltec optional)
├── include/
│   ├── board_config.h             # Pin mappings for all boards including T3S3-LR1121
│   ├── task_config.h              # FreeRTOS task parameters
│   ├── detection_types.h          # Shared structs, enums, queue handles
│   ├── drone_signatures.h         # Sub-GHz + 2.4 GHz drone frequency tables
│   ├── version.h                  # Firmware version
│   ├── splash_logo.h              # Custom boot logo bitmap (NEW)
│   ├── rf_scanner.h
│   ├── gps_manager.h
│   ├── gnss_integrity.h
│   ├── detection_engine.h
│   ├── alert_handler.h
│   ├── display.h
│   ├── data_logger.h
│   ├── wifi_dashboard.h
│   ├── wifi_scanner.h             # WiFi promiscuous Remote ID (NEW)
│   ├── remote_id_parser.h         # OpenDroneID parsing (NEW)
│   └── compass.h                  # QMC5883L/QMC6310 magnetometer (NEW)
├── src/
│   ├── main.cpp                   # Task creation, mode switching
│   ├── rf_scanner.cpp             # Dual-band sweep (sub-GHz + 2.4 GHz on LR1121)
│   ├── gps_manager.cpp            # u-blox M10 UART + UBX
│   ├── gnss_integrity.cpp         # Jamming/spoofing algorithms
│   ├── drone_signatures.cpp       # ELRS/Crossfire/DJI/Tracer frequency tables
│   ├── detection_engine.cpp       # Threat FSM with dual-band correlation
│   ├── alert_handler.cpp          # LED patterns, serial alerts
│   ├── display.cpp                # 8-screen OLED UI with dashboard summary
│   ├── data_logger.cpp            # SD/SPIFFS CSV logging
│   ├── wifi_dashboard.cpp         # HTTP API server (dashboard mode)
│   ├── wifi_scanner.cpp           # Promiscuous mode Remote ID (NEW)
│   ├── remote_id_parser.cpp       # OpenDroneID frame parsing (NEW)
│   └── compass.cpp                # Magnetometer + bearing estimation (NEW)
├── lib/
│   └── opendroneid/               # opendroneid-core-c library (NEW)
├── data/                          # SPIFFS web dashboard HTML
├── docs/
│   ├── wiring.md
│   ├── detection_logic.md
│   └── field_testing.md
├── LICENSE
└── README.md
```

---

## Dependencies Summary

| Library | Source | Purpose |
|---------|--------|---------|
| RadioLib v7+ | PlatformIO lib | SX1262 + LR1121 radio control |
| SparkFun u-blox GNSS v3 | PlatformIO lib | GPS UBX protocol |
| Adafruit SSD1306 + GFX | PlatformIO lib | OLED display |
| opendroneid-core-c | GitHub | Remote ID frame parsing |
| QMC5883LCompass | PlatformIO lib | Magnetometer heading |
| WebServer (built-in) | ESP32 Arduino | WiFi dashboard |

---

## Risk Register

| Risk | Impact | Mitigation |
|------|--------|------------|
| LR1121 RadioLib `getRSSI(false)` on 2.4 GHz may behave differently than SX1262 | High | Test immediately on Stage 2. Semtech's own spectral scan example confirms GetRssiInst works on 2.4 GHz. |
| Band switching latency may slow dual-band sweep cycle | Medium | Measure actual switch time. LR1121 datasheet shows ~100µs PLL settling for in-band, ~1-5ms for cross-band. Budget 10ms for safety. |
| WiFi promiscuous + LR1121 SPI contention | Low | Different hardware buses. WiFi is internal, LR1121 is external SPI. No electrical conflict. |
| Compass calibration drift near electronics | Medium | Hard/soft iron calibration routine. Keep magnetometer on QWIIC cable, physically separated from the board. |
| GPS_MIN_CNO at 6 degrades fix in production | High | **MUST raise to 15-20 before field deployment.** Currently tracked in Claude memory. |
| opendroneid-core-c library size may exceed flash on Heltec V3 | Medium | Heltec V3 has 8MB flash — should be fine. Test during Sprint 8 Stage 4. |
