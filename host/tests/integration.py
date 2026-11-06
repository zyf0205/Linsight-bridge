#!/usr/bin/env python3
"""End-to-end integration test: fake device -> linsightd -> .lsc -> linsight-cli.

Usage: integration.py <linsightd-path> <linsight-cli-path>
Exits 0 on success, 1 on failure.
"""

import json
import os
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor

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


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    daemon, cli = sys.argv[1], sys.argv[2]

    tmp = tempfile.mkdtemp(prefix="linsight-test-")
    ctl = os.path.join(tmp, "ctl.sock")
    log_dir = os.path.join(tmp, "logs")
    port = _free_port()
    ctl_port = _free_port()
    http_port = _free_port()
    web_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "web"))

    proc = subprocess.Popen(
        [daemon, "--port", str(port), "--ctl", ctl, "--ctl-port", str(ctl_port),
         "--log-dir", log_dir, "--http-port", str(http_port),
         "--http-bind", "127.0.0.1", "--web-dir", web_dir],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )

    def _stop_daemon():
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()

    try:
        _run_test(port, ctl_port, http_port, ctl, cli, log_dir, proc)
    except BaseException:
        _stop_daemon()
        err = proc.stderr.read().decode(errors="replace")
        print("--- daemon stderr ---")
        print(err[-3000:])
        raise
    finally:
        _stop_daemon()
    print("integration: all passed")


def _run_test(port, ctl_port, http_port, ctl, cli, log_dir, proc):
    dev = _wait_port(port, proc)
    dev.settimeout(5)

    # Subscribe a live consumer over the new loopback TCP control port before
    # the records arrive. `live` must be a newline-framed candump stream.
    live = socket.create_connection(("127.0.0.1", ctl_port), timeout=5)
    live.settimeout(5)
    live.sendall(b"live\n")
    time.sleep(0.2)

    # Dashboard SSE stream, subscribed before any records so the very first
    # broadcast lands here. Headers arrive immediately.
    sse, sse_buf = _sse_connect(http_port)

    # HELLO, then 10 records in two batches, then STATS.
    dev.sendall(frame(1, b'{"proto":32,"fw":"2.0.0","device":"fake"}'))
    records = [
        (i, 1000 + i * 10, (0x123 if i % 2 else 0x1ABCDEF),
         (0x01 if i % 2 else 0x00), [i, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88][:5])
        for i in range(10)
    ]
    dev.sendall(batch_frame(records[:6]))
    dev.sendall(batch_frame(records[6:]))
    dev.sendall(frame(3, b'{"fps":4330,"load_permille":123,"total":10}'))
    time.sleep(0.3)

    live_data = b""
    deadline = time.time() + 3
    while live_data.count(b"\n") < 10 and time.time() < deadline:
        chunk = live.recv(4096)
        if not chunk:
            break
        live_data += chunk
    live.close()
    if live_data.count(b"\n") != 10:
        fail(f"live stream lines: {live_data!r}")
    if not live_data.endswith(b"\n"):
        fail(f"live stream not newline-terminated: {live_data[-40:]!r}")
    live_text = live_data.decode(errors="replace")
    if "can0" not in live_text or "01ABCDEF" not in live_text or "00000123" not in live_text:
        fail(f"live stream content: {live_text!r}")

    # Dashboard page + JSON stats (same epoll loop as device and CLI).
    status, head, body = _http_get(http_port, "/")
    if b"200" not in status or b"text/html" not in head:
        fail(f"dashboard status/type: {status!r} {head[:120]!r}")
    if b"LinSight" not in body:
        fail("dashboard page content missing")

    status, head, body = _http_get(http_port, "/api/stats")
    if b"200" not in status or b"application/json" not in head:
        fail(f"stats status/type: {status!r} {head[:120]!r}")
    js = json.loads(body)
    if js.get("device") is not True or js.get("records") != 10:
        fail(f"http stats: {js}")
    if js.get("stats", {}).get("load_permille") != 123:
        fail(f"http stats did not relay device payload: {js}")

    # SSE: the batches above must have been broadcast as JSON arrays.
    ev, sse_buf = _wait_sse_data(sse, sse_buf)
    batch = json.loads(ev)
    if not isinstance(batch, list) or not batch:
        fail(f"sse payload: {ev!r}")
    hit = [r for r in batch if r.get("i") == 0x123]
    if not hit:
        fail(f"sse batch missing 0x123: {batch}")
    rec = hit[0]
    if rec.get("b") != "0122334455" or rec.get("d") != 5 or rec.get("f") != 1:
        fail(f"sse record shape: {rec}")

    # Dashboard control is asynchronous: the daemon forwards CONTROL and the
    # browser learns the outcome from fresh STATS.
    _web_control(dev, http_port, "pause")
    _web_control(dev, http_port, "resume")
    status, head, body = _http_get(http_port, "/api/control?cmd=nope")
    if json.loads(body).get("ok") is not False:
        fail(f"bad control accepted: {body!r}")
    sse.close()

    # stats over the control socket.
    out = _ctl_roundtrip(ctl, b"stats\n")
    st = json.loads(out.strip())
    if st.get("records") != 10:
        fail(f"stats records: {st}")
    if st.get("written") != 10:
        fail(f"stats written: {st}")
    if not st.get("log"):
        fail(f"stats missing log path: {st}")
    log_path = st["log"]
    if not os.path.exists(log_path):
        fail(f"log file missing: {log_path}")

    # The loopback TCP control endpoint must expose the same command set.
    tcp_st = json.loads(_tcp_roundtrip(ctl_port, b"stats\n").strip())
    if tcp_st.get("records") != 10:
        fail(f"tcp stats records: {tcp_st}")

    def _control_roundtrip(cmd, needle):
        """CLI sends cmd; device sees CONTROL, answers ACK; CLI gets the echo."""
        with ThreadPoolExecutor(max_workers=1) as ex:
            fut = ex.submit(_ctl_roundtrip, ctl, cmd)
            ftype, payload = _recv_frame(dev)
            if ftype != 7 or needle not in payload:
                fail(f"expected CONTROL {cmd.decode()}, got type={ftype} {payload!r}")
            _reply_ack(dev, payload)
            ack = fut.result(timeout=5)
        if '"code":0' not in ack:
            fail(f"expected ACK echo, got: {ack!r}")

    _control_roundtrip(b"pause\n", b"pause")
    _control_roundtrip(b"resume\n", b"resume")

    # .lsc content check: header + 10 raw records.
    with open(log_path, "rb") as f:
        blob = f.read()
    if blob[:4] != b"LSC1":
        fail(f"bad .lsc magic: {blob[:4]!r}")
    if len(blob) != 32 + 10 * REC.size:
        fail(f".lsc size {len(blob)} != {32 + 10 * REC.size}")
    seqs = [REC.unpack_from(blob, 32 + i * REC.size)[0] for i in range(10)]
    if seqs != list(range(10)):
        fail(f"record seqs wrong: {seqs}")

    # Export via the CLI and verify shape.
    out_txt = os.path.join(log_dir, "export.txt")
    r = subprocess.run([cli, "export", "candump", log_path, "-o", out_txt],
                       capture_output=True, text=True, timeout=10)
    if r.returncode != 0:
        fail(f"cli export rc={r.returncode}: {r.stderr}")
    with open(out_txt) as f:
        lines = f.read().splitlines()
    if len(lines) != 10:
        fail(f"export lines: {len(lines)}")
    # Timestamp = start_wall + ts_ms/1000, so record 0 (ts 1000 ms) ends .000000.
    if "can0" not in lines[0] or ".000000)" not in lines[0]:
        fail(f"export line 0 malformed: {lines[0]!r}")
    if "01ABCDEF" not in lines[0]:  # even index -> extended id, no pad
        fail(f"export line 0 missing id: {lines[0]!r}")
    if "00000123" not in lines[1]:  # odd index -> standard id, 8-digit pad
        fail(f"export line 1 missing id: {lines[1]!r}")

    # Device disconnect closes the log gracefully.
    dev.close()
    time.sleep(0.3)

    # Session replacement: reconnect while a large old-session backlog may
    # still be queued. The new .lsc must contain only the new session's
    # records and must keep logging (a stale close request used to kill it).
    dev2 = socket.create_connection(("127.0.0.1", port), timeout=5)
    dev2.settimeout(5)
    dev2.sendall(frame(1, b'{"proto":32,"fw":"2.0.0","device":"fake2"}'))
    for k in range(20):  # 16380 records: fills the writer ring
        dev2.sendall(batch_frame([(i, i, 0x321, 0, [i & 0xFF]) for i in range(819)]))
    dev2.close()

    dev3 = socket.create_connection(("127.0.0.1", port), timeout=5)
    dev3.settimeout(5)
    dev3.sendall(frame(1, b'{"proto":32,"fw":"2.0.0","device":"fake3"}'))
    time.sleep(0.2)
    fresh = [(i, i, 0x456, 0, [0xAA] * 8) for i in range(100)]
    dev3.sendall(batch_frame(fresh))
    time.sleep(0.5)

    st = json.loads(_ctl_roundtrip(ctl, b"stats\n").strip())
    if st.get("records") != len(fresh):
        fail(f"replacement session records: {st}")
    fresh_log = st.get("log")
    with open(fresh_log, "rb") as f:
        blob = f.read()
    if blob[:4] != b"LSC1":
        fail(f"replacement .lsc magic: {blob[:4]!r}")
    if len(blob) != 32 + len(fresh) * REC.size:
        fail(f"replacement .lsc size {len(blob)} != {32 + len(fresh) * REC.size}")
    dev3.close()
    time.sleep(0.3)


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


def _ctl_roundtrip(ctl_path, msg):
    """Send one control command, read the reply until EOF or an ACK arrives."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(ctl_path)
    s.sendall(msg)
    # pause/resume replies arrive when the device ACKs; poll briefly.
    data = b""
    deadline = time.time() + 3
    while time.time() < deadline:
        try:
            chunk = s.recv(4096)
        except socket.timeout:
            break
        if not chunk:
            break
        data += chunk
        if b"code" in data:
            break
    s.close()
    return data.decode(errors="replace")


def _tcp_roundtrip(port, msg):
    """Same line protocol as the Unix socket, over loopback TCP."""
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.settimeout(5)
    s.sendall(msg)
    data = b""
    deadline = time.time() + 3
    while time.time() < deadline:
        try:
            chunk = s.recv(4096)
        except socket.timeout:
            break
        if not chunk:
            break
        data += chunk
        if b"code" in data or data.endswith(b"\n"):
            break
    s.close()
    return data.decode(errors="replace")


def _http_get(port, path, timeout=5):
    """One-shot HTTP/1.1 GET; returns (status line, headers, body bytes)."""
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    s.settimeout(timeout)
    s.sendall(f"GET {path} HTTP/1.1\r\nHost: localhost\r\n\r\n".encode())
    data = b""
    while True:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            break
        if not chunk:
            break
        data += chunk
    s.close()
    head, sep, body = data.partition(b"\r\n\r\n")
    if not sep:
        fail(f"http {path}: no header terminator in {data[:200]!r}")
    return head.split(b"\r\n", 1)[0], head, body


def _sse_connect(port, timeout=5.0):
    """Open an EventSource-style stream; returns (socket, bytes after headers)."""
    deadline = time.time() + timeout
    while True:
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
            break
        except OSError:
            if time.time() > deadline:
                fail("sse: http listener never accepted")
            time.sleep(0.05)
    s.settimeout(timeout)
    s.sendall(b"GET /api/live HTTP/1.1\r\nHost: localhost\r\n"
              b"Accept: text/event-stream\r\n\r\n")
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = s.recv(1024)
        if not chunk:
            fail("sse: closed before headers")
        buf += chunk
    head, _, rest = buf.partition(b"\r\n\r\n")
    if b"200" not in head.split(b"\r\n", 1)[0] or b"text/event-stream" not in head:
        fail(f"sse headers: {head!r}")
    return s, rest


def _wait_sse_data(sock, buf, timeout=5.0):
    """Return (data payload, unread remainder), skipping comment frames."""
    deadline = time.time() + timeout
    while True:
        while b"\n\n" in buf:
            event, _, buf = buf.partition(b"\n\n")
            data = b"\n".join(line[5:].lstrip()
                              for line in event.split(b"\n")
                              if line.startswith(b"data:"))
            if data:
                return data, buf
        if time.time() > deadline:
            fail("sse: no data event before timeout")
        sock.settimeout(max(0.05, deadline - time.time()))
        try:
            chunk = sock.recv(65536)
        except socket.timeout:
            fail("sse: no data event before timeout")
        if not chunk:
            fail("sse: closed while waiting for data")
        buf += chunk


def _web_control(dev, port, cmd):
    """Drive one CONTROL through /api/control and ACK it on the device side."""
    with ThreadPoolExecutor(max_workers=1) as ex:
        fut = ex.submit(_http_get, port, "/api/control?cmd=" + cmd)
        ftype, payload = _recv_frame(dev)
        if ftype != 7 or cmd.encode() not in payload:
            fail(f"expected CONTROL {cmd}, got type={ftype} {payload!r}")
        _reply_ack(dev, payload)
        status, head, body = fut.result(timeout=5)
    if b"200" not in status:
        fail(f"control {cmd} status: {status!r}")
    if json.loads(body).get("ok") is not True:
        fail(f"control {cmd} rejected: {body!r}")


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


def _reply_ack(dev_sock, ctrl_payload):
    """Parse the CONTROL id out of the daemon's JSON and answer with an ACK frame."""
    ctrl = json.loads(ctrl_payload.decode(errors="replace"))
    # Compact JSON: ACK payload is capped at 16 bytes by the protocol.
    ack = json.dumps({"id": ctrl.get("id", 0), "code": 0},
                     separators=(",", ":")).encode()
    dev_sock.sendall(frame(8, ack))


if __name__ == "__main__":
    main()
