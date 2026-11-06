#!/usr/bin/env python3
"""SocketCAN soak generator with a 32-bit payload counter.

The first 4 data bytes are a little-endian send index, followed by 11 22 33 44.
`--record` writes one CSV row per accepted frame. `--verify` checks a captured
.lsc against the exact expected counter sequence (no gaps, duplicates or
reordering), which is the frame-level evidence used by the M3.1 soak.

Examples:
    python3 tools/can_soak.py --iface can0 --rate 1000 --duration 600 \\
        --record /tmp/soak-tx.csv
    python3 tools/can_soak.py --verify /tmp/can-logs/can-....lsc --expect 600000
"""

import argparse
import csv
import os
import signal
import socket
import struct
import sys
import time

CAN_FRAME = struct.Struct("=IB3x8s")
LSC_HEADER = 32
LSC_REC = struct.Struct("<HIIBB8s")
PATTERN = b"\x11\x22\x33\x44"
MAX_CONSEC_ERRORS = 100
TICK_S = 0.001


def build_frame(idx, can_id):
    data = idx.to_bytes(4, "little") + PATTERN
    return CAN_FRAME.pack(can_id, 8, data)


def tx(args):
    s = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    s.bind((args.iface,))
    s.settimeout(0.005)
    total = int(args.rate * args.duration) if args.duration else None
    can_id = args.id | (socket.CAN_EFF_FLAG if args.ext else 0)

    rec = None
    if args.record:
        rec = open(args.record, "w", newline="")
        writer = csv.writer(rec)
        writer.writerow(["index", "monotonic_ns", "id", "ext", "dlc", "data"])

    stop = False

    def on_sigint(_sig, _frame):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGINT, on_sigint)
    signal.signal(signal.SIGTERM, on_sigint)

    sent = 0
    errors = 0
    consec = 0
    start = time.monotonic()
    next_report = start + 5.0
    print(f"tx {args.iface} id=0x{args.id:X} rate={args.rate} fps "
          f"duration={args.duration or 'until Ctrl-C'}s", flush=True)

    try:
        while not stop and (total is None or sent < total):
            if consec >= MAX_CONSEC_ERRORS:
                print(f"abort: {consec} consecutive send errors", file=sys.stderr)
                break
            deadline = start + (sent + 1) / args.rate
            now = time.monotonic()
            if deadline - now > TICK_S:
                time.sleep(deadline - now - TICK_S)
            while time.monotonic() < deadline:
                pass
            try:
                s.send(build_frame(sent, can_id))
            except OSError as exc:
                errors += 1
                consec += 1
                print(f"send {sent} failed: {exc}", file=sys.stderr)
                if consec >= 8 and consec % 8 == 0:
                    time.sleep(0.2)  # let the driver queue drain
                continue
            consec = 0
            if rec:
                rec.write(f"{sent},{time.monotonic_ns()},{args.id:X},"
                          f"{int(args.ext)},8,{sent.to_bytes(4, 'little').hex()}"
                          f"{PATTERN.hex()}\n")
            sent += 1
            if time.monotonic() >= next_report:
                elapsed = time.monotonic() - start
                print(f"sent={sent} actual={sent / elapsed:.1f} fps "
                      f"errors={errors}", flush=True)
                next_report += 5.0
    finally:
        if rec:
            rec.close()
        s.close()
    elapsed = time.monotonic() - start
    print(f"done: sent={sent} in {elapsed:.1f}s actual={sent / elapsed:.1f} fps "
          f"errors={errors}", flush=True)
    return 0 if errors == 0 else 1


def verify(args):
    with open(args.verify, "rb") as f:
        blob = f.read()
    if blob[:4] != b"LSC1":
        print(f"FAIL: bad magic {blob[:4]!r}", file=sys.stderr)
        return 1
    n = (len(blob) - LSC_HEADER) // LSC_REC.size
    if args.expect is not None and n != args.expect:
        print(f"FAIL: records={n} expected={args.expect}", file=sys.stderr)
        return 1
    want = 0
    bad = 0
    for i in range(n):
        seq, ts, ident, flags, dlc, data = LSC_REC.unpack_from(
            blob, LSC_HEADER + i * LSC_REC.size)
        if dlc != 8:
            print(f"FAIL: record {i} dlc={dlc}", file=sys.stderr)
            return 1
        if args.id is not None and ident != args.id:
            continue
        got = int.from_bytes(data[:4], "little")
        if got != want:
            if bad < 10:
                print(f"FAIL: record {i} counter={got} expected={want}",
                      file=sys.stderr)
            bad += 1
        want = got + 1
    if bad:
        print(f"FAIL: {bad} counter mismatches in {n} records", file=sys.stderr)
        return 1
    print(f"verify: {n} records, counters 0..{want - 1}, no gaps/duplicates")
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--iface", default="can0")
    p.add_argument("--rate", type=float, default=1000.0, help="frames per second")
    p.add_argument("--duration", type=float, default=10.0, help="seconds; 0 = until Ctrl-C")
    p.add_argument("--id", type=lambda v: int(v, 0), default=0x123)
    p.add_argument("--ext", action="store_true", help="send extended IDs")
    p.add_argument("--record", help="write accepted sends as CSV")
    p.add_argument("--verify", metavar="LSC", help="verify a captured .lsc instead")
    p.add_argument("--expect", type=int, help="expected record count for --verify")
    args = p.parse_args()
    if args.rate <= 0:
        p.error("--rate must be > 0")
    return verify(args) if args.verify else tx(args)


if __name__ == "__main__":
    sys.exit(main())
