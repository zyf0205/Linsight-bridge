# CLAUDE.md

This file provides guidance to Claude Code when working in the WSL Linux host repository.

## Repository state

LinSight Linux host: `linsightd` (epoll daemon + `.lsc` writer), `linsight-cli`, and the
ctest suite. The ESP32-S3 firmware, Wi-Fi credentials, and flashing live in the separate
Windows checkout `D:\_study\projects\_MyProject\LinSight`; this repo does not build firmware.

Layout:
- `host/src/` — daemon, `.lsc` writer/reader, replay, compare, DBC, HTML report, device session, SocketCAN forward
- `host/cli/` — stats/live/config/pause/resume/clear/export/report/replay/compare/decode
- `ros2_ws/src/linsight_ros2/` — ROS 2 Humble bridge (vcan → `/can_rx`, `/linsight/stats`)
- `host/systemd/` — service unit + `/etc/default` args (CMake `install` targets)
- `host/tests/` — record/parser/fuzz/ring/canlog/compare/dbc/session/export/import/report/integration/rotation
- `protocol/` — shared v2.1 parser (mirror of the firmware repo)
- `firmware/can_ring.c/h` — read-only copy of the firmware ring for `test_ring`
- `tools/can-up.sh` / `can-down.sh` — one-shot usbipd + peak_usb + can0 bring-up/teardown
- `tools/can_soak.py` — AF_CAN 32-bit-counter generator + `.lsc` verifier
- `host/README.md` — build, CLI, service and bench reference; `docs/hardware.md` is the shared hardware doc

## Commands

```bash
cd ~/linsight/host
cmake -S . -B build && cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
cmake -S . -B build-asan -DLINSIGHT_SANITIZE=ON   # ASan/UBSan build

build/linsightd --port 8888 --ctl-port 8889 --log-dir ~/can-logs \
                --log-max-mb 128 [--vcan vcan0]
build/linsight-cli stats [--watch] [--interval MS]
build/linsight-cli live [--id 0x123] [--ext|--std] [--color]
build/linsight-cli config --bitrate 500000 --mode normal
build/linsight-cli pause|resume|clear
build/linsight-cli export asc|candump|csv FILE.lsc [-o OUT] [--id ID] [--ext|--std]
                                     [--from EPOCH] [--to EPOCH]
build/linsight-cli report FILE.lsc [-o report.html] [--title TEXT]
build/linsight-cli replay FILE.lsc [--iface can0] [--speed 1.0]
build/linsight-cli compare REF.lsc CAND.lsc
build/linsight-cli decode FILE.lsc --dbc MODEL.dbc [--id ID] [--limit N]

sudo cmake --install build        # binaries + unit + prefix etc/default
python3 ../tools/can_soak.py --iface can0 --rate 1000 --duration 600 --record /tmp/tx.csv
python3 ../tools/can_soak.py --verify FILE.lsc --expect 600000
build/linsight-cli report FILE.lsc -o report.html          # self-contained HTML
```

SocketCAN/ROS 2: `--vcan vcan0` forwards accepted records to SocketCAN; the
`ros2_ws/src/linsight_ros2` package publishes `/can_rx` and `/linsight/stats`.
Both `linsightd` and the node need `CAP_NET_RAW` (run with sudo in WSL).

`config` exits 0 on ACK OK, 2 on BAD_ARG, 3 on FAILED. `compare` exits 1 when
missing/extra/order are non-zero.

Bench (PCAN via usbipd; the Windows GUI lives in the firmware repo as `tools/can_gui.py`):
```bash
../tools/can-up.sh [BUSID] [BITRATE]   # attach + modprobe + bitrate + link up
../tools/can-up.sh --status            # Windows + WSL state, no changes
../tools/can-down.sh                   # can0 down, hand the adapter back
cangen can0 -n 500 -g 20 -I 0x123 -L 8 # 50 fps x 10 s; device total is ground truth
```

## Architecture

- `linsightd`: epoll main loop; TCP listener for the device (`INADDR_ANY`), Unix socket
  `/tmp/linsight.sock` for the CLI, optional loopback `--ctl-port`, `--vcan` SocketCAN
  forward, signalfd, 1 Hz timerfd. Writer thread: bounded 262144-record ring (~6 MiB)
  -> per-session `can-YYYYMMDD-HHMMSS-NNN.lsc`; close ordered before open. A 1 Hz
  `fdatasync` makes recent batches durable; failures use the same 2 s fresh-part retry.
  `--log-max-mb N` rotates a session into `-pNNN` parts before a record crosses the
  cap; all parts share the session anchor, so timestamps stay continuous.
- `--vcan IFACE`: after dedupe, records are written nonblocking as `struct can_frame`
  to SocketCAN; `CAN_EFF_FLAG`/`CAN_RTR_FLAG`/`CAN_ERR_FLAG` are mapped and drops are
  counted, never blocking the epoll loop.
- `linsight-cli`: control commands use the Unix socket; `export`/`report`/`replay`/`compare`
  read files directly and work without a running daemon. `live` is newline-framed candump
  text. `config` sends only explicit fields and waits for the device ACK. `report` emits a
  standalone HTML page (tiles, SVG FPS timeline, top-ID table, session metadata); `export
  csv` is the spreadsheet path for SD black-box captures.
- `.lsc` reader takes a record-count snapshot at open, so replaying the daemon's current
  log terminates instead of following the growing file. Truncated tail records are EOF.
- `replay` uses `AF_CAN`/`CAN_RAW` and `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)`;
  reports p99/max scheduling lateness. Replayed frames are captured again if the device
  is on the same bus — replay in a quiet bus or account for the extra records.
- `compare` matches FIFO per `(id, flags, dlc, data)` and reports missing/extra/order;
  it is offline, so there are no cross-device latency statistics.
- `decode` parses `BO_`/`SG_` (Intel/Motorola, signed, factor/offset); conditional
  `mN` signals are skipped, `--limit 0` prints all matches.
- `protocol/frame.c` is pure C (no IDF, no malloc); keep it allocation-free and fuzzable.
- Single device TCP client; the daemon is the only writer to `.lsc`.
- On reconnect the device drains its PSRAM ring backlog first (up to ~209k records).

## Shared files

`protocol/` and `docs/` are mirrors of the firmware repo; edit them there and copy here.
`firmware/can_ring.c/h` is a read-only copy for `test_ring`; sync it when the firmware ring changes.

## Git commits

Chinese, one-line subject plus only necessary bullets; keep `build/`, logs and captures out of git.

## Firmware

Build/flash in the Windows checkout only: `idf.py build`, `idf.py -p COMx flash`.
Bench Wi-Fi credentials are Kconfig defaults in the firmware repo (`main/Kconfig.projbuild`);
do not add secrets or host IPs to this repo.
