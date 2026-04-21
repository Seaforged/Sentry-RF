#!/usr/bin/env python3
"""
SENTRY-RF → DragonSync ZMQ bridge.

Reads lines from the SENTRY-RF board over USB serial, filters lines prefixed
with "[ZMQ] ", strips the prefix, and republishes the JSON payload as ZMQ PUB
messages on tcp://*:<zmq-port>. Companion to the ENABLE_ZMQ_OUTPUT=1 firmware
(Phase L).

Usage:
    python zmq_bridge.py --port COM14 --zmq-port 4227

Defaults match the SENTRY-RF development bench on Windows:
    --port COM14       (LR1121 board, see memory/hardware_com_mapping.md)
    --baud 115200      (matches Serial.begin in main.cpp)
    --zmq-port 4227    (DragonSync convention, 4221-4226 reserved elsewhere)

The bridge handles serial disconnects gracefully: on any serial exception it
closes the port, waits 5 seconds, and tries to reopen. ZMQ publishers do not
reconnect — subscribers are responsible for reconnecting to the PUB socket.
"""

from __future__ import annotations

import argparse
import datetime
import json
import sys
import time
from typing import Optional

try:
    import serial  # pyserial>=3.5
except ImportError:
    print("ERROR: pyserial not installed. Run: pip install -r requirements_zmq.txt",
          file=sys.stderr)
    sys.exit(2)

try:
    import zmq  # pyzmq>=25
except ImportError:
    print("ERROR: pyzmq not installed. Run: pip install -r requirements_zmq.txt",
          file=sys.stderr)
    sys.exit(2)


ZMQ_PREFIX = b"[ZMQ] "   # must match main.cpp exactly — 6 bytes


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Bridge SENTRY-RF [ZMQ] serial lines to a real ZMQ PUB socket",
    )
    p.add_argument("--port", default="COM14",
                   help="Serial port (default: COM14 — see hardware mapping)")
    p.add_argument("--baud", type=int, default=115200,
                   help="Serial baud rate (default: 115200)")
    p.add_argument("--zmq-port", type=int, default=4227,
                   help="ZMQ PUB port (default: 4227, DragonSync convention)")
    p.add_argument("--zmq-bind", default="tcp://*",
                   help="ZMQ bind spec (default: tcp://* — all interfaces)")
    p.add_argument("--reconnect-s", type=float, default=5.0,
                   help="Seconds to wait before retrying a failed serial open")
    p.add_argument("--quiet", action="store_true",
                   help="Do not echo published messages to stdout")
    return p.parse_args()


def open_serial(port: str, baud: int) -> Optional[serial.Serial]:
    """Open the serial port or return None on failure (caller retries)."""
    try:
        s = serial.Serial(port, baud, timeout=1.0)
        # On ESP32-S3 native USB-CDC the DTR/RTS state can reset the chip.
        # Leave both deasserted so the board keeps running.
        s.setDTR(False)
        s.setRTS(False)
        return s
    except serial.SerialException as exc:
        print(f"[bridge] could not open {port}: {exc}", file=sys.stderr)
        return None


def main() -> int:
    args = parse_args()

    ctx = zmq.Context.instance()
    pub = ctx.socket(zmq.PUB)
    bind_addr = f"{args.zmq_bind}:{args.zmq_port}"
    pub.bind(bind_addr)
    print(f"[bridge] ZMQ PUB bound to {bind_addr}")
    # Small linger so a SIGINT doesn't drop the last message mid-flight.
    pub.setsockopt(zmq.LINGER, 250)

    ser: Optional[serial.Serial] = None
    try:
        while True:
            if ser is None:
                print(f"[bridge] opening serial {args.port} @ {args.baud}")
                ser = open_serial(args.port, args.baud)
                if ser is None:
                    time.sleep(args.reconnect_s)
                    continue

            try:
                raw = ser.readline()
            except serial.SerialException as exc:
                print(f"[bridge] serial read failed: {exc} — reopening in "
                      f"{args.reconnect_s}s", file=sys.stderr)
                try:
                    ser.close()
                except Exception:
                    pass
                ser = None
                time.sleep(args.reconnect_s)
                continue

            if not raw:
                continue  # timeout with no data — keep reading

            # Only [ZMQ]-prefixed lines are bridge payloads. Anything else
            # is normal serial logging and gets ignored.
            if not raw.startswith(ZMQ_PREFIX):
                continue

            # Strip the exactly-6-byte prefix per the firmware contract.
            payload = raw[len(ZMQ_PREFIX):].strip()
            if not payload:
                continue

            # Validate JSON before publishing so malformed lines don't
            # poison downstream subscribers. Parsing also strips any CR/LF.
            try:
                doc = json.loads(payload.decode("utf-8", errors="replace"))
            except (json.JSONDecodeError, UnicodeDecodeError) as exc:
                print(f"[bridge] dropped malformed JSON: {exc}; line={payload!r}",
                      file=sys.stderr)
                continue

            # Publish as a compact single-line JSON string. DragonSync
            # consumers parse the whole frame as one JSON object.
            out = json.dumps(doc, separators=(",", ":"))
            pub.send_string(out)

            if not args.quiet:
                ts = datetime.datetime.utcnow().strftime("%H:%M:%S.%f")[:-3]
                print(f"{ts}Z  {out}")

    except KeyboardInterrupt:
        print("\n[bridge] interrupted — closing")
    finally:
        if ser is not None:
            try:
                ser.close()
            except Exception:
                pass
        pub.close()
        ctx.term()

    return 0


if __name__ == "__main__":
    sys.exit(main())
