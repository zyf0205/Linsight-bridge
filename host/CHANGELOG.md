# Changelog

## Unreleased

- Built-in traffic simulator (`src/sim.c`): `engine`/`bms`/`mix`/`counter`
  patterns at 1–1000 fps plus one-shot inject, driven from a 10 Hz timerfd in
  the daemon's epoll loop. Simulated frames reuse the real ingest path (writer
  ring, CLI `live`, SSE) and carry a monotone `seq`, so `verify --strict` is
  clean. No hardware and no device needed; sim traffic never reaches the bus.
- CLI `sim start|stop|status|inject`; dashboard Simulate row; `sim` object in
  `/api/stats` and `linsight-cli stats`.
- One-shot USB-CAN bench bring-up: `tools/can-up.sh` / `tools/can-down.sh`
  wrap `usbipd attach`, `modprobe peak_usb`, bitrate configuration and
  `ip link set can0 up`, with `--status` for diagnostics.
- `test_sim` added; suite is 13 tests.

## v0.3.0 — 2026-11-08

- Protocol v2.1 (additive): `DEVICE_INFO` type 10, HELLO `device_id`/`boot`/
  `uptime_ms`, `pause` lease and `mark`; `.lsc` header keeps boot count and a
  millisecond anchor in its reserved bytes.
- Host timestamp trust: HELLO uptime anchors device time once per boot; a
  reconnect backlog is deduped by `ts_ms`/`seq` and gaps are counted instead of
  shifting history. 512-byte STATS frames are no longer dropped.
- Nonblocking daemon: device and CLI replies use per-connection output queues
  with EPOLLOUT; no `send_all`/`poll` in epoll callbacks, ACK timeout 3 s,
  64-event epoll batches.
- `.lsc` recovery/retention: re-open after a transient write error, 1 Hz
  `fdatasync`, `--log-max-total-mb` oldest-first deletion, 64 KiB HTTP response.
- CLI: socket auto-detection, `--ctl-port`, `--version`, `pause --lease`,
  `import` (mtime/`--boot-wall` re-anchoring) and `verify` (`--strict`).
- Exports now match can-utils candump (`123#1122`, `18DAF110#...`, `#R`) and
  Vector ASC headers/records; exporting onto the input path is rejected.
- Tests: 12 (session, export, import added); integration covers dedupe and
  nonblocking control.

## v0.2.0 — 2026-11-07

- HTTP + SSE dashboard served by `linsightd` on the same epoll loop
  (single-file `web/index.html`, no framework or build step).
- `--log-max-mb` size-based `.lsc` rotation with continuous timestamps.
- systemd unit (`DynamicUser`, `StateDirectory`, journald) and CMake install
  rules for binaries, page and the prefix's `etc/default/linsightd`.
- `linsight-cli decode` for DBC files (Intel/Motorola, signed, factor/offset;
  conditional multiplexed signals skipped) with a unit test.
- Counter-checked SocketCAN soak tool (`tools/can_soak.py`) with `.lsc` verifier,
  and `tools/soak24h.py` for unattended runs with health samples and per-part
  verification.
- 10.9-minute bench, 600k counter-checked frames (~917 fps; PCAN-USB over
  usbip is limited to about 1 ms per frame): device/host zero drops and
  continuous per-frame counters.
- 1 Mbps / 60 s bench: 54k frames, zero drops, no reset or WDT (~825 fps,
  about 10% bus load).

## v0.1.0 — 2026-11-06

- epoll daemon, `.lsc` writer thread with bounded ring, CLI `stats/live/config/
  pause/resume/clear`, candump/ASC export.
- `replay` (AF_CAN + absolute clock_nanosleep, lateness histogram) and
  `compare` (FIFO per `(id, flags, dlc, data)`).
- Parser/ring/canlog/compare tests; ASan/UBSan build; CI workflow.
