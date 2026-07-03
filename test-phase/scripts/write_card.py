#!/usr/bin/env python3
"""Write an order payload from backend-full/data onto an RFID card.

This talks to the xiao-esp32c6-rfid-writer sketch over serial (SERIAL MODE),
so it does NOT need Wi-Fi. It builds the exact same JSON payload that
backend-full/src/server.js writes:  {"user_name":...,"order_number":...,"v":1}

Examples
--------
List pending writer jobs from backend-full/data/writer_jobs.csv:
    python3 write_card.py --list

Write the newest PENDING writer job to a card:
    python3 write_card.py

Write a specific order (looked up in orders.csv):
    python3 write_card.py --order ORD-20260101-ABCDEF

Write an explicit payload (no CSV lookup):
    python3 write_card.py --user alice --order ORD-1

Notes
-----
* This is for bench/manual writing. In production the XIAO runs ONLINE MODE
  and the backend API is what advances writer_jobs.csv / orders.csv status.
  This helper only writes the card; it does not mutate the CSV files.
"""
import argparse
import csv
import glob
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DATA_DIR = os.path.abspath(os.path.join(HERE, "..", "..", "backend-full", "data"))


def data_file(name):
    return os.path.join(DATA_DIR, name)


def read_rows(name):
    path = data_file(name)
    if not os.path.exists(path):
        return []
    with open(path, newline="", encoding="utf-8") as fh:
        return list(csv.DictReader(fh))


def build_payload(user_name, order_number):
    # Must byte-match server.js makeWriterPayload (no spaces, this key order).
    return json.dumps(
        {"user_name": user_name, "order_number": order_number, "v": 1},
        separators=(",", ":"),
        ensure_ascii=False,
    )


def pick_from_jobs():
    jobs = read_rows("writer_jobs.csv")
    pending = [j for j in jobs if j.get("status") in ("PENDING", "CLAIMED")]
    if not pending:
        return None
    job = pending[-1]  # newest
    return job.get("user_name", ""), job.get("order_number", "")


def pick_from_order(order_number):
    for o in read_rows("orders.csv"):
        if o.get("order_number") == order_number:
            return o.get("user_name", ""), o.get("order_number", "")
    return None


def list_jobs():
    jobs = read_rows("writer_jobs.csv")
    if not jobs:
        print(f"No writer jobs in {data_file('writer_jobs.csv')}")
        return
    print(f"{'STATUS':10} {'ORDER':28} {'USER':16} JOB_ID")
    for j in jobs:
        print(f"{j.get('status',''):10} {j.get('order_number',''):28} "
              f"{j.get('user_name',''):16} {j.get('job_id','')}")


def detect_port():
    if os.environ.get("XIAO_PORT"):
        return os.environ["XIAO_PORT"]
    for pattern in ("/dev/cu.usbmodem*", "/dev/cu.usbserial*", "/dev/cu.wchusbserial*"):
        hits = sorted(glob.glob(pattern))
        if hits:
            return hits[0]
    return None


def send_write(port, payload, seconds):
    try:
        import serial  # pyserial
    except ImportError:
        sys.stderr.write("pyserial not installed. Run: python3 -m pip install pyserial\n")
        return 2

    try:
        ser = serial.Serial(port, 115200, timeout=0.2)
    except Exception as exc:  # noqa: BLE001
        sys.stderr.write(f"Could not open {port}: {exc}\n")
        return 2

    # Opening the port may reset the board; give it a moment to boot.
    ser.dtr = False
    ser.rts = False
    time.sleep(2.0)
    ser.reset_input_buffer()

    line = f"WRITE {payload}\n"
    print(f"-> {line.strip()}")
    ser.write(line.encode("utf-8"))
    ser.flush()

    ok = False
    deadline = time.time() + seconds
    buf = ""
    while time.time() < deadline:
        chunk = ser.read(256)
        if chunk:
            text = chunk.decode("utf-8", errors="replace")
            sys.stdout.write(text)
            sys.stdout.flush()
            buf += text
            if "WRITE OK" in buf:
                ok = True
                break
            if "WRITE FAILED" in buf or "verify mismatch" in buf or "timeout" in buf.lower():
                break
        else:
            time.sleep(0.02)
    ser.close()
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", action="store_true", help="list writer jobs and exit")
    ap.add_argument("--order", help="order_number to look up in orders.csv")
    ap.add_argument("--user", help="user_name (use with --order to skip CSV lookup)")
    ap.add_argument("--port", help="serial port (default: auto-detect / $XIAO_PORT)")
    ap.add_argument("--seconds", type=float, default=25, help="how long to wait for the write")
    ap.add_argument("--dry-run", action="store_true", help="print the payload, do not send")
    args = ap.parse_args()

    if args.list:
        list_jobs()
        return 0

    # Resolve the (user_name, order_number) pair.
    if args.user and args.order:
        pair = (args.user, args.order)
    elif args.order:
        pair = pick_from_order(args.order)
        if not pair:
            sys.stderr.write(f"order_number {args.order} not found in orders.csv\n")
            return 1
    else:
        pair = pick_from_jobs()
        if not pair:
            sys.stderr.write("No PENDING/CLAIMED writer jobs found. "
                             "Create an order in the backend first, or pass --order.\n")
            return 1

    user_name, order_number = pair
    payload = build_payload(user_name, order_number)
    print(f"user_name={user_name}  order_number={order_number}")
    print(f"payload={payload}  ({len(payload)} bytes, max 96)")
    if len(payload) > 96:
        sys.stderr.write("Payload exceeds 96 bytes (6 blocks x 16). Shorten the fields.\n")
        return 1
    if args.dry_run:
        return 0

    port = args.port or detect_port()
    if not port:
        sys.stderr.write("No serial port found. Plug in the XIAO or pass --port.\n")
        return 1
    print(f"port={port}")
    print("Place a card on the reader now...")
    return send_write(port, payload, args.seconds)


if __name__ == "__main__":
    sys.exit(main())
