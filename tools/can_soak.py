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
import json
import math
import os
import signal
import socket
import struct
import sys
import time

CAN_FRAME = struct.Struct("=IB3x8s")

# Shared .lsc definitions (tools/lscfile.py); inline fallback keeps this file
# usable when copied around on its own.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    from lscfile import LSC_HEADER_SIZE as LSC_HEADER, LSC_REC, read_header
except ImportError:  # standalone copy without tools/lscfile.py
    LSC_HEADER = 32
    LSC_REC = struct.Struct("<HIIBB8s")

    def read_header(f):
        hdr = f.read(LSC_HEADER)
        if len(hdr) != LSC_HEADER or hdr[:4] != b"LSC1":
            raise ValueError("short or invalid .lsc header")
        if struct.unpack_from("<H", hdr, 4)[0] != 1:
            raise ValueError("unsupported .lsc version")
        flags = struct.unpack_from("<I", hdr, 10)[0]
        if flags & 2 and struct.unpack_from("<I", hdr, 28)[0] > 999:
            raise ValueError("invalid millisecond anchor")
PATTERN = b"\x11\x22\x33\x44"
MAX_CONSEC_ERRORS = 100
TICK_S = 0.001


def build_frame(idx, can_id):
    data = (idx & 0xFFFFFFFF).to_bytes(4, "little") + PATTERN
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
                          f"{int(args.ext)},8,{(sent & 0xFFFFFFFF).to_bytes(4, 'little').hex()}"
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
    result = {"sent": sent, "errors": errors, "completed": total is None or sent == total}
    print("result: " + json.dumps(result, separators=(",", ":")), flush=True)
    return 0 if errors == 0 and result["completed"] else 1


def verify_file(path, can_id, *, ext=False, first=0, expect=None, allow_empty=False):
    """Stream and strictly validate one part; return total and matched counts."""
    records = matched = 0
    want = first & 0xFFFFFFFF
    expected_flags = 1 if ext else 0
    with open(path, "rb") as f:
        read_header(f)
        while True:
            raw = f.read(LSC_REC.size)
            if not raw:
                break
            if len(raw) != LSC_REC.size:
                raise ValueError(f"truncated tail at record {records}")
            _, _, ident, frame_flags, dlc, data = LSC_REC.unpack(raw)
            index = records
            records += 1
            if frame_flags & ~7 or dlc > 8 or ident > (0x1FFFFFFF if frame_flags & 1 else 0x7FF):
                raise ValueError(f"invalid frame at record {index}")
            if can_id is not None and ident != can_id:
                continue
            if frame_flags != expected_flags or dlc != 8 or data[4:] != PATTERN:
                raise ValueError(f"wrong flags/DLC/pattern at record {index}")
            got = int.from_bytes(data[:4], "little")
            if got != want:
                raise ValueError(f"record {index} counter={got} expected={want}")
            matched += 1
            want = (want + 1) & 0xFFFFFFFF
    if matched == 0 and expect != 0 and not allow_empty:
        raise ValueError(f"no matching frames in {records} records")
    if expect is not None and matched != expect:
        raise ValueError(f"matched={matched} expected={expect} (records={records})")
    return {"records": records, "matched": matched, "next": want}


def verify(args):
    try:
        result = verify_file(args.verify, args.id, ext=args.ext, first=args.first,
                             expect=args.expect)
    except (OSError, ValueError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    print(f"verify: {result['records']} records, {result['matched']} matched, "
          f"next counter {result['next']}, no gaps/duplicates")
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--iface", default="can0")
    p.add_argument("--rate", type=float, default=1000.0, help="frames per second")
    p.add_argument("--duration", type=float, default=10.0, help="seconds; 0 = until Ctrl-C")
    p.add_argument("--id", type=lambda v: int(v, 0), default=0x123)
    p.add_argument("--ext", action="store_true", help="send/verify extended IDs")
    p.add_argument("--record", help="write accepted sends as CSV")
    p.add_argument("--verify", metavar="LSC", help="verify a captured .lsc instead")
    p.add_argument("--expect", type=int, help="expected matching frame count for --verify")
    p.add_argument("--first", type=int, default=0,
                   help="first counter value for --verify (default 0)")
    args = p.parse_args()
    if not math.isfinite(args.rate) or args.rate <= 0:
        p.error("--rate must be finite and > 0")
    if not math.isfinite(args.duration) or args.duration < 0:
        p.error("--duration must be finite and >= 0")
    if not 0 <= args.id <= (0x1FFFFFFF if args.ext else 0x7FF):
        p.error("--id out of range for frame kind")
    if args.first < 0 or (args.expect is not None and args.expect < 0):
        p.error("--first and --expect must be >= 0")
    return verify(args) if args.verify else tx(args)


if __name__ == "__main__":
    sys.exit(main())
