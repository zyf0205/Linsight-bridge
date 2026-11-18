# CLAUDE.md

This file provides guidance to Claude Code when working in the WSL Linux host repository.

## Repository state

LinSight Linux host: `linsightd`, a Linux `epoll` bridge (module sources under
`src/`: TCP device session -> reconnect dedupe -> `CLOCK_MONOTONIC` 1:1 paced
SocketCAN forward). The ESP32-S3 firmware, Wi-Fi credentials, and flashing live
in the separate Windows checkout `D:\_study\projects\_MyProject\LinSight`; this
repo does not build firmware.

Layout:
- `src/main.c` — CLI parsing, epoll loop, signal/timer handling, TCP listener, lifecycle
- `src/device.c` — device session, v2.1 frame dispatch, reconnect dedupe, PONG TX
- `src/pacer.c` — `CLOCK_MONOTONIC` 1:1 pacing ring and pump
- `src/vcan.c` — SocketCAN open/forward/stats
- `src/discovery.c` — UDP discovery responder
- `src/util.c` — nonblocking helper and monotonic clock
- `systemd/` — `linsightd.service`, `vcan0.service` + args template (CMake `install` targets)
- `protocol/` — shared v2.1 parser (mirror of the firmware repo)
- `README.md` — build, usage and service reference (the hardware doc lives in the firmware repo)

## Commands

```bash
cd ~/linsight
cmake -S . -B build && cmake --build build -j$(nproc)

sudo ./build/linsightd --port 8888 --vcan vcan0 [--no-pace | --pace 0|1]

sudo cmake --install build        # linsightd + both units + prefix etc/default
sudo systemctl daemon-reload
sudo systemctl enable --now vcan0.service linsightd
```

SocketCAN: `--vcan vcan0` forwards accepted records to SocketCAN;
`linsightd` needs `CAP_NET_RAW` (run with sudo in WSL). `vcan0.service`
creates the interface at boot; for a manual session use
`sudo ip link add vcan0 type vcan && sudo ip link set vcan0 up`.

Analysis and text logging belong to Linux ecosystem tools (`candump vcan0`,
SavvyCAN, Wireshark, PlotJuggler) fed via SocketCAN — do not re-grow an
in-house text formatter, decoder or GUI.

## Architecture

- `linsightd` (`src/`, single-threaded `epoll`): TCP listener for the
  device (`INADDR_ANY`), UDP discovery responder (default `8889`, replies to
  `LINSIGHT?1` with `LINSIGHT!1 <port>\n`), `signalfd`, 1 Hz `timerfd` (30 s
  idle watchdog + stderr stats line). One device session at a time.
- Dedupe: `session_record_new` skips a reconnect backlog by `ts_ms`/`seq`
  marks; marks survive a reconnect with the same `boot` count and clear on a
  boot change. HELLO `uptime_ms` anchors device time onto `CLOCK_MONOTONIC`
   directly (`anchor_mono_ns`, `anchor_dev_ms`); old firmware falls back to the
  first record's timestamp.
- `--vcan IFACE`: accepted records are paced onto SocketCAN (`PF_CAN`,
  `SOCK_RAW`) as `struct can_frame` (`CAN_EFF_FLAG`/`CAN_RTR_FLAG`/`CAN_ERR_FLAG`
  mapped) at their 1:1 `CLOCK_MONOTONIC` target times via a 262144-entry
  drop-oldest ring. Late backlog (`target_ns <= now_ns`) pops immediately
  without catch-up sleeps. `--no-pace` (or `--pace 0`) forwards immediately in
  burst mode. Per-iteration budget (`4096` records or `5 ms`) keeps `PONG`
  latency bounded.
- `protocol/frame.c` is pure C (no IDF, no malloc); keep it allocation-free and fuzzable.
- `systemd/vcan0.service` (oneshot) creates and brings up vcan0 before
  `linsightd.service`; the daemon unit uses `DynamicUser`,
  `ProtectSystem=strict`, `CAP_NET_RAW` and reads
  `<prefix>/etc/default/linsightd`.

## Shared files

`protocol/` is a mirror of the firmware repo; edit it there and copy here.
The hardware doc (`docs/hardware.md`) lives in the firmware repo only.

## Git commits

Chinese, one-line subject plus only necessary bullets; keep `build/`, logs and captures out of git.

## Firmware

Build/flash in the Windows checkout only: `idf.py build`, `idf.py -p COMx flash`.
Bench Wi-Fi credentials are Kconfig defaults in the firmware repo (`main/Kconfig.projbuild`);
do not add secrets or host IPs to this repo.
