# SENTRY-RF

This repository uses the **mex** project memory scaffold. All persistent context lives in `.mex/`.

## Session bootstrap

At the start of every session:

1. Read `.mex/AGENTS.md` — project identity, non-negotiables, commands
2. Read `.mex/ROUTER.md` — current project state, routing table, behavioural contract

Then follow the `ROUTER.md` routing table to load the context files relevant to your task.

## Structure

```
.mex/
  AGENTS.md           # always-loaded anchor (read first)
  ROUTER.md           # session bootstrap + routing table
  SETUP.md            # mex setup notes
  SYNC.md             # drift detection instructions
  context/
    architecture.md   # FreeRTOS pipeline, scan loop, detection engine
    stack.md          # RadioLib, SparkFun GNSS, Adafruit SSD1306, PlatformIO
    conventions.md    # naming, structure, verify checklist
    decisions.md      # AAD persistence gate, LR1121 quirks, diversity gate rationale
    setup.md          # build/flash commands + common issues per board
  patterns/
    INDEX.md          # lookup table — check before starting any task
    README.md         # how to write a pattern
```

## Why this structure

The previous flat `CLAUDE.md` was 263 lines of dense reference material that had to be re-read every session. mex splits it into task-scoped files that load on demand, so the agent only pulls architecture details when working on architecture, stack details when choosing libraries, etc. The pattern system grows over time as we solve recurring task types.

The original CLAUDE.md is preserved at `CLAUDE.md.backup` for reference and will be removed once the mex scaffold is validated across a few sessions.
