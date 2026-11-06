# Changelog

## v0.2.0 — 2026-11-07

- HTTP + SSE dashboard served by `linsightd` on the same epoll loop
  (single-file `web/index.html`, no framework or build step).
- `--log-max-mb` size-based `.lsc` rotation with continuous timestamps.
- systemd unit (`DynamicUser`, `StateDirectory`, journald) and CMake install
  rules for binaries, page and the prefix's `etc/default/linsightd`.
- `linsight-cli decode` for DBC files (Intel/Motorola, signed, factor/offset;
  conditional multiplexed signals skipped) with a unit test.
- Counter-checked SocketCAN soak tool (`tools/can_soak.py`) with `.lsc` verifier.
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
