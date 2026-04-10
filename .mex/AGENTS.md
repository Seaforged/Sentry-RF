---
name: agents
description: Always-loaded project anchor. Read this first. Contains project identity, non-negotiables, commands, and pointer to ROUTER.md for full context.
last_updated: 2026-04-10
---

# SENTRY-RF

## What This Is

Open-source passive drone RF detector and GNSS integrity monitor on ESP32-S3 with SX1262 or LR1121 LoRa radios — detects drone FHSS control links via LoRa CAD and identifies Remote ID broadcasts via WiFi.

## Non-Negotiables

- Never hardcode pin numbers — always use symbols from `include/board_config.h`
- All code must compile clean for **all three targets**: `t3s3`, `heltec_v3`, `t3s3_lr1121`
- Board-specific code uses `#ifdef BOARD_T3S3 / BOARD_HELTEC_V3 / BOARD_T3S3_LR1121` guards
- Always connect the antenna before powering on a board — transmitting without one damages the SX1262/LR1121
- Never push to `origin` without explicit user confirmation

## Commands

- Build one target: `pio run -e t3s3` (or `heltec_v3`, `t3s3_lr1121`)
- Build all three: `pio run -e t3s3 -e heltec_v3 -e t3s3_lr1121`
- Flash: `pio run -e t3s3 --target upload --upload-port COM9`
- Monitor: `pio device monitor -b 115200 -p COM9`
- List devices: `pio device list`

## Scaffold Growth

After every task: if no pattern exists for the task type you just completed, create one. If a pattern or context file is now out of date, update it. The scaffold grows from real work, not just setup. See the GROW step in `ROUTER.md` for details.

## Navigation

At the start of every session, read `ROUTER.md` before doing anything else.
For full project context, patterns, and task guidance — everything is there.
