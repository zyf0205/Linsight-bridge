# CLAUDE.md

This file provides guidance to Claude Code when working in the WSL Linux host repository.

## Repository state

LinSight Linux host: `linsightd` (epoll daemon + `.lsc` writer), `linsight-cli`, and the
ctest suite. The ESP32-S3 firmware, Wi-Fi credentials, and flashing live in the separate
Windows checkout `D:\_study\projects\_MyProject\LinSight`; this repo does not build firmware.

Layout:
- `host/src/` — daemon, `.lsc` writer, device session
- `host/cli/` — stats/live/pause/resume/clear/export
- `host/tests/` — record/parser/fuzz/ring/integration
- `protocol/` — shared v2.0 parser (mirror of the firmware repo)
- `firmware/can_ring.c/h` — read-only copy of the firmware ring for `test_ring`
- `tools/bench_can.py` — optional python-can bench tool; `cangen` is the primary bench
- `docs/` — project docs mirrored from the firmware repo

## Commands

```bash
cd ~/linsight/host
cmake -S . -B build && cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
build/linsightd --port 8888 --ctl-port 8889 --log-dir ~/can-logs
build/linsight-cli stats|live|pause|resume|clear|export asc|candump FILE.lsc
```

Bench (PCAN via usbipd; the Windows GUI is legacy/removed):
```bash
# one-time admin bind: usbipd bind --busid 2-1
# after each replug/WSL restart: usbipd attach --wsl --busid 2-1
sudo modprobe peak_usb
sudo ip link set can0 type can bitrate 500000 && sudo ip link set can0 up
cangen can0 -n 500 -g 20 -I 0x123 -L 8   # 50 fps x 10 s; device total is ground truth
```

## Architecture

- `linsightd`: epoll main loop; TCP listener for the device (`INADDR_ANY`), Unix socket
  `/tmp/linsight.sock` for the CLI, optional loopback `--ctl-port`, signalfd, 1 Hz timerfd.
  Writer thread: bounded 16384-record ring -> per-session `can-YYYYMMDD-HHMMSS-NNN.lsc`;
  close ordered before open.
- `linsight-cli`: stats/live/pause/resume/clear/export; `live` is newline-framed candump text.
- `protocol/frame.c` is pure C (no IDF, no malloc); keep it allocation-free and fuzzable.
- Single device TCP client; the daemon is the only writer to `.lsc`.
- On reconnect the device drains its PSRAM ring backlog first (up to ~52k records).

## Shared files

`protocol/` and `docs/` are mirrors of the firmware repo; edit them there and copy here.
`firmware/can_ring.c/h` is a read-only copy for `test_ring`; sync it when the firmware ring changes.

## Git commits

Chinese, one-line subject plus only necessary bullets; keep `build/`, logs and captures out of git.

## Firmware

Build/flash in the Windows checkout only: `idf.py build`, `idf.py -p COMx flash`.
Never put Wi-Fi credentials or the host IP in this repo.
