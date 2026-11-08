# CLAUDE.md

This file provides guidance to Claude Code when working in the WSL Linux host repository.

## Repository state

LinSight Linux host: `linsightd` (epoll daemon + `.lsc` writer), `linsight-cli`, and the
ctest suite. The ESP32-S3 firmware, Wi-Fi credentials, and flashing live in the separate
Windows checkout `D:\_study\projects\_MyProject\LinSight`; this repo does not build firmware.

Layout:
- `host/src/` — daemon, `.lsc` writer/reader, replay, compare, DBC, device session, HTTP/SSE, traffic simulator
- `host/cli/` — stats/live/config/pause/resume/clear/export/replay/compare/decode/sim
- `host/web/index.html` — single-file dashboard (served by `linsightd`, no build step)
- `host/systemd/` — service unit + `/etc/default` args (CMake `install` targets)
- `host/tests/` — record/parser/fuzz/ring/canlog/compare/dbc/session/export/import/sim/integration/rotation
- `protocol/` — shared v2.1 parser (mirror of the firmware repo)
- `firmware/can_ring.c/h` — read-only copy of the firmware ring for `test_ring`
- `tools/can-up.sh` / `can-down.sh` — one-shot usbipd + peak_usb + can0 bring-up/teardown
- `tools/bench_can.py` — optional python-can bench tool; `cangen` is the primary bench
- `tools/can_soak.py` — AF_CAN 32-bit-counter generator + `.lsc` verifier
- `tools/soak24h.py` — self-contained daemon + generator + health samples + report
- `docs/` — project docs mirrored from the firmware repo

## Commands

```bash
cd ~/linsight/host
cmake -S . -B build && cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
cmake -S . -B build-asan -DLINSIGHT_SANITIZE=ON   # ASan/UBSan build

build/linsightd --port 8888 --ctl-port 8889 --log-dir ~/can-logs \
                --log-max-mb 128 --http-port 8080 --http-bind 127.0.0.1 --web-dir web
build/linsight-cli stats [--watch] [--interval MS]
build/linsight-cli live [--id 0x123] [--ext|--std] [--color]
build/linsight-cli config --bitrate 500000 --mode normal
build/linsight-cli pause|resume|clear
build/linsight-cli export candump FILE.lsc [-o OUT] [--id ID] [--ext|--std]
                                     [--from EPOCH] [--to EPOCH]
build/linsight-cli replay FILE.lsc [--iface can0] [--speed 1.0]
build/linsight-cli compare REF.lsc CAND.lsc
build/linsight-cli decode FILE.lsc --dbc MODEL.dbc [--id ID] [--limit N]

sudo cmake --install build        # binaries + web + unit + prefix etc/default
python3 ../tools/can_soak.py --iface can0 --rate 1000 --duration 600 --record /tmp/tx.csv
python3 ../tools/can_soak.py --verify FILE.lsc --expect 600000
python3 ../tools/soak24h.py --duration 86400 --rate 800   # report in ~/can-logs/soak-latest
```

Dashboard: `http://127.0.0.1:8080/` (Windows browser via WSL localhost forwarding);
`--http-port 0` disables it, `--http-bind 0.0.0.0` exposes it on the LAN.
The page is loaded at startup, so restart the daemon after editing `web/index.html`.
Endpoints: `GET /api/stats` (JSON snapshot), `GET /api/live` (SSE batches),
`GET /api/control?cmd=pause|resume|clear|config%20bitrate%3D500000` (async CONTROL).

`config` exits 0 on ACK OK, 2 on BAD_ARG, 3 on FAILED. `compare` exits 1 when
missing/extra/order are non-zero.

Bench (PCAN via usbipd; the Windows GUI is legacy/removed):
```bash
../tools/can-up.sh [BUSID] [BITRATE]   # attach + modprobe + bitrate + link up
../tools/can-up.sh --status            # Windows + WSL state, no changes
../tools/can-down.sh                   # can0 down, hand the adapter back
cangen can0 -n 500 -g 20 -I 0x123 -L 8 # 50 fps x 10 s; device total is ground truth
```

Simulator (no hardware or device needed; host-side only):
```bash
build/linsight-cli sim start engine|bms|mix|counter [1..1000]
build/linsight-cli sim stop | sim status | sim inject 1AB DE AD BE EF
```

## Architecture

- `linsightd`: epoll main loop; TCP listener for the device (`INADDR_ANY`), Unix socket
  `/tmp/linsight.sock` for the CLI, optional loopback `--ctl-port`, optional HTTP
  dashboard, signalfd, 1 Hz timerfd. Writer thread: bounded 16384-record ring ->
  per-session `can-YYYYMMDD-HHMMSS-NNN.lsc`; close ordered before open.
  `--log-max-mb N` rotates a session into `-pNNN` parts before a record crosses the
  cap; all parts share the session anchor, so timestamps stay continuous.
- HTTP dashboard (`src/http.c`): same epoll loop, `HTTP_MAX_CLIENTS` fixed slots.
  `/api/live` is Server-Sent Events; each event is a JSON array of ≤64 records.
  Sends are nonblocking — a slow browser drops events instead of stalling CAN RX.
  `/api/control` only whitelists pause/resume/clear/config/sim and forwards CONTROL
  (except `sim`, which is handled in-process); it does not wait for the device ACK.
- Traffic simulator (`src/sim.c`): 10 Hz timerfd in the same epoll loop emits
  pattern frames (`engine`/`bms`/`mix`/`counter`, 1–1000 fps, fractional carry so
  slow rates stay even) straight into the real ingest path — writer ring, CLI
  `live`, SSE — so simulated traffic is indistinguishable downstream. Records
  carry a monotone `seq` like the device, otherwise `verify` reports duplicates.
  It opens its own `.lsc` session when no device is connected and defers to a
  real device session when one appears. Host-side only: never touches the bus.
- `linsight-cli`: control commands use the Unix socket; `export`/`replay`/`compare` read
  files directly and work without a running daemon. `live` is newline-framed candump text.
  `config` sends only explicit fields and waits for the device ACK.
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
Bench Wi-Fi credentials are committed in the firmware repo (`BENCH_*` in `main/net.c`);
do not add secrets or host IPs to this repo.
