# CLAUDE.md

This file provides guidance to Claude Code when working in the WSL Linux host repository.

## Repository state

LinSight Linux host: `linsightd`, a thin epoll bridge (TCP device session ->
dedupe -> paced SocketCAN forward / candump `--dump`) plus `tools/lsc.py` for
SD black-box `.lsc` files. The ESP32-S3 firmware, Wi-Fi credentials, and
flashing live in the separate Windows checkout `D:\_study\projects\_MyProject\LinSight`;
this repo does not build firmware.

Layout:
- `host/src/` — bridge daemon (main.c, session dedupe, pace scheduler) + shared parser
- `host/tests/` — record/parser/fuzz/ring/session/pace/integration/tools/lsc
- `ros2_ws/src/linsight_ros2/` — ROS 2 Humble bridge (vcan → `/can_rx`, `/linsight/stats`)
- `host/systemd/` — service unit + `/etc/default` args (CMake `install` targets)
- `protocol/` — shared v2.1 parser (mirror of the firmware repo)
- `firmware/can_ring.c/h` — read-only copy of the firmware ring for `test_ring`
- `tools/can-up.sh` / `can-down.sh` — one-shot usbipd + peak_usb + can0 bring-up/teardown
- `tools/lsc.py` / `tools/lscfile.py` — SD black-box `.lsc` export/verify/import
- `tools/can_soak.py` — AF_CAN 32-bit-counter generator + `.lsc` verifier
- `host/README.md` — build, usage, service and bench reference; `docs/hardware.md` is the shared hardware doc

## Commands

```bash
cd ~/linsight/host
cmake -S . -B build && cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure            # 9 tests
cmake -S . -B build-asan -DLINSIGHT_SANITIZE=ON   # ASan/UBSan build

sudo ./build/linsightd --port 8888 --vcan vcan0 [--dump cap.log|-] \
                        [--pace SPEED] [--pace-replay]

python3 ../tools/lsc.py export asc|candump|csv FILE.lsc [-o OUT] [--id ID] [--ext|--std]
python3 ../tools/lsc.py verify FILE.lsc [--strict]
python3 ../tools/lsc.py import SRC.lsc -o DST.lsc [--boot-wall EPOCH]
python3 ../tools/can_soak.py --iface can0 --rate 1000 --duration 600 --record /tmp/tx.csv
python3 ../tools/can_soak.py --verify FILE.lsc --expect 600000

sudo cmake --install build        # linsightd + unit + prefix etc/default
```

`lsc.py` exits 0 on success, 1 when `verify --strict` fails, 2 on usage/IO
errors. Interactive analysis belongs to ecosystem tools (SavvyCAN, Wireshark,
PlotJuggler) fed via `--vcan`, `--dump`, or `lsc.py export` — do not re-grow
an in-house decoder/GUI.

SocketCAN/ROS 2: `--vcan vcan0` forwards accepted records to SocketCAN; the
`ros2_ws/src/linsight_ros2` package publishes `/can_rx` and `/linsight/stats`.
Both `linsightd` and the node need `CAP_NET_RAW` (run with sudo in WSL).

Bench (PCAN via usbipd; the Windows GUI lives in the firmware repo as `tools/can_gui.py`):
```bash
../tools/can-up.sh [BUSID] [BITRATE]   # attach + modprobe + bitrate + link up
../tools/can-up.sh --status            # Windows + WSL state, no changes
../tools/can-down.sh                   # can0 down, hand the adapter back
cangen can0 -n 500 -g 20 -I 0x123 -L 8 # 50 fps x 10 s; device total is ground truth
```

## Architecture

- `linsightd` (single-threaded epoll): TCP listener for the device
  (`INADDR_ANY`), signalfd, 1 Hz timerfd (idle watchdog + stats line +
  `--dump` flush). One device session at a time; no writer thread, no
  recording — the SD black box or tcpdump/SavvyCAN own capture.
- Dedupe: `session_record_new` skips a reconnect backlog by `ts_ms`/`seq`
  marks; marks survive a reconnect with the same boot count and clear on a
  boot change. HELLO `uptime_ms` anchors device time once per boot; old
  firmware falls back to the first record's timestamp.
- `--vcan IFACE`: accepted records are paced onto SocketCAN as
  `struct can_frame` (`CAN_EFF_FLAG`/`CAN_RTR_FLAG`/`CAN_ERR_FLAG` mapped)
  at their device-timestamped wall times via `pace.c`: a 262144-entry
  drop-oldest queue with absolute `CLOCK_MONOTONIC` targets. Late backlog
  pops immediately (compression, never catch-up sleep). `--pace 0` = burst,
  `--pace-replay` = 1:1 historical intervals. Per-iteration budget keeps
  PONG latency bounded; drops are counted, never blocking the epoll loop.
- `--dump PATH|-`: live candump text (same format as `lsc.py export
  candump`), flushed per batch; useful without root and for tests.
- `tools/lsc.py`: the only `.lsc` writer/reader in the repo — export
  (asc/candump/csv, byte-locked to the old C formats), verify (seq rules,
  truncated tails, `--strict`), import (wall re-anchoring). Analysis belongs
  to ecosystem tools (SavvyCAN with DBC files, PlotJuggler, Wireshark). Do
  not re-grow an in-house decoder/GUI or CLI.
- `protocol/frame.c` is pure C (no IDF, no malloc); keep it allocation-free and fuzzable.
- Single device TCP client; the device pings every 5 s and a silent session
  is closed after 30 s. On reconnect the device drains its PSRAM ring
  backlog first (up to ~209k records) — the dedupe marks absorb it.

## Shared files

`protocol/` and `docs/` are mirrors of the firmware repo; edit them there and copy here.
`firmware/can_ring.c/h` is a read-only copy for `test_ring`; sync it when the firmware ring changes.

## Git commits

Chinese, one-line subject plus only necessary bullets; keep `build/`, logs and captures out of git.

## Firmware

Build/flash in the Windows checkout only: `idf.py build`, `idf.py -p COMx flash`.
Bench Wi-Fi credentials are Kconfig defaults in the firmware repo (`main/Kconfig.projbuild`);
do not add secrets or host IPs to this repo.
