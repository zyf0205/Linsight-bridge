#!/usr/bin/env python3
"""End-to-end integration test: fake device -> linsightd bridge -> --dump.

Usage: integration.py <linsightd-path>
Exits 0 on success, 1 on failure.

Assertions:
  1. frame passthrough format  (candump lines: std/ext/RTR, data hex)
  2. time anchoring            (HELLO uptime_ms anchors dump timestamps)
  3. PING -> PONG              (echoed on the device socket)
  4. resend dedupe             (identical batch -> no new dump lines)
  5. reconnect backlog dedupe  (same boot: old ts replayed = skipped,
                                only new records appear)
  6. generation-change flood   (20x819 flood, then a new boot's 100 fresh
                                low-ts records: none lost, none reordered)
  7. clean SIGTERM exit        (rc 0, "exited cleanly")
"""

import os
import re
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

REC = struct.Struct("<HIIBB8s")  # seq u16 | ts_ms u32 | id u32 | flags u8 | dlc u8 | data[8]
HEADER = struct.Struct("<BBBBB")  # AA 55 type len_lo len_hi


def frame(ftype, payload):
    return HEADER.pack(0xAA, 0x55, ftype, len(payload) & 0xFF, len(payload) >> 8) + payload


def batch_frame(records):
    body = struct.pack("<H", len(records))
    for seq, ts, ident, flags, data in records:
        body += REC.pack(seq, ts, ident, flags, len(data),
                         bytes(data).ljust(8, b"\0"))
    return frame(2, body)  # CAN_BATCH


def fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def _dump_lines(path):
    with open(path) as f:
        return f.read().splitlines()


def _wait_min_lines(path, n, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        lines = _dump_lines(path)
        if len(lines) >= n:
            return lines
        time.sleep(0.05)
    fail(f"dump has {len(_dump_lines(path))} lines, expected >= {n}")


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(1)
    daemon = sys.argv[1]

    tmp = tempfile.mkdtemp(prefix="linsight-test-")
    dump_path = os.path.join(tmp, "dump.log")
    port = _free_port()

    proc = subprocess.Popen(
        [daemon, "--port", str(port), "--dump", dump_path],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )

    def _stop_daemon(sig=signal.SIGTERM):
        if proc.poll() is None:
            proc.send_signal(sig)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()

    try:
        _run_test(port, dump_path, proc)
    except BaseException:
        _stop_daemon(signal.SIGKILL)
        err = proc.stderr.read().decode(errors="replace")
        print("--- daemon stderr ---")
        print(err[-3000:])
        raise
    _stop_daemon()
    err = proc.stderr.read().decode(errors="replace")
    if proc.returncode != 0:
        fail(f"daemon exit rc={proc.returncode}: {err[-500:]}")
    if "exited cleanly" not in err:
        fail(f"missing 'exited cleanly' on stderr: {err[-500:]}")
    print("integration: all passed")


def _run_test(port, dump_path, proc):
    dev = _wait_port(port, proc)
    dev.settimeout(5)

    # ---- 1+2: frame passthrough format and time anchoring -----------------
    # HELLO uptime 5000 anchors uptime 0 to wall-5000ms; records at
    # ts 5000..5003 therefore stamp at ~the HELLO wall time.
    hello = b'{"proto":32,"fw":"2.0.0","device_id":"itest","boot":1,"uptime_ms":5000}'
    t_hello = time.time()
    dev.sendall(frame(1, hello))
    records = [
        (0, 5000, 0x123, 0x00, [0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88]),
        (1, 5001, 0x1ABCDEF, 0x01, [0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11]),
        (2, 5002, 0x123, 0x02, []),                    # RTR
        (3, 5003, 0x123, 0x00, [0xA1, 0xB2]),          # short dlc
    ]
    dev.sendall(batch_frame(records))

    lines = _wait_min_lines(dump_path, 4)
    if len(lines) != 4:
        fail(f"dump lines after first batch: {lines}")
    pat = re.compile(r"^\((\d+)\.(\d{6})\) can0 ")
    expected_suffix = [
        "can0 123#1122334455667788",
        "can0 01ABCDEF#AABBCCDDEEFF0011",
        "can0 123#R",
        "can0 123#A1B2",
    ]
    for i, (line, suffix) in enumerate(zip(lines, expected_suffix)):
        if not line.endswith(suffix):
            fail(f"dump line {i} format: {line!r} (want suffix {suffix!r})")
        if not pat.match(line):
            fail(f"dump line {i} timestamp: {line!r}")
    m = pat.match(lines[0])
    wall0 = int(m.group(1)) + int(m.group(2)) / 1e6
    if abs(wall0 - t_hello) > 3.0:
        fail(f"timestamp not anchored to HELLO wall: {lines[0]!r} vs {t_hello}")
    print("PASS 1+2: dump format + time anchoring")

    # ---- 3: PING -> PONG --------------------------------------------------
    dev.sendall(frame(5, b"PING"))  # PING/PONG payloads are capped at 4 bytes
    ftype, payload = _recv_frame(dev)
    if ftype != 6 or payload != b"PING":
        fail(f"PING->PONG: type={ftype} payload={payload!r}")
    print("PASS 3: PING->PONG")

    # ---- 4: resend dedupe -------------------------------------------------
    dev.sendall(batch_frame(records))  # identical ts/seq replay
    time.sleep(0.5)
    lines = _dump_lines(dump_path)
    if len(lines) != 4:
        fail(f"resend dedupe: dump grew to {len(lines)} lines")
    print("PASS 4: resend dedupe")

    # ---- 5: reconnect backlog dedupe (same boot) --------------------------
    dev.close()
    time.sleep(0.2)
    dev = socket.create_connection(("127.0.0.1", port), timeout=5)
    dev.settimeout(5)
    # Same boot: dedupe marks survive the reconnect, so the replayed old-ts
    # records must be skipped and only the new ones appear.
    dev.sendall(frame(1, b'{"proto":32,"fw":"2.0.0","device_id":"itest","boot":1,'
                         b'"uptime_ms":5100}'))
    backlog = records + [
        (4, 5010, 0x123, 0x00, [0x01]),
        (5, 5011, 0x123, 0x00, [0x02]),
    ]
    dev.sendall(batch_frame(backlog))
    lines = _wait_min_lines(dump_path, 6)
    if len(lines) != 6:
        fail(f"backlog dedupe: dump has {len(lines)} lines, expected 6")
    if not lines[4].endswith("can0 123#01") or not lines[5].endswith("can0 123#02"):
        fail(f"backlog dedupe kept wrong records: {lines[4:]}")
    print("PASS 5: reconnect backlog dedupe")

    # ---- 6: generation-change flood + fresh session -----------------------
    dev.close()
    time.sleep(0.2)
    # New boot clears the dedupe marks: the flood is accepted wholesale.
    dev = socket.create_connection(("127.0.0.1", port), timeout=5)
    dev.settimeout(30)
    dev.sendall(frame(1, b'{"proto":32,"fw":"2.0.0","device_id":"itest","boot":2,'
                         b'"uptime_ms":20000}'))
    flood_n = 20 * 819
    for k in range(20):
        batch = []
        for j in range(819):
            i = k * 819 + j
            batch.append((i, 1000 + i, 0x321, 0x00,
                          [i & 0xFF, (i >> 8) & 0xFF]))
        dev.sendall(batch_frame(batch))
    dev.close()

    # Wait until every flood record is in the dump before the next connection:
    # a new accept replaces the old session and would drop data still buffered
    # on the closed socket.
    _wait_min_lines(dump_path, 6 + flood_n, timeout=30)

    # Generation change (boot 2 -> 3) clears marks again: 100 fresh records
    # with low ts values must arrive whole and in order.
    dev = socket.create_connection(("127.0.0.1", port), timeout=5)
    dev.settimeout(10)
    dev.sendall(frame(1, b'{"proto":32,"fw":"2.0.0","device_id":"itest","boot":3,'
                         b'"uptime_ms":0}'))
    fresh = [(i, i, 0x456, 0x00, [0xAA, i & 0xFF]) for i in range(100)]
    dev.sendall(batch_frame(fresh))
    dev.close()

    total = 6 + flood_n + 100
    lines = _wait_min_lines(dump_path, total, timeout=30)
    if len(lines) != total:
        fail(f"flood integrity: dump has {len(lines)} lines, expected {total}")

    for i in range(flood_n):
        want = f"321#{i & 0xFF:02X}{(i >> 8) & 0xFF:02X}"
        if not lines[6 + i].endswith(want):
            fail(f"flood record {i} wrong: {lines[6 + i]!r} (want {want!r})")
    for i in range(100):
        want = f"456#AA{i:02X}"
        if not lines[6 + flood_n + i].endswith(want):
            fail(f"fresh record {i} wrong: {lines[6 + flood_n + i]!r} (want {want!r})")
    print("PASS 6: generation-change flood (20x819) + 100 fresh, no loss/disorder")


def _wait_port(port, proc, timeout=5.0):
    """Connect once the daemon's listener is up (avoids a bind race)."""
    deadline = time.time() + timeout
    last_err = None
    while time.time() < deadline:
        if proc.poll() is not None:
            fail(f"daemon exited early (rc={proc.returncode})")
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=1)
            s.settimeout(5)
            return s
        except OSError as e:
            last_err = e
            time.sleep(0.05)
    fail(f"daemon never listened on {port}: {last_err}")


def _free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def _recv_frame(sock):
    """Read one protocol frame (header + payload), tolerating TCP splits."""
    buf = b""
    while len(buf) < 5:
        chunk = sock.recv(5 - len(buf))
        if not chunk:
            fail("device socket closed while reading header")
        buf += chunk
    magic, magic2, ftype, lo, hi = buf
    if magic != 0xAA or magic2 != 0x55:
        fail(f"bad header from daemon: {buf!r}")
    plen = lo | (hi << 8)
    payload = b""
    while len(payload) < plen:
        chunk = sock.recv(plen - len(payload))
        if not chunk:
            fail("device socket closed while reading payload")
        payload += chunk
    return ftype, payload


if __name__ == "__main__":
    main()
