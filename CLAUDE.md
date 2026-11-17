# CLAUDE.md

This file provides guidance to Claude Code when working in the WSL Linux host repository.

## Repository state

LinSight Linux host: `linsightd`, a thin epoll bridge (TCP device session ->
dedupe -> paced SocketCAN forward / candump `--dump`). The ESP32-S3 firmware,
Wi-Fi credentials, and flashing live in the separate Windows checkout
`D:\_study\projects\_MyProject\LinSight`; this repo does not build firmware.

Layout:
- `src/` — bridge daemon (main.c, session dedupe, pace scheduler)
- `systemd/` — `linsightd.service`, `vcan0.service` + args template (CMake `install` targets)
- `protocol/` — shared v2.1 parser (mirror of the firmware repo)
- `README.md` — build, usage and service reference (the hardware doc lives in the firmware repo)

## Commands

```bash
cd ~/linsight
cmake -S . -B build && cmake --build build -j$(nproc)

sudo ./build/linsightd --port 8888 --vcan vcan0 [--dump cap.log|-] \
                        [--pace SPEED] [--pace-replay]

sudo cmake --install build        # linsightd + both units + prefix etc/default
sudo systemctl daemon-reload
sudo systemctl enable --now vcan0.service linsightd
```

SocketCAN: `--vcan vcan0` forwards accepted records to SocketCAN;
`linsightd` needs `CAP_NET_RAW` (run with sudo in WSL). `vcan0.service`
creates the interface at boot; for a manual session use
`sudo ip link add vcan0 type vcan && sudo ip link set vcan0 up`.

Analysis belongs to ecosystem tools (SavvyCAN, Wireshark, PlotJuggler) fed via
`--vcan` or `--dump` — do not re-grow an in-house decoder/GUI.

## Architecture

- `linsightd` (single-threaded epoll): TCP listener for the device
  (`INADDR_ANY`), UDP discovery responder (default `8889`, tag 2; replies to
  `LINSIGHT?1` with the TCP port), signalfd, 1 Hz timerfd (idle watchdog +
  stats line + `--dump` flush). One device session at a time; no writer
  thread, no recording — the SD black box or tcpdump/SavvyCAN own capture.
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
- `--dump PATH|-`: live candump text, flushed per batch; useful without root.
- `protocol/frame.c` is pure C (no IDF, no malloc); keep it allocation-free and fuzzable.
- `systemd/vcan0.service` (oneshot) creates and brings up vcan0 before
  `linsightd.service`; the daemon unit uses `DynamicUser`,
  `ProtectSystem=strict`, `CAP_NET_RAW` and reads
  `<prefix>/etc/default/linsightd`.
- Single device TCP client; the device pings every 5 s and a silent session
  is closed after 30 s. On reconnect the device drains its PSRAM ring
  backlog first (up to ~209k records) — the dedupe marks absorb it.

## Shared files

`protocol/` is a mirror of the firmware repo; edit it there and copy here.
The hardware doc (`docs/hardware.md`) lives in the firmware repo only.

## Git commits

Chinese, one-line subject plus only necessary bullets; keep `build/`, logs and captures out of git.

## Firmware

Build/flash in the Windows checkout only: `idf.py build`, `idf.py -p COMx flash`.
Bench Wi-Fi credentials are Kconfig defaults in the firmware repo (`main/Kconfig.projbuild`);
do not add secrets or host IPs to this repo.
