#!/usr/bin/env python3
"""Reset a XIAO ESP32C6 and read its serial output for a fixed time.

Used by verify-rfid.sh, but handy on its own:

    python3 read_serial.py --port /dev/cu.usbmodem11201 --seconds 15
    python3 read_serial.py --port <p> --expect "MFRC522 firmware version: 0x9[12]"

Exit codes:
    0  finished (or --expect pattern matched)
    1  --expect pattern was not seen in time
    2  could not open the port / pyserial missing
"""
import argparse
import re
import sys
import time

try:
    import serial  # pyserial
except ImportError:
    sys.stderr.write("pyserial not installed. Run: python3 -m pip install pyserial\n")
    sys.exit(2)


def reset_board(ser):
    """Toggle RTS to hard-reset the ESP32-C6 (USB-serial/JTAG)."""
    ser.dtr = False
    ser.rts = True
    time.sleep(0.05)
    ser.rts = False
    time.sleep(0.2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=15)
    ap.add_argument("--expect", default=None, help="regex; exit 0 as soon as it appears")
    ap.add_argument("--no-reset", action="store_true", help="do not reset the board first")
    args = ap.parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.2)
    except Exception as exc:  # noqa: BLE001
        sys.stderr.write(f"Could not open {args.port}: {exc}\n")
        return 2

    if not args.no_reset:
        reset_board(ser)
    ser.reset_input_buffer()

    pattern = re.compile(args.expect) if args.expect else None
    buf = ""
    deadline = time.time() + args.seconds
    while time.time() < deadline:
        chunk = ser.read(256)
        if chunk:
            text = chunk.decode("utf-8", errors="replace")
            sys.stdout.write(text)
            sys.stdout.flush()
            buf += text
            if pattern and pattern.search(buf):
                sys.stdout.write(f"\n[expect] matched /{args.expect}/\n")
                ser.close()
                return 0
        else:
            time.sleep(0.02)
    ser.close()

    if pattern:
        sys.stdout.write(f"\n[expect] NOT matched /{args.expect}/ within {args.seconds:.0f}s\n")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
