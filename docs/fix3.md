Research and propose exact fixes for findings 6.1, 6.2, and 6.3 from the final audit. Read-only first, then propose exact code changes. Do not implement yet.
Finding 6.1 — MEDIUM: [GNSS] position jump Serial.printf unprotected
File: src/gnss_integrity.cpp around line 79–80.
Root cause: a Serial.printf for the position jump spoofing indicator runs in gpsReadTask on Core 0 without SERIAL_SAFE. It predates the serial sweep and was missed.
Proposed fix: wrap it in SERIAL_SAFE(...). Show the exact before/after replacement.
Finding 6.2 — LOW: loRaScanTask log-block uses 50ms-timeout take, not blocking
File: src/main.cpp around line 386–487.
Root cause: the per-cycle [SCAN]/[CAD] log block takes serialMutex with pdMS_TO_TICKS(50) and silently drops the entire block on timeout. Inconsistent with the blocking SERIAL_SAFE discipline used everywhere else.
The block is ~100 lines with 5 printf sites and intermediate non-printf code between them, so a single monolithic SERIAL_SAFE wrapper is not clean.
Research the best approach from these options:

A: Split into 5 individual SERIAL_SAFE(...) calls, one per printf site, accepting that a burst can interleave between them (each line is complete, just possibly reordered)
B: Keep the 50ms-timeout take but add a silent drop counter that logs via SERIAL_SAFE once per 60 seconds so drops are observable
C: Leave as-is — these are diagnostic logs only, the timeout-drop is acceptable, document it with a comment
Propose the best option with exact code showing what changes.

Finding 6.3 — LOW: ZMQ snapshot missing cad* and lastRID field mirror
File: src/detection_engine.cpp around line 1580–1592.
Root cause: the post-transition ZMQ snapshot overrides threatLevel, score, anchor, mask etc. but not cadDiversity, cadConfirmed, cadTotalTaps, or lastRID. The ZMQ threat frame includes div and cad_conf fields — these show the pre-transition CAD numbers while the threat level is post-transition.
Research whether this is worth fixing at all: the CAD diversity that triggered the transition is arguably what you want in the emit (not the next cycle's values). Or is there a clean way to mirror the correct post-transition CAD fields?
Propose either: the exact mirror code to add, OR a comment explaining why this is correct behavior and should be documented as-is.
After researching all three, produce a summary recommendation: which of these should be fixed before the v2.0.0 tag (must-fix vs nice-to-have), and which can go into v2.0.1.