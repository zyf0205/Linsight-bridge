# LinSight host

Linux side of LinSight, a wireless CAN analyzer: `linsightd` (epoll daemon,
binary logging, HTTP/SSE dashboard) and `linsight-cli`. The ESP32-S3 firmware
lives in a separate checkout and speaks protocol v2.1 from `protocol/`.

```text
device ──Wi-Fi/TCP── linsightd ── .lsc writer thread
                       ├─ Unix socket / TCP :8889 → linsight-cli
                       └─ HTTP + SSE :8080     → browser dashboard
```

## Build and test

```bash
cd host
cmake -S . -B build && cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure        # 12 tests
cmake -S . -B build-asan -DLINSIGHT_SANITIZE=ON   # ASan/UBSan
```

Tests cover the shared parser (including fuzz), record layout, the firmware ring
drop-oldest policy, `.lsc` reader snapshots, HELLO anchoring/dedupe, candump/ASC
export shape, SD-style import/verify, `compare`, and two end-to-end daemon tests:
device→CLI/log/HTTP and size-based log rotation.

## Run

```bash
build/linsightd --port 8888 --ctl-port 8889 --log-dir ~/can-logs \
                --log-max-mb 128 --log-max-total-mb 4096 \
                --http-port 8080 --http-bind 127.0.0.1 --web-dir web
```

Open <http://127.0.0.1:8080/>. The page is loaded once at startup; restart the
daemon after editing `web/index.html`. `--http-port 0` disables the dashboard.
`--http-bind 0.0.0.0` exposes it on the LAN (no authentication — isolated bench
only). `--log-max-mb N` rotates each session into `-pNNN` parts; `0` keeps one
file per session. `--log-max-total-mb N` deletes the oldest `can-*.lsc` files at
startup and after a session closes (default `0` = keep all). A transient write
error closes the bad part and retries a fresh one after 2 s instead of dropping
the rest of the session.

## CLI

```bash
build/linsight-cli stats [--watch] [--interval MS]
build/linsight-cli live [--id 0x123] [--ext|--std] [--color]
build/linsight-cli config --bitrate 500000 --mode normal   # waits for device ACK
build/linsight-cli pause [--lease MS] | resume | clear
build/linsight-cli export candump FILE.lsc [-o OUT] [--id ID] [--ext|--std]
                                     [--from EPOCH] [--to EPOCH]
build/linsight-cli replay FILE.lsc [--iface can0] [--speed 1.0]
build/linsight-cli compare REF.lsc CAND.lsc
build/linsight-cli decode FILE.lsc --dbc MODEL.dbc [--id ID] [--limit N]
build/linsight-cli import rec00001.lsc -o can-fixed.lsc [--boot-wall EPOCH]
build/linsight-cli verify rec00001.lsc [--strict]
```

Global options: `--ctl PATH`, `--ctl-port PORT`, `--version`. The control socket
is resolved from `--ctl`, then `$LINSIGHT_CTL`, then `/run/linsight/ctl.sock` if
it exists, then `/tmp/linsight.sock`. Control commands use a 5 s receive timeout;
`config` exits 0 on ACK OK, 2 on `BAD_ARG`, 3 on `FAILED`, 4 when the daemon
reports an ACK timeout. `compare` exits 1 when missing/extra/order counts are
non-zero. `decode` supports Intel/Motorola signals with signedness, factor and
offset; conditional `mN` multiplexed signals are skipped and `--limit 0` prints
every matching frame.

`import` re-anchors a black-box log written by the device to wall time: pass
`--boot-wall` for an exact epoch, or the CLI uses `mtime - last_ts_ms` and
prints a warning. `verify` checks the header, record length, time order and
sequence gaps; `--strict` makes a truncated tail or any gap a failure.

## HTTP API

| Endpoint | Behaviour |
|---|---|
| `GET /api/stats` | daemon + latest device STATS as one JSON object |
| `GET /api/live` | SSE stream; each `data:` event is a JSON array of ≤64 records |
| `GET /api/control?cmd=...` | `pause`, `resume`, `clear`, `config bitrate=500000`, ... |

Control is asynchronous: the daemon forwards CONTROL and returns immediately;
the dashboard reflects the result in the next STATS. Device/CLI replies are
queued in 4 KiB per-connection buffers drained by EPOLLOUT, so a slow peer can
never block the epoll loop; a missing device ACK closes the CLI wait after 3 s.
SSE sends are nonblocking: a slow browser drops event batches instead of
stalling CAN receive.

## Install as a service

```bash
sudo cmake --install build            # /usr/local by default
sudo systemctl daemon-reload
sudo systemctl enable --now linsightd
journalctl -u linsightd -f
```

Installs the binaries, `share/linsight/web/index.html`, the systemd unit and
`/etc/default/linsightd`. The unit uses `DynamicUser`, `StateDirectory` and
`ProtectSystem=strict`; `.lsc` files live in `/var/lib/linsight` and rotate via
`--log-max-mb` (no external logrotate needed for binary logs). With
`DynamicUser`, the control socket defaults to `/run/linsight/ctl.sock`; the CLI
checks that path automatically, or pass `--ctl`/`--ctl-port`.

## Bench

PCAN-USB passed through to WSL (`usbipd attach --wsl --busid 2-1`):

```bash
sudo modprobe peak_usb
sudo ip link set can0 type can bitrate 500000 && sudo ip link set can0 up
cangen can0 -n 500 -g 20 -I 0x123 -L 8
build/linsight-cli stats --watch
```

For counter-checked soaks use `../tools/can_soak.py` (AF_CAN generator, 32-bit
payload counter) and compare the captured `.lsc` against the send sequence.
`../tools/soak24h.py` wraps a private daemon, the generator, one-minute health
samples and per-part verification into one long run:

```bash
python3 ../tools/soak24h.py --duration 86400 --rate 800
# report: ~/can-logs/soak-latest/summary.txt; stop via soak-latest/wrapper.pid
```
