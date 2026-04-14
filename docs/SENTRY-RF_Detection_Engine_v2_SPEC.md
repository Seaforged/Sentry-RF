<!-- ACTIVE SPEC — supersedes SENTRY-RF_Detection_Engine_v2_Redesign.md and SENTRY-RF_Architecture_Checklist.md -->
<!-- Last updated: April 14, 2026 | Current phase: COMPLETE -->

# SENTRY-RF Detection Engine v2.0 — Unified Implementation Spec
## Candidate-Centric Evidence Fusion: The Build Contract

**Status:** ACTIVE — supersedes all prior detection engine planning documents  
**Supersedes:** `SENTRY-RF_Detection_Engine_v2_Redesign.md`, `SENTRY-RF_Architecture_Checklist.md`  
**Use `SENTRY-RF_Session_Handoff.md` for:** onboarding/context only  
**Authors:** Claude (technical lead) + Codex (adversarial review) + ND (project owner)  
**Last updated:** April 13, 2026  
**Current codebase:** v1.8.0 — commit 61dae2e  

---

## Part 1 — Why This Redesign Exists

The v1.8.0 detection engine uses **global counters**. Every piece of evidence — CAD taps, RSSI peaks, protocol matches, Remote ID, GNSS anomalies — feeds into one shared fast score and one shared confirm score. This causes three failures:

**1. Unrelated evidence summation.**  
A single infrastructure LoRa tap (fast=10) plus a real WiFi RID broadcast (confirm=30) equals WARNING. These are completely unrelated events that happened to occur in the same scan window. They should never promote each other.

**2. Brittle confirmation timing.**  
Confirm evidence only exists on the exact RSSI sweep cycle (`freshRssi=true`) and vanishes on non-sweep cycles. One good sweep should provide confirmation that persists for several seconds across multiple scan cycles — not disappear the moment the next non-sweep cycle runs.

**3. No candidate association.**  
The engine cannot say "this RSSI peak at 919 MHz corroborates this CAD tap at 919.2 MHz." It just sums everything globally. There is no concept of a specific detection event that evidence must attach to in order to promote.

**The fix: candidate-centric evidence fusion.**  
Sub-GHz CAD creates a detection candidate. Subsequent evidence — RSSI, protocol match, 2.4 GHz CAD, Remote ID, GNSS anomaly — must be *explicitly associated* with that candidate to promote it. Evidence decays with configurable TTLs measured in wall-clock milliseconds, not scan cycles.

---

## Part 2 — Architecture

```
┌─────────────────────────────────────────────────────────┐
│                      SCAN LAYER                          │
│  cadSubGHzFskScan() → CadBandSummary + anchor           │
│  cad24Scan()        → CadBandSummary + anchor (timer)   │
│  scannerSweep()     → ScanResult sub-GHz (timer)        │
│  scannerSweep24()   → ScanResult24 2.4 GHz (timer)      │
│  wifiScanTask()     → RID evidence (async)               │
│  gpsTask()          → GNSS integrity (async)             │
└───────────────────────────┬─────────────────────────────┘
                             │
                             ▼
┌─────────────────────────────────────────────────────────┐
│                   CANDIDATE ENGINE                        │
│                                                          │
│  DetectionCandidate[MAX_CANDIDATES=6]                    │
│                                                          │
│  ┌────────────────────────────────────────────────┐     │
│  │ candidate 0 (sub-GHz, SEEDING → TRACKING)       │     │
│  │   anchor:    919.2 MHz / SF6                    │     │
│  │   freqRange: [918.5, 920.1 MHz]                 │     │
│  │   cadConf:   {score=40, lastSeen=Xms, live}     │     │
│  │   fhssSub:   {score=15, lastSeen=Xms, live}     │     │
│  │   sweepSub:  {score=5,  lastSeen=Xms, live}     │     │
│  │   protoSub:  {score=15, lastSeen=Xms, live}     │     │
│  │   rid:       {score=0,  dead}                   │     │
│  │   → fast=55, confirm=20 → WARNING               │     │
│  └────────────────────────────────────────────────┘     │
│                                                          │
│  ┌────────────────────────────────────────────────┐     │
│  │ candidate 1 (confirmer role only)               │     │
│  │   cad24: {score=10, lastSeen=Xms, live}         │     │
│  │   → attaches to candidate 0 (temporal match)    │     │
│  └────────────────────────────────────────────────┘     │
│                                                          │
│  chooseBestCandidate() → ThreatDecision                  │
└───────────────────────────┬─────────────────────────────┘
                             │
                             ▼
┌─────────────────────────────────────────────────────────┐
│                     THREAT FSM                            │
│  Fast up: direct jump to evidence-warranted level        │
│  Slow down: one step per COOLDOWN_MS                     │
│  emitThreatTransition(prev, next, nowMs) on change       │
└─────────────────────────────────────────────────────────┘
```

### Main Loop Execution Order (mandatory)

The following order is required in `src/main.cpp`. Deviating from this order breaks the fast-alert guarantee.

```
1. cadSubGHzFskScan()              → produces CadBandSummary + anchor
2. detectionEngineIngestCadSubGHz() → refreshes candidate fast evidence
3. detectionEngineAssess()          → produces ThreatDecision, updates FSM
4. publish threat to systemState    → OLED, LED, buzzer, log see result

[on timer] sub-GHz sweep ingest
[on separate timer] LR1121 2.4 GHz CAD ingest
[on separate timer] LR1121 2.4 GHz sweep ingest
```

**The 2.4 GHz scan must never run before the first assess of the same cycle.** It is a confirmer. It never drives the fast-alert path.

---

## Part 3 — Data Structures

These structs are the exact contract CC must implement. Do not deviate without ND sign-off.

### 3.1 EvidenceTerm

```cpp
struct EvidenceTerm {
    uint8_t  score;        // Current score value (0 = dead)
    uint32_t lastSeenMs;   // millis() when last refreshed
    uint32_t ttlMs;        // Evidence expires when (millis() - lastSeenMs) > ttlMs
    bool     ready;        // Per-evidence readiness gate (see Part 6)
};
```

An EvidenceTerm is **live** when: `ready == true && score > 0 && (millis() - lastSeenMs) <= ttlMs`

### 3.2 CandidateState

```cpp
enum CandidateState : uint8_t {
    CAND_EMPTY,      // Slot is free
    CAND_SEEDING,    // Just created, first CAD anchor seen
    CAND_TRACKING,   // Active, live fast evidence
    CAND_CONFIRMED   // ThreatDecision is WARNING or CRITICAL
};
```

### 3.3 DetectionCandidate

```cpp
struct DetectionCandidate {
    CandidateState      state;
    bool                active;
    uint8_t             bandMask;        // 1=subGHz, 2=2.4GHz, 3=both
    float               anchorFreq;      // MHz — frequency of best CAD tap at creation
    float               minFreqSeen;     // MHz — lowest frequency associated so far
    float               maxFreqSeen;     // MHz — highest frequency associated so far
    const DroneProtocol* protoHint;      // nullptr until protocol matched; NEVER store stale pointer
    uint32_t            firstSeenMs;
    uint32_t            lastSeenMs;
    EvidenceTerm        cadConfirmed;    // Confirmed CAD taps (consecutiveHits >= 2)
    EvidenceTerm        cadPending;      // Pending CAD taps (single hit)
    EvidenceTerm        fskConfirmed;    // Confirmed FSK taps
    EvidenceTerm        fhssSub;         // FHSS velocity / spread evidence
    EvidenceTerm        sweepSub;        // RSSI sweep corroboration
    EvidenceTerm        protoSub;        // Protocol signature match
    EvidenceTerm        cad24;           // 2.4 GHz CAD corroboration
    EvidenceTerm        proto24;         // 2.4 GHz protocol match
    EvidenceTerm        rid;             // WiFi Remote ID
    EvidenceTerm        gnss;            // GNSS anomaly temporal correlation
};
```

**Critical implementation note on `protoHint`:**  
Always initialize to `nullptr`. When `resetCandidate()` is called, set `protoHint = nullptr` immediately. Never read `protoHint` without checking `!= nullptr` first. A reset candidate with a stale non-null `protoHint` silently influences scoring — this is a subtle bug CC must be guarded against explicitly.

### 3.4 CadEvidenceAnchor (add to CadBandSummary)

```cpp
struct CadEvidenceAnchor {
    bool    valid;
    float   frequency;        // MHz of best tap
    uint8_t sf;               // Spreading factor of best tap
    bool    isFsk;
    uint8_t consecutiveHits;  // Hits on this tap in last cycle
};
```

Add to `CadBandSummary`:
```cpp
CadEvidenceAnchor anchor;
uint32_t          capturedMs;    // millis() when summary was produced
bool              warmupReady;   // ambient filter has run >= MIN_AMBIENT_CYCLES
bool              hwFault;       // cadHwFaultFlag debounced true
```

### 3.5 ThreatDecision

```cpp
struct ThreatDecision {
    ThreatLevel level;
    uint8_t     fastScore;
    uint8_t     confirmScore;
    float       anchorFreq;     // MHz of driving candidate
    uint8_t     bandMask;       // bands contributing to decision
    bool        hasCandidate;   // false = RID-only ADVISORY
};
```

---

## Part 4 — Configuration Constants

Add all of the following to `include/sentry_config.h`. These are the values as of April 13, 2026. Do not hardcode any of these inline in .cpp files.

```cpp
// --- Candidate engine sizing ---
#define MAX_CANDIDATES              6
#define CAND_AGE_OUT_MS             12000   // Evict if all evidence dead > 12s

// --- Association windows ---
#define CAND_ASSOC_SUB_MHZ          1.0f    // Sub-GHz evidence must be within ±1 MHz of anchor
#define CAND_ASSOC_24_MHZ           4.0f    // 2.4 GHz evidence association window ±4 MHz
#define CAND_ASSOC_SUB_TTL_MS       6000    // Sub-GHz evidence attaches only if candidate seen within 6s
#define CAND_ASSOC_24_TTL_MS        5000    // 2.4 GHz evidence attaches only if candidate seen within 5s

// --- Evidence TTLs (milliseconds) ---
#define TTL_CAD_CONFIRMED_MS        2500
#define TTL_CAD_PENDING_MS          2500
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
#define FAST_SCORE_FHSS_CLUSTER     15
```

---

## Part 5 — Candidate Creation Rules

A candidate **may only be created** by `detectionEngineIngestCadSubGHz()` when ALL of the following are true:

- `summary.anchor.valid == true`
- `summary.hwFault == false`
- At least one is true:
  - `confirmedCadCount > 0`
  - `confirmedFskCount > 0`
  - `strongPendingCad > 0`
  - `persistentDiversityCount > 0`
  - `diversityVelocity >= DIVERSITY_VELOCITY_FHSS_MIN`

A candidate **must never be created** by:
- 2.4 GHz CAD
- Sweep-only evidence
- Remote ID
- GNSS

This keeps the fast-trigger path identical across SX1262, Heltec V3, and LR1121.

---

## Part 6 — Association Rules

These rules determine whether a piece of evidence attaches to an existing candidate. Evidence that fails association is discarded (or, for RID/GNSS, generates a standalone ADVISORY only).

### Sub-GHz evidence (CAD, sweep, protocol)

Attaches to a candidate only if **all** are true:
- `candidate.active == true`
- `millis() - candidate.lastSeenMs <= CAND_ASSOC_SUB_TTL_MS`
- Evidence frequency is in the same regional band as the candidate (US 902–928 MHz or EU 860–870 MHz)
- **And at least one** of:
  - `fabs(freq - candidate.anchorFreq) <= CAND_ASSOC_SUB_MHZ`
  - `freq` falls within `[candidate.minFreqSeen - 1.0, candidate.maxFreqSeen + 1.0]`
  - Candidate has live `fhssSub` evidence AND freq is in the same regional band (FHSS association by band)

When evidence attaches, update `candidate.minFreqSeen` and `candidate.maxFreqSeen` to expand the range if needed.

### 2.4 GHz evidence (CAD, sweep, protocol)

Attaches to a sub-GHz candidate only if **all** are true:
- At least one active sub-GHz candidate exists
- `millis() - candidate.lastSeenMs <= CAND_ASSOC_24_TTL_MS`
- **And one** of:
  - Exactly one live sub-GHz candidate exists (unambiguous association)
  - One candidate's fast score exceeds the next-best candidate's fast score by at least 15 (clear leader)

2.4 GHz evidence **never creates a candidate**. It is confirmer-only.

### Remote ID and GNSS

Attach only if:
- Exactly one live candidate exists

If RID or GNSS cannot be uniquely attached to a single candidate, they may generate a standalone RID-only ADVISORY (if `ridScore >= POLICY_RID_ONLY_ADVISORY`) but **must not promote any RF candidate to WARNING or CRITICAL**.

---

## Part 7 — Evidence Readiness Gates

These gates determine whether each evidence type is allowed to contribute to scoring. This replaces the old global warmup cap.

| Evidence | Ready When |
|---|---|
| `cadConfirmed` | Always (cycle 1) |
| `cadPending` | Always (cycle 1) |
| `fskConfirmed` | Always (cycle 1) |
| `fhssSub` | `persistentDiversityCount > 0` OR `diversityVelocity >= DIVERSITY_VELOCITY_FHSS_MIN` |
| `fhssCluster` | `diversityCount >= 8` AND `fastConfirmedCadCount >= 1` |
| `sweepSub` | `ambientFilterReady()` OR peak matches a known protocol family OR candidate already has live fast evidence |
| `protoSub` | Always when `match.protocol != nullptr` |
| `bandEnergy` | `ambientFilterReady()` only; attach as candidate-confirm only |
| `cad24` | Only as confirmer on existing live sub-GHz candidate |
| `proto24` | Only as confirmer on existing live sub-GHz candidate |
| `rid` | Always (independent hardware) |
| `gnss` | GPS has valid 3D fix |

### Progressive ambient tagging (B.5 — must happen BEFORE shadow mode)

During warmup, after each `recordAmbientTap()` call: immediately scan all live candidate taps and mark any tap at that frequency as `isAmbient`. Do not wait until warmup ends to do a batch tag. This prevents warmup-period infrastructure taps from being inherited as evidence into the candidate engine.

---

## Part 8 — Candidate Lifecycle

### States

| State | Meaning | Entry Condition |
|---|---|---|
| `CAND_EMPTY` | Slot free | Initial / after eviction |
| `CAND_SEEDING` | Just created | First valid sub-GHz CAD anchor |
| `CAND_TRACKING` | Active | Any live fast evidence |
| `CAND_CONFIRMED` | Promoted | `ThreatDecision.level >= WARNING` |

### Age-out and eviction

- A candidate is aged out when **all evidence terms are dead** for `> CAND_AGE_OUT_MS`
- Eviction order when all slots full: **oldest inactive candidate first**, then **lowest total live score**
- Caveat: do not evict the only `CAND_CONFIRMED` candidate unless it has been dead for `> CAND_AGE_OUT_MS`

### Required lifecycle helpers (implement in `src/detection_engine.cpp`)

```
resetCandidate()           — zero all fields, protoHint = nullptr
findCandidateForBandFreq() — returns best matching candidate or nullptr
allocCandidate()           — finds empty slot or evicts per eviction rules
refreshEvidence()          — updates score + lastSeenMs on a term if association passes
evidenceLive()             — returns bool: ready && score > 0 && not expired
ageOutCandidates()         — marks CAND_EMPTY on fully-expired candidates
computeFastScore()         — returns uint8_t: sum of live fast terms, capped
computeConfirmScore()      — returns uint8_t: sum of live confirm terms
chooseBestCandidate()      — returns ThreatDecision from highest-scoring live candidate
```

---

## Part 9 — Scoring Policy

Scoring uses **candidate-attached evidence only**. Global scores do not exist in v2.0.

### Fast score (per candidate)

```
fast = cadConfirmed.score
     + cadPending.score
     + fskConfirmed.score
     + (fhssSub live     ? FAST_SCORE_DIVERSITY_BONUS : 0)
     + (fhssCluster live ? FAST_SCORE_FHSS_CLUSTER    : 0)
```

Component values:
- Confirmed CAD taps: `FAST_SCORE_CAD_PER_TAP` per tap, cap `FAST_SCORE_CAD_CAP`
- Confirmed FSK taps: `FAST_SCORE_FSK_PER_TAP` per tap, cap `FAST_SCORE_FSK_CAP`
- `fhssSub` live when sustained diversity is present:
  `persistentDiversityCount > 0` OR
  `diversityVelocity >= DIVERSITY_VELOCITY_FHSS_MIN`
- Live `fhssSub`: `+FAST_SCORE_DIVERSITY_BONUS`
- Live `fhssCluster`: `+FAST_SCORE_FHSS_CLUSTER`
  with `diversityCount >= 8` AND `fastConfirmedCadCount >= 1`

### Confirm score (per candidate)

```
confirm = sweepSub.score
        + protoSub.score
        + bandEnergy.score
        + cad24.score
        + proto24.score
        + rid.score
        + gnss.score
```

Component values:

| Evidence | First attach | Persistent (re-attaches) |
|---|---|---|
| `sweepSub` (RSSI corroboration) | 5 | 10 |
| `protoSub` (protocol match) | 15 | 15 |
| `cad24` (2.4 GHz corroboration) | 10 | 10 |
| `proto24` (2.4 GHz protocol) | 15 | 15 |
| `rid` (WiFi Remote ID) | 30 | 30 |
| `gnss` (anomaly correlation) | 25 | 25 |

### Threat policy

```
ADVISORY:   fast >= POLICY_ADVISORY_FAST
WARNING:    fast >= POLICY_WARNING_FAST  &&  confirm >= POLICY_WARNING_CONFIRM
CRITICAL:   fast >= POLICY_CRITICAL_FAST &&  confirm >= POLICY_CRITICAL_CONFIRM
RID-only:   confirm(rid) >= POLICY_RID_ONLY_ADVISORY  (no fast evidence required)
```

**All fast and confirm evidence must come from the SAME candidate.** Fast score from candidate 0 cannot combine with confirm score from candidate 1.

### FSM behavior

- **Fast up:** Direct jump to the level warranted by current evidence (no step-by-step escalation)
- **Slow down:** One step per `COOLDOWN_MS`
- **Transitions:** `emitThreatTransition(prevThreat, currentThreat, millis())` — this is the single emit point for buzzer, LED, display updates, and log entries on state change

---

## Part 10 — Phase Plan

Phases must be executed in this exact order. Do not skip phases or combine them out of sequence.

---

### Phase A — Foundation Fixes
**Risk:** Zero — corrective only, no behavior change  
**Acceptance:** All three targets build clean. Serial output identical to pre-A.

**A.1 — Fix escalation emitter** (`src/detection_engine.cpp`)  
Save `prevThreat` before computing `desired`. Add `emitThreatTransition(prev, next, nowMs)` helper. Replace all inline transition emit code with single call to this helper. No-op if `prev == next`.

**A.2 — Replace sweepTimeMs validity check** (`src/main.cpp`)  
Replace `localResult.sweepTimeMs > 0 ? &localResult : nullptr` with `localResult.valid ? &localResult : nullptr`. If `ScanResult` lacks a `valid` field, add `bool valid = false;` to the struct and set `valid = true` only on successful sweep completion.

**A.3 — Initialize logSnapshot safely** (`src/main.cpp`)  
Change declaration to `SystemState logSnapshot = {};`. Skip `loggerWrite()` if mutex was not acquired.

**A.4 — Add all configuration constants** (`include/sentry_config.h`)  
Add the full block from Part 4 above. Build to verify no name collisions with existing constants.

---

### Phase B — Enriched CAD Summaries
**Risk:** Low — additive struct extension, no logic change  
**Acceptance:** Build clean. Anchor frequency visible in `[CAD]` serial debug output. Existing threat behavior unchanged.

**B.1 — Add `CadEvidenceAnchor` and new fields to `CadBandSummary`** (`include/cad_scanner.h`)  
Add the structs defined in Part 3.4 above.

**B.2 — Implement `chooseAnchor()`** (`src/cad_scanner.cpp`)  
Priority: best confirmed non-ambient tap → strongest pending tap → first active tap. Helpers: `tapEligibleAsAnchor()`, `betterAnchor()`, `chooseAnchor()`.

**B.3 — Populate anchor in `fillBandSummary()`** (`src/cad_scanner.cpp`)  
Call `chooseAnchor()` in both LR1121 and SX1262 `cadFskScan()` paths. Keep all legacy aggregate fields intact.

---

### Phase B.5 — Progressive Ambient Tagging During Warmup
**Risk:** Low — changes warmup behavior, not detection policy  
**Acceptance:** Cold boot, 5 runs each board. Zero false WARNING/CRITICAL in first 60 seconds. Check `[AMBIENT]` serial output shows infrastructure taps tagged progressively, not in a batch at warmup end.

**B.5.1 — Move ambient tagging to per-cycle** (`src/cad_scanner.cpp` near line 1117)  
After each `recordAmbientTap(freq)` call, immediately iterate all live taps and set `isAmbient = true` on any tap matching that frequency. Do not wait for warmup to end.

**B.5.2 — Remove post-warmup batch reset** (`src/cad_scanner.cpp`)  
Delete or disable the block that clears all taps/proto arrays when warmup ends. Infrastructure taps should be progressively marked ambient throughout warmup, not wiped in batch.

**B.5.3 — Verify no regression** in Phase A/B serial output before continuing.

---

### Phase C — Shadow-Mode Candidate Engine
**Risk:** Low — new code runs in parallel, actual threat output unchanged  
**Acceptance:** See shadow-mode acceptance criteria in Part 11.

**C.1 — Add candidate structs and TTL config** (`include/detection_engine.h`, `include/sentry_config.h`)  
Implement `EvidenceTerm`, `DetectionCandidate`, `ThreatDecision`, `CandidateState` per Part 3.

**C.2 — Implement candidate lifecycle helpers** (`src/detection_engine.cpp`)  
Implement all helpers listed in Part 8. Pay special attention to `protoHint = nullptr` in `resetCandidate()`.

**C.3 — Shadow-mode evaluation in `detectionEngineAssess()`**  
After the existing scorer runs:
- Ingest current CAD summary into candidate engine
- Compute candidate-based `ThreatDecision`
- Log: `[CAND] fast=%d conf=%d level=%s anchor=%.1fMHz ttMs=%lu`
- Log mismatches: `[CAND-DELTA] legacy=%s candidate=%s`
- **Do NOT change actual threat output**

**C.4 — Bench validation (required before Phase D)**  
Run each scenario and collect serial logs:
- SX1262: 10 min clean bench
- LR1121: 10 min clean bench
- SX1262: 3 runs JJ ELRS (time to ADVISORY, WARNING)
- LR1121: 3 runs JJ ELRS (time to ADVISORY, WARNING)
- Both boards: cold boot x5 (watch first 60s)

Bring logs to Claude before proceeding to Phase D.

---

### Phase D — Candidate Engine Cutover
**Risk:** Medium — this changes actual threat behavior. Run full validation before AND after.  
**Acceptance:** See Part 11. Both boards must pass before this phase is complete.

**D.1 — Cut over CAD ingest to per-candidate** (`src/detection_engine.cpp`, `src/main.cpp`)  
`detectionEngineIngestCadBandSummary()` refreshes candidate fast evidence.
`detectionEngineIngestCad24BandSummary()` refreshes cad24 on existing
candidates only (never creates). Aggregate CAD/FSK counters may remain wired
to the legacy comparison scorer until Phase G, but they must not drive the
real FSM or candidate scoring.

**D.2 — Cut over sweep ingest to candidate confirmation** (`src/detection_engine.cpp`)  
First corroborating sweep: `sweepSub.score = 5`, `sweepSub.ttlMs = TTL_SWEEP_SUB_MS`. Protocol match: `protoSub.score = 15`. Persistent sweep: upgrade score in-place.

**D.3 — Replace `assessThreat()` with `chooseBestCandidate()`**  
`chooseBestCandidate()` becomes the real threat decision path. Keep
`lastFastScore`, `lastConfirmScore` for display/logging. Legacy global scoring
may remain comparison-only for regression alarming until Phase G, but it must
not commit `currentThreat` or emit real transitions.

**D.4 — Run full validation suite** (see Part 11).

---

### Phase E — Per-Evidence Readiness Cleanup
**Risk:** Medium — changes cold-start behavior  
**Acceptance:** Cold boot x5 each board, zero false WARNING/CRITICAL in first 60 seconds.

**E.1 — Apply readiness gates per Part 7**  
Implement per-evidence `ready` flag logic. Remove any remaining global
warmup-cap references from the candidate path. The legacy `assessThreat()`
comparison path may retain private warmup housekeeping until it is removed in
Phase G, but it must not influence candidate scoring or the real FSM.

**E.2 — Verify Phase B.5 ambient tagging is working correctly**  
By this phase, progressive ambient tagging (B.5) should have already eliminated the batch-reset problem. This phase verifies it holds under the candidate engine.

---

### Phase F — LR1121 2.4 GHz Off Critical Path
**Risk:** Low — scheduling change only  
**Acceptance:** LR1121 ADVISORY latency matches SX1262 within 2 seconds on the same JJ trace.

**F.1 — Split `cadFskScan()` into `cadSubGHzFskScan()` + `cad24Scan()`** (`src/cad_scanner.cpp`, `include/cad_scanner.h`)  
Sub-GHz scan returns immediately → ingest → assess → threat decision. 2.4 GHz scan runs on `LR24_CAD_PERIOD_MS` timer.

**F.2 — Wall-clock scheduling in `main.cpp`**  
Replace `cycleCount % RSSI_SWEEP_INTERVAL` with `millis() >= nextSubGHzSweepMs`. Add `nextLr24CadMs`, `nextLr24SweepMs` timers.

**F.3 — Verify LR1121 non-sweep cycle time ~= SX1262 (~1500ms)**

---

### Phase G — Diagnostics and Cleanup
**Risk:** Zero — additive  
**Acceptance:** Build clean with zero new warnings. Legacy scoring code gone.

**Status: COMPLETE — April 14, 2026**
Legacy `assessThreat()` removed. Candidate engine is now the sole decision
path for `currentThreat` and FSM transitions. `[CAND-DELTA]` logging removed.
`legacyThreat`, `lastThreatEventMs`, `cleanSinceMs`, `freshRssiThisCycle`, and
the `*ThisCycle` aggregate counters removed along with their populate shim
`detectionEngineIngestCad()` and the dead backward-compat wrappers
`detectionEngineSetCadFsk()` / `detectionEngineUpdate()`.

**G.1 — Expose candidate diagnostics to `SystemState`** ✅
Added `fastScore`, `confirmScore`, `anchorFreq`, `bandMask`, `hasCandidate`,
`candidateCount` to `SystemState`. Mirrored from `ThreatDecision` inside
`detectionEngineAssess()` via new accessors (`detectionEngineGetAnchorFreq()`,
etc). Shown on OLED threat screen as a `CAND:` line when a candidate is
active (replaces the `LAST: Xs ago` fallback line). Already logged via
`[CAND]` serial line.

**G.2 — Delete legacy code** ✅
Removed `assessThreat()` (173 lines), `legacyThreat`, `lastThreatEventMs`,
`cleanSinceMs`, `freshRssiThisCycle`, and the eight `*ThisCycle` aggregate
counters. Removed `detectionEngineIngestCad()`, `detectionEngineSetCadFsk()`,
`detectionEngineUpdate()`. Removed 20 legacy weight and threshold constants
from `sentry_config.h` (`WEIGHT_DIVERSITY_PER_FREQ` through
`WEIGHT_FAST_DETECT`, `SCORE_ADVISORY/WARNING/CRITICAL`,
`FAST_DETECT_MIN_*`, `FAST_ADVISORY_THRESH`, `FAST_WARNING_THRESH`,
`CONFIRM_WARNING_THRESH`, `CONFIRM_CRITICAL_THRESH`, `CONFIRM_ADVISORY_THRESH`,
`WEIGHT_FAST_PERSISTENT_DIV`, `WEIGHT_FAST_FHSS_VELOCITY`). Kept the five
`WEIGHT_CONFIRM_*` constants that the candidate engine actively consumes.
Kept `CadFskResult` — it's the live data-transfer struct between
`cadFskScan()` and the candidate engine, not a legacy artifact.

**G.3 — Fix version string** ✅
`include/version.h` — bumped `FW_VERSION` from `"1.6.0"` to `"1.9.0-rc1"`
with a phase-completion note.

**G.4 — Spec doc updated** ✅
Phase marker moved from `F` to `COMPLETE`, this section stamped with the
completion status.

---

## Part 11 — Acceptance Criteria

### Shadow-mode acceptance (Phase C → D gate)

All of the following must be true before cutting over:

| Scenario | Requirement |
|---|---|
| Clean bench, 10 min, all 3 boards | Zero WARNING/CRITICAL |
| Cold boot x5 each board | Zero false WARNING/CRITICAL in first 60s |
| JJ ELRS ADVISORY | ≤ 4 seconds |
| JJ ELRS WARNING | ≤ 10 seconds |
| JJ ELRS CRITICAL | Only when confirm evidence is attached to same candidate as fast evidence |
| LR1121 vs SX1262 on same JJ trace | ADVISORY within 2s wall-clock |
| Multiple unrelated evidence sources | Do NOT promote each other unless association rules pass |

The metric "better than legacy on >80% of cycles" is explicitly **not** an acceptance gate. One false CRITICAL in a 10-minute run fails the run.

### Final acceptance (Phase D complete)

| Metric | Target |
|---|---|
| SX1262 ADVISORY latency | ≤ 4 seconds |
| SX1262 WARNING latency | ≤ 10 seconds |
| LR1121 ADVISORY latency | ≤ 4 seconds (after Phase F) |
| FP: no drone, 30 min | Zero WARNING/CRITICAL |
| FP: dense LoRa bench | Zero WARNING/CRITICAL (candidate isolation) |
| Board parity | Within 2s wall-clock |
| Cold-boot false alerts | Zero in first 60 seconds |

### COM14 (LR1121) bench acceptance note

The LR1121's higher sensitivity detects persistent sub-GHz bench emitters
(observed at 902.3, 912.2, 914.7 MHz) that survive warmup ambient tagging.
These produce intermittent false ADVISORY in cold-boot captures, typically
at 40-60 seconds post-warmup. This is a test environment condition, not a
code defect. The SX1262 (COM9) passes 5/5 across all Phase E runs.

Phase E acceptance is declared on the basis of:

- 5/5 COM9 cold-boot PASS
- Zero false WARNING or CRITICAL on either board
- Correct `[CAND-DELTA] legacy=ADVISORY candidate=CLEAR` chronic-noise
  filter operation on COM14 throughout (candidate engine correctly
  suppresses the aggregate 2.4 GHz diversity the legacy scorer fires on)

LR1121 field validation (with appropriate antenna and no bench emitters)
is required before deployment. The bench emitters observed during Phase E
testing cannot be replicated in code — they are real RF sources that a
production deployment would not see, and any gate tight enough to block
them would also block real low-power FHSS signals.

---

## Part 12 — What Remains Outside This Redesign

These items are known but explicitly out of scope for v2.0:

- **Buzzer and LED alert methods** — GPIO 16 passive piezo (KY-006), LED system
- **GNSS jamming/spoofing host-side algorithms** — beyond the GNSS evidence term in this engine
- **LR1121 2.4 GHz image recalibration latency** — `setBandwidth(812.5, true)` triggers ~1250ms recal; investigate but not blocking
- **LR1121 RSSI sweep GFSK params** — board-specific calibration (freqDev, rxBw) needed vs SX1262 defaults
- **Heltec V4 node integration** — distributed sensing, after v2.0 stable
- **TESTKIT-GPS / AntSDR E200** — spoofing/jamming test signal generation
- **MemPalace** — ChromaDB + MCP cross-project memory system

---

## Part 13 — Known Active Issues (as of April 13, 2026)

| Issue | Severity | Resolution |
|---|---|---|
| GPS_MIN_CNO = 6 | 🔴 MUST FIX before field deployment | Raise to 15–20 |
| LR1121 bench FP on dense LoRa | 🟡 Known | Candidate isolation (Phase D) |
| Version string bug main.cpp:422 | 🟡 Minor | Phase G.3 |
| 2.4 GHz cycle slow (~3900ms) | 🟡 Known | Phase F (moves off critical path) |
| CC work unvalidated until flashed | ⚠️ Process | Hardware flash required before trusting any sprint |

---

## Part 14 — How We Work

| Role | Responsibility |
|---|---|
| **Claude** | Architecture decisions, sprint planning, CC prompt generation, pre-flash review |
| **Claude Code (CC)** | Code implementation, file editing, git operations |
| **ND** | Final decisions, all hardware interaction, serial capture, field testing |
| **Codex/ChatGPT** | Adversarial reviews at major milestones |

**CC work is not validated until flashed.** Commits after compile-only verification are treated as unproven. Hardware validation is mandatory before trusting any sprint output.

**Git:** Author `ND <ndywoo10@gmail.com>`. Always build all three targets before committing.

**Targets:** `t3s3`, `heltec_v3`, `t3s3_lr1121`

---

*This document is the single source of truth for SENTRY-RF Detection Engine v2.0.*  
*All prior planning documents are superseded.*  
*Last updated: April 13, 2026 — Claude + Codex + ND*
