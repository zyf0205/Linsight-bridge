#!/usr/bin/env python3
"""Log-rotation integration test: linsightd --log-max-mb 1.

Sends 60k records through a fake device and verifies the writer split them
across parts without losing a record and without reordering.

Usage: rotation.py <linsightd-path>
"""

import glob
import json
import os
import resource
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

REC = struct.Struct("<HIIBB8s")
HEADER = struct.Struct("<BBBBB")
LSC_HEADER = 32
REC_SIZE = REC.size
MAX_BYTES = 1024 * 1024
TOTAL = 60000
BATCH = 819


def frame(ftype, payload):
    return HEADER.pack(0xAA, 0x55, ftype, len(payload) & 0xFF, len(payload) >> 8) + payload


def fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def wait_port(port, proc, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            fail(f"daemon exited early (rc={proc.returncode})")
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=1)
            s.settimeout(5)
            return s
        except OSError:
            time.sleep(0.05)
    fail(f"daemon never listened on {port}")


def ctl_roundtrip(port, msg):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.settimeout(5)
    s.sendall(msg)
    data = b""
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        data += chunk
    s.close()
    return data.decode(errors="replace")


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(1)
    daemon = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="linsight-rotate-") as tmp:
        check_rotation(daemon, tmp)
    check_retention(daemon)
    check_open_retry(daemon)
    check_buffered_failure(daemon)


def check_rotation(daemon, tmp):
    log_dir = os.path.join(tmp, "logs")
    port = free_port()
    ctl_port = free_port()
    proc = subprocess.Popen(
        [daemon, "--port", str(port), "--ctl", os.path.join(tmp, "ctl.sock"),
         "--ctl-port", str(ctl_port), "--log-dir", log_dir, "--log-max-mb", "1",
         "--http-port", "0"],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        dev = wait_port(port, proc)
        dev.settimeout(5)
        dev.sendall(frame(1, b'{"proto":32,"fw":"2.0.0","device":"fake"}'))
        sent = 0
        while sent < TOTAL:
            n = min(BATCH, TOTAL - sent)
            body = struct.pack("<H", n)
            for i in range(sent, sent + n):
                data = i.to_bytes(4, "little") + b"\x11\x22\x33\x44"
                body += REC.pack(i & 0xFFFF, i, 0x100 + (i % 8), 0, 8, data)
            dev.sendall(frame(2, body))
            sent += n

        deadline = time.time() + 15
        st = {}
        while time.time() < deadline:
            st = json.loads(ctl_roundtrip(ctl_port, b"stats\n").strip())
            if st.get("written", 0) == TOTAL:
                break
            time.sleep(0.1)
        dev.close()
        if st.get("written") != TOTAL:
            fail(f"writer did not drain: {st}")
        if st.get("dropped") != 0:
            fail(f"unexpected drop: {st}")
        if "-p" not in os.path.basename(st.get("log", "")):
            fail(f"log did not rotate: {st.get('log')}")

        files = sorted(glob.glob(os.path.join(log_dir, "*.lsc")),
                       key=os.path.getmtime)
        if len(files) < 2:
            fail(f"expected multiple parts, got {files}")
        seqs = []
        for path in files:
            with open(path, "rb") as f:
                blob = f.read()
            if blob[:4] != b"LSC1":
                fail(f"bad magic in {path}")
            if len(blob) < LSC_HEADER or (len(blob) - LSC_HEADER) % REC_SIZE:
                fail(f"bad size in {path}: {len(blob)}")
            if len(blob) > MAX_BYTES:
                fail(f"part over cap: {path} {len(blob)}")
            for off in range(LSC_HEADER, len(blob), REC_SIZE):
                seqs.append(REC.unpack_from(blob, off)[0])
        if len(seqs) != TOTAL:
            fail(f"records across parts: {len(seqs)} != {TOTAL}")
        if seqs != [i & 0xFFFF for i in range(TOTAL)]:
            fail("record order/continuity broken across parts")
        print(f"rotation: {len(files)} parts, {TOTAL} records, no loss")
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        if proc.returncode not in (0, None):
            err = proc.stderr.read().decode(errors="replace")
            print("--- daemon stderr ---")
            print(err[-2000:])


def stop_daemon(proc):
    if proc.poll() is None:
        proc.terminate()
    try:
        proc.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.communicate()


def start_daemon(daemon, tmp, extra=(), preexec_fn=None):
    port, ctl_port = free_port(), free_port()
    proc = subprocess.Popen(
        [daemon, "--port", str(port), "--ctl", os.path.join(tmp, "ctl.sock"),
         "--ctl-port", str(ctl_port), "--log-dir", os.path.join(tmp, "logs"),
         "--http-port", "0", *extra],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, preexec_fn=preexec_fn)
    dev = wait_port(port, proc)
    dev.sendall(frame(1, b'{"proto":33,"fw":"2.1.0","device":"fake"}'))
    return proc, dev, ctl_port


def send_records(dev, start, count):
    for first in range(start, start + count, BATCH):
        n = min(BATCH, start + count - first)
        body = struct.pack("<H", n)
        for i in range(first, first + n):
            body += REC.pack(i & 0xFFFF, i, 0x123, 0, 8,
                             i.to_bytes(4, "little") + b"\x11\x22\x33\x44")
        dev.sendall(frame(2, body))


def wait_stats(ctl_port, predicate, timeout=10):
    deadline = time.monotonic() + timeout
    last = {}
    while time.monotonic() < deadline:
        last = json.loads(ctl_roundtrip(ctl_port, b"stats\n"))
        if predicate(last):
            return last
        time.sleep(0.05)
    fail(f"stats condition timed out: {last}")


def check_retention(daemon):
    with tempfile.TemporaryDirectory(prefix="linsight-retain-") as tmp:
        proc, dev, ctl_port = start_daemon(
            daemon, tmp, ["--log-max-mb", "1", "--log-max-total-mb", "1"])
        try:
            send_records(dev, 0, 120000)
            wait_stats(ctl_port, lambda s: s.get("written") == 120000)
            time.sleep(1.1)
            send_records(dev, 120000, 1)
            st = wait_stats(ctl_port, lambda s: s.get("written") == 120001)
            files = glob.glob(os.path.join(tmp, "logs", "*.lsc"))
            if len(files) != 1 or files[0] != st["log"]:
                fail(f"retention did not run during the active session: {files}")
            if os.path.getsize(files[0]) > MAX_BYTES or st.get("dropped") != 0:
                fail(f"invalid retained part or drops: {st}")
            print("retention: active rotation removes old parts and protects current log")
        finally:
            dev.close()
            stop_daemon(proc)


def check_open_retry(daemon):
    with tempfile.TemporaryDirectory(prefix="linsight-open-retry-") as tmp:
        proc, dev, ctl_port = start_daemon(daemon, tmp)
        log_dir = os.path.join(tmp, "logs")
        try:
            os.rmdir(log_dir)
            send_records(dev, 0, 1)
            wait_stats(ctl_port, lambda s: s.get("dropped") == 1 and s.get("log_error"))
            os.mkdir(log_dir)
            time.sleep(2.1)
            send_records(dev, 1, 1)
            st = wait_stats(ctl_port, lambda s: s.get("written") == 1)
            if st.get("log_error") or st.get("dropped") != 1:
                fail(f"initial open did not recover in the same session: {st}")
            print("open recovery: initial failure retries without device reconnect")
        finally:
            dev.close()
            stop_daemon(proc)


def check_buffered_failure(daemon):
    def limit_file():
        signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
        resource.setrlimit(resource.RLIMIT_FSIZE, (LSC_HEADER, LSC_HEADER))

    with tempfile.TemporaryDirectory(prefix="linsight-flush-failure-") as tmp:
        proc, dev, ctl_port = start_daemon(daemon, tmp, preexec_fn=limit_file)
        try:
            send_records(dev, 0, 1)
            st = wait_stats(ctl_port, lambda s: s.get("dropped") == 1)
            if st.get("written") != 0 or st.get("log_error") != "flush failed":
                fail(f"buffered write failure reported as success: {st}")
            print("write failure: failed fflush reports an error and no committed records")
        finally:
            dev.close()
            stop_daemon(proc)


if __name__ == "__main__":
    main()
