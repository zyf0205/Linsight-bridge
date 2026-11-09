#!/usr/bin/env python3
"""Self-contained long soak: daemon + counter-checked CAN generator + report.

The wrapper starts a private linsightd, waits for the device, runs can_soak.py,
samples daemon/device health every minute, stops the daemon, then verifies every
.lsc part's 32-bit payload counters with a running offset across rotations and
reconnects.

Example (24 h, ~850 fps, about 1.5 GB of logs):
    nohup python3 tools/soak24h.py --duration 86400 --rate 850 \\
        > ~/can-logs/soak-console.log 2>&1 &
    tail -f ~/can-logs/soak-latest/summary.txt

Stop early with Ctrl-C or SIGTERM; the wrapper forwards SIGINT to the generator,
closes the daemon and still writes a report.
"""

import argparse
import csv
import datetime
import glob
import json
import os
import re
import signal
import subprocess
import sys
import time
import urllib.request

from can_soak import verify_file

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_DAEMON = os.path.join(ROOT, "host", "build", "linsightd")
DEFAULT_WEB = os.path.join(ROOT, "host", "web")
CAN_SOAK = os.path.join(ROOT, "tools", "can_soak.py")
STOP = False


def on_signal(_sig, _frame):
    global STOP
    STOP = True


def iso(epoch):
    return datetime.datetime.fromtimestamp(epoch).isoformat(timespec="seconds")


def http_stats(url, timeout=3.0):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return json.load(r)


def wait_device(url, daemon, timeout=60.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if daemon.poll() is not None:
            return None
        try:
            s = http_stats(url)
            if s.get("device") and s.get("stats", {}).get("uptime_ms", 0) > 0:
                return s
        except Exception:
            pass
        time.sleep(0.5)
    return None


def wait_drain(url, daemon, timeout=60.0):
    last = None
    stable = 0
    deadline = time.time() + timeout
    while time.time() < deadline and daemon.poll() is None:
        try:
            s = http_stats(url)
        except Exception:
            time.sleep(1.0)
            continue
        key = (s.get("records"), s.get("written"), s.get("stats", {}).get("ring_used"))
        if key == last:
            stable += 1
        else:
            stable = 0
            last = key
        if stable >= 3:
            return True
        time.sleep(1.0)
    return False


def take_sample(writer, url, daemon, stats_prev, counters):
    try:
        s = http_stats(url)
    except Exception as exc:
        writer.writerow([time.time(), iso(time.time()), "down", "", "", "", "",
                         "", "", "", "", "", "", "", "", "", "", "", "",
                         _rss(daemon.pid), _fds(daemon.pid)])
        counters["down"] += 1
        return stats_prev

    st = s.get("stats") or {}
    uptime = int(st.get("uptime_ms", 0))
    if stats_prev is not None and uptime and uptime < stats_prev:
        counters["resets"] += 1
    if not s.get("device"):
        counters["disconnects"] += 1
    writer.writerow([
        time.time(), iso(time.time()), int(bool(s.get("device"))),
        s.get("records", 0), s.get("written", 0), s.get("dropped", 0),
        uptime, st.get("fps", 0), st.get("load_permille", 0), st.get("state", ""),
        st.get("bitrate", 0), st.get("mode", ""), st.get("ring_used", 0),
        st.get("missed", 0), st.get("rxq_full", 0), st.get("bus_err", 0),
        st.get("bus_off", 0), st.get("tec", 0), st.get("rec", 0),
        _rss(daemon.pid), _fds(daemon.pid)])
    return uptime or stats_prev


def _rss(pid):
    try:
        with open(f"/proc/{pid}/status", "r", encoding="ascii") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except OSError:
        pass
    return ""


def _fds(pid):
    try:
        return len(os.listdir(f"/proc/{pid}/fd"))
    except OSError:
        return ""


def log_part_key(path):
    """Order numeric session/part suffixes, including files with equal mtimes."""
    match = re.fullmatch(r"can-(\d{8}-\d{6})-(\d+)(?:-p(\d+))?\.lsc",
                         os.path.basename(path))
    if match:
        return (0, match[1], int(match[2]), int(match[3] or 0))
    return (1, os.path.getmtime(path), path)


def generator_sent(path):
    """Read the generator's final structured result; missing evidence fails."""
    result = None
    try:
        with open(path, encoding="utf-8") as f:
            for line in f:
                if line.startswith("result: "):
                    result = json.loads(line[len("result: "):])
    except (OSError, ValueError):
        return None
    if (not isinstance(result, dict) or type(result.get("sent")) is not int or
            result["sent"] < 0):
        return None
    return result["sent"]


def verify_parts(log_dir, can_id, log_path, expected_sent=None):
    files = sorted(glob.glob(os.path.join(log_dir, "*.lsc")), key=log_part_key)
    total = 0
    matched = 0
    ok = True
    with open(log_path, "w", encoding="utf-8") as out:
        for path in files:
            try:
                result = verify_file(path, int(can_id, 0), first=matched,
                                     allow_empty=True)
            except (OSError, ValueError) as exc:
                out.write(f"{os.path.basename(path)}: FAIL: {exc}\n")
                ok = False
                break
            total += result["records"]
            matched += result["matched"]
            out.write(f"{os.path.basename(path)}: records={result['records']} "
                      f"matched={result['matched']} next={result['next']}\n")
        if expected_sent is None or matched != expected_sent or matched == 0:
            out.write(f"FAIL: captured={matched} generator_sent={expected_sent} "
                      f"total_records={total}\n")
            ok = False
    return ok, matched, len(files)


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--duration", type=float, default=86400.0)
    p.add_argument("--rate", type=float, default=850.0)
    p.add_argument("--iface", default="can0")
    p.add_argument("--id", default="0x2A0")
    p.add_argument("--daemon", default=DEFAULT_DAEMON)
    p.add_argument("--web-dir", default=DEFAULT_WEB)
    p.add_argument("--port", type=int, default=8888)
    p.add_argument("--http-port", type=int, default=8080)
    p.add_argument("--log-max-mb", type=int, default=128)
    p.add_argument("--sample", type=float, default=60.0)
    p.add_argument("--report", help="report directory (default ~/can-logs/soak-<ts>)")
    args = p.parse_args()

    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)

    report = args.report or os.path.join(
        os.path.expanduser("~"), "can-logs",
        "soak-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
    os.makedirs(report, exist_ok=True)
    log_dir = os.path.join(report, "lsc")
    os.makedirs(log_dir, exist_ok=True)
    latest = os.path.join(os.path.dirname(report), "soak-latest")
    try:
        if os.path.islink(latest) or os.path.exists(latest):
            os.unlink(latest)
        os.symlink(report, latest)
    except OSError:
        pass

    with open(os.path.join(report, "wrapper.pid"), "w", encoding="ascii") as f:
        f.write(f"{os.getpid()}\n")

    api = f"http://127.0.0.1:{args.http_port}/api/stats"
    ctl = os.path.join(report, "ctl.sock")
    daemon_cmd = [args.daemon, "--port", str(args.port), "--ctl", ctl,
                  "--log-dir", log_dir, "--log-max-mb", str(args.log_max_mb),
                  "--http-port", str(args.http_port),
                  "--http-bind", "127.0.0.1", "--web-dir", args.web_dir]
    daemon_log = open(os.path.join(report, "daemon.log"), "w", encoding="utf-8")
    daemon = subprocess.Popen(daemon_cmd, stdout=daemon_log, stderr=subprocess.STDOUT,
                              text=True)
    with open(os.path.join(report, "daemon.pid"), "w", encoding="ascii") as f:
        f.write(f"{daemon.pid}\n")

    gen = None
    gen_log = None
    generator_log_path = os.path.join(report, "generator.log")
    status = "failed"
    verify_ok = False
    total_records = 0
    parts = 0
    counters = {"down": 0, "disconnects": 0, "resets": 0}
    start = time.time()
    start_iso = iso(start)

    try:
        s = wait_device(api, daemon)
        if not s:
            raise RuntimeError("device did not connect (daemon log: %s)" %
                               os.path.join(report, "daemon.log"))
        st = s.get("stats") or {}
        print(f"device connected: bitrate={st.get('bitrate')} mode={st.get('mode')}",
              flush=True)

        gen_cmd = [sys.executable, CAN_SOAK, "--iface", args.iface,
                   "--rate", str(args.rate), "--duration", str(args.duration),
                   "--id", args.id]
        gen_log = open(generator_log_path, "w", encoding="utf-8")
        gen = subprocess.Popen(gen_cmd, stdout=gen_log, stderr=subprocess.STDOUT,
                               text=True)
        with open(os.path.join(report, "generator.pid"), "w", encoding="ascii") as f:
            f.write(f"{gen.pid}\n")

        with open(os.path.join(report, "samples.csv"), "w", newline="",
                  encoding="utf-8") as samples:
            writer = csv.writer(samples)
            writer.writerow(["epoch", "iso", "device", "records", "written",
                             "writer_dropped", "uptime_ms", "fps", "load_permille",
                             "state", "bitrate", "mode", "ring_used", "rx_missed",
                             "rxq_full", "bus_err", "bus_off", "tec", "rec",
                             "rss_kb", "fds"])
            prev = None
            last_sample = 0.0
            while gen.poll() is None and not STOP:
                time.sleep(0.5)
                if time.time() - last_sample >= args.sample:
                    prev = take_sample(writer, api, daemon, prev, counters)
                    last_sample = time.time()
                    samples.flush()
            if STOP:
                gen.send_signal(signal.SIGINT)
            try:
                gen.wait(timeout=30)
            except subprocess.TimeoutExpired:
                gen.kill()
                gen.wait()
            prev = take_sample(writer, api, daemon, prev, counters)
        status = "stopped" if STOP else ("done" if gen.returncode == 0 else "generator-error")
        print(f"generator exit={gen.returncode}; draining writer", flush=True)
        wait_drain(api, daemon)
    except Exception as exc:
        print(f"soak error: {exc}", file=sys.stderr, flush=True)
    finally:
        if gen and gen.poll() is None:
            gen.kill()
            gen.wait()
        if daemon.poll() is None:
            daemon.terminate()
            try:
                daemon.wait(timeout=10)
            except subprocess.TimeoutExpired:
                daemon.kill()
                daemon.wait()
        if gen_log:
            gen_log.close()
        daemon_log.close()

    sent = generator_sent(generator_log_path) if gen else None
    verify_log = os.path.join(report, "verify.log")
    verify_ok, total_records, parts = verify_parts(log_dir, args.id, verify_log, sent)
    end = time.time()
    summary = [
        "LinSight soak report",
        f"report: {report}",
        f"start: {start_iso}",
        f"end: {iso(end)}",
        f"elapsed_s: {end - start:.0f}",
        f"target_rate_fps: {args.rate}",
        f"generator_exit: {gen.returncode if gen else 'not-started'}",
        f"generator_sent: {sent if sent is not None else 'unavailable'}",
        f"status: {status}",
        f"device_down_samples: {counters['down']}",
        f"device_disconnect_samples: {counters['disconnects']}",
        f"uptime_regressions: {counters['resets']}",
        f"log_parts: {parts}",
        f"records: {total_records}",
        f"verify: {'PASS' if verify_ok else 'FAIL'}",
        f"verify_log: {verify_log}",
        f"samples: {os.path.join(report, 'samples.csv')}",
    ]
    text = "\n".join(summary) + "\n"
    with open(os.path.join(report, "summary.txt"), "w", encoding="utf-8") as f:
        f.write(text)
    print(text, flush=True)
    return 0 if verify_ok and status == "done" else 1


if __name__ == "__main__":
    sys.exit(main())
