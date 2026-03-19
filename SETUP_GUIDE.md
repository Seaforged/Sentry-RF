# SENTRY-RF Repository Setup Guide

## Step 1: Create the GitHub repo

Go to https://github.com/organizations/Seaforged/repositories/new

- **Repository name:** SENTRY-RF
- **Description:** Passive drone RF detection + GNSS jamming/spoofing monitor for ESP32-S3
- **Visibility:** Public
- **DO NOT** initialize with README, .gitignore, or license (we already have those)
- Click "Create repository"

## Step 2: Initialize and push locally

Open a terminal and run these commands (replace the path if needed):

```bash
# Navigate to wherever you keep your projects
cd ~/projects  # or wherever you want

# Create the project directory and move into it
mkdir SENTRY-RF
cd SENTRY-RF

# Initialize git
git init
git branch -M main

# Copy in the files from this package (CLAUDE.md, LICENSE, .gitignore, README.md)
# Or if you downloaded them, just make sure they're in this directory

# Stage everything
git add .

# First commit
git commit -m "Initial project setup: CLAUDE.md, README, LICENSE, .gitignore"

# Connect to GitHub and push
git remote add origin https://github.com/Seaforged/SENTRY-RF.git
git push -u origin main
```

## Step 3: Set up the Claude Code project

1. Open Claude Code (terminal: `claude` command)
2. Navigate to the SENTRY-RF directory: `cd ~/projects/SENTRY-RF`
3. Claude Code will automatically detect and read the CLAUDE.md file
4. Add the sprint roadmap as project knowledge:
   - In Claude Code settings/project, add the SPRINT_ROADMAP.md file
   - This gives Claude Code the full 8-sprint plan as persistent context

## Step 4: Verify everything works

In Claude Code, ask:
> "What is this project and what sprint are we on?"

Claude Code should respond with details about SENTRY-RF and confirm we're starting Sprint 1.

Then verify PlatformIO:
```bash
# Make sure PlatformIO CLI is available
pio --version

# If not installed:
pip install platformio
```

## Step 5: Start Sprint 1

Tell Claude Code:
> "Let's start Sprint 1. Create the PlatformIO project structure with platformio.ini, board_config.h, version.h, and main.cpp. Target both the LilyGo T3S3 and Heltec V3. The goal is to verify LoRa radio init, OLED display, and serial output on both boards."

Claude Code has all the context it needs from CLAUDE.md to build this correctly.

## Notes

- The boards currently have DEMAN JR firmware — PlatformIO will overwrite it on first upload
- If you want to keep DEMAN JR available, note which board has which firmware before flashing
- You can always re-flash DEMAN JR later from its own repo
