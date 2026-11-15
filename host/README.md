# LinSight host

Linux side of LinSight, a wireless CAN analyzer: `linsightd` (epoll daemon,
binary logging, optional SocketCAN forward) and `linsight-cli`. The ESP32-S3
firmware lives in a separate checkout and speaks protocol v2.1 from `protocol/`.

```text
device ──Wi-Fi/TCP── linsightd ── .lsc writer thread
                       ├─ Unix socket / TCP :8889 → linsight-cli
                       └─ --vcan IFACE          → vcan0 → ROS 2 / local consumers
```

## Build and test

```bash
cd host
cmake -S . -B build && cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure        # 14 tests
cmake -S . -B build-asan -DLINSIGHT_SANITIZE=ON   # ASan/UBSan
```

Tests cover the shared parser (including fuzz), record layout, the firmware ring
drop-oldest policy, `.lsc` reader snapshots, HELLO anchoring/dedupe, candump/ASC/CSV
export shape, HTML reports, SD-style import/verify, `compare`, and two
end-to-end daemon tests:
device→CLI/log and size-based log rotation.

## Run

```bash
build/linsightd --port 8888 --ctl-port 8889 --log-dir ~/can-logs \
                --log-max-mb 128 --log-max-total-mb 4096
```

`--log-max-mb N` rotates each session into `-pNNN` parts; `0` keeps one file per
session. `--log-max-total-mb N` deletes the oldest `can-*.lsc` files at startup,
after rotation/session close, and at most once per second during writes (default
`0` = keep all); the current part is protected. A transient write, flush or 1 Hz
`fdatasync` error closes the bad part and retries a fresh one after 2 s; initial
open failures also retry within the same session.

Add `--vcan vcan0` to mirror accepted (already de-duplicated) records onto
SocketCAN for ROS 2 or `candump`. The write is nonblocking; queue pressure
increments `vcan_dropped` in `linsight-cli stats` instead of stalling CAN
receive. A raw CAN socket needs `CAP_NET_RAW`.

## CLI

```bash
build/linsight-cli stats [--watch] [--interval MS]
build/linsight-cli live [--id 0x123] [--ext|--std] [--color]
build/linsight-cli config --bitrate 500000 --mode normal   # waits for device ACK
build/linsight-cli pause [--lease MS] | resume | clear
build/linsight-cli export asc|candump|csv FILE.lsc [-o OUT] [--id ID] [--ext|--std]
                                     [--from EPOCH] [--to EPOCH]
build/linsight-cli report FILE.lsc [-o report.html] [--title TEXT]
build/linsight-cli replay FILE.lsc [--iface can0] [--speed 1.0]
build/linsight-cli compare REF.lsc CAND.lsc
build/linsight-cli import rec00001.lsc -o can-fixed.lsc [--boot-wall EPOCH]
build/linsight-cli verify rec00001.lsc [--strict]
```

Global options: `--ctl PATH`, `--ctl-port PORT`, `--version`. The control socket
is resolved from `--ctl`, then `$LINSIGHT_CTL`, then `/run/linsight/ctl.sock` if
it exists, then `/tmp/linsight.sock`. Request/reply commands use a 5 s receive
timeout; `live` subscriptions stay open while idle.
`config` exits 0 on ACK OK, 2 on `BAD_ARG`, 3 on `FAILED`, 4 when the daemon
reports an ACK timeout. `compare` exits 1 when missing/extra/order counts are
non-zero.

`import` re-anchors a black-box log written by the device to wall time: pass
`--boot-wall` for an exact epoch, or the CLI uses `mtime - last_ts_ms` and
prints a warning. `verify` checks the header, record length, time order and
sequence gaps; `--strict` makes a truncated tail or any gap a failure.

`report` turns a `.lsc` into one self-contained HTML page: frame/duration/ID
tiles, an SVG frames-per-second timeline, the top 12 IDs with share bars, and
session metadata. It needs no daemon or browser assets, so an SD `recNNNNN.lsc`
can be copied off the card and reported directly; run `import` first if you
need wall-clock stamps instead of uptime, and `-o -` writes to stdout.
`export csv` emits `wall_ms,uptime_ms,seq,id,ext,rtr,dlc,data_hex` for Excel
or pandas. A `README.TXT` on the SD card points at these commands.

## SocketCAN / ROS 2

`--vcan IFACE` opens `PF_CAN/CAN_RAW` and writes each accepted record as a
`struct can_frame`; `LINSIGHT_REC_FLAG_EXT`, `_RTR` and `_ERR` map to
`CAN_EFF_FLAG`, `CAN_RTR_FLAG` and `CAN_ERR_FLAG`. The epoll loop never blocks
on a full interface queue.

The minimal ROS 2 Humble package in `../ros2_ws/src/linsight_ros2` reads that
interface and publishes `/can_rx` (`linsight_ros2/msg/CanFrame`) and a 1 Hz
`/linsight/stats` (`BusStats`). See its `README_ROS2.md` for vcan setup,
`colcon build`, permissions and verification.

## Ecosystem tools (analysis front-ends)

LinSight is the recorder and forensics toolbox; interactive decoding and
plotting are handed to established tools via SocketCAN or file exports. GUI
tools run under WSLg:

| Need | Tool | Data path |
|---|---|---|
| Live trace, DBC decode, signal plots | [SavvyCAN](https://www.savvycan.com/) | SocketCAN on the `--vcan` interface, or File → Import of `export asc/candump/csv` |
| Quick terminal glance | can-utils | `candump vcan0` (or `linsight-cli live`) |
| Frame-level forensics | Wireshark | `tcpdump -i vcan0 -w cap.pcapng`, open the capture on Windows |
| Multi-signal time plots | PlotJuggler | ROS 2 topic `/can_rx` (`ros2 run plotjuggler plotjuggler`) |

```bash
sudo ip link add vcan0 type vcan && sudo ip link set vcan0 up
build/linsightd --port 8888 --log-dir ~/can-logs --vcan vcan0  # CAP_NET_RAW
build/linsight-cli export asc can-*.lsc -o capture.asc         # offline import
```

## Install as a service

```bash
sudo cmake --install build            # /usr/local by default
sudo systemctl daemon-reload
sudo systemctl enable --now linsightd
journalctl -u linsightd -f
```

Installs the binaries, the systemd unit and `/etc/default/linsightd`. The
unit uses `DynamicUser`, `StateDirectory`, `ProtectSystem=strict` and
`CAP_NET_RAW` (for optional `--vcan`); `.lsc` files live in `/var/lib/linsight` and rotate via
`--log-max-mb` (no external logrotate needed for binary logs). With
`DynamicUser`, the control socket defaults to `/run/linsight/ctl.sock`; the CLI
checks that path automatically, or pass `--ctl`/`--ctl-port`.

## Bench

### One-shot USB-CAN bring-up

`../tools/can-up.sh` replaces the manual sequence (usbipd attach → modprobe →
bitrate → link up) with one command:

```bash
../tools/can-up.sh                  # busid 2-1, 500 kbit/s
../tools/can-up.sh 2-1 250000       # explicit busid / bitrate
../tools/can-up.sh --status         # show Windows + WSL state only
../tools/can-down.sh                # can0 down + hand the adapter back
```

The first `usbipd bind` must be run once in an **elevated** Windows terminal;
after that `attach` works per-user. `modprobe`/`ip` prompt for sudo once.

### Manual equivalent

```bash
sudo modprobe peak_usb
sudo ip link set can0 type can bitrate 500000 && sudo ip link set can0 up
cangen can0 -n 500 -g 20 -I 0x123 -L 8
build/linsight-cli stats --watch
```

For counter-checked soaks use `../tools/can_soak.py` (AF_CAN generator, 32-bit
payload counter) and compare the captured `.lsc` against the send sequence.
With a daemon already running, start the generator with `--record` for a short
run, or wrap the same command in a shell loop for longer soaks.
