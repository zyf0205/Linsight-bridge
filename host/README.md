# LinSight host

Linux side of LinSight, a wireless CAN analyzer: `linsightd`, a thin epoll
bridge that feeds ecosystem tools, and `tools/lsc.py` for SD black-box `.lsc`
files. The ESP32-S3 firmware lives in a separate checkout and speaks protocol
v2.1 from `protocol/`.

```text
device ──Wi-Fi/TCP── linsightd ── --vcan IFACE (paced)  → SavvyCAN / ROS 2 / candump
                       ├─ --dump PATH|-                  → candump text (no root)
                       └─ tools/lsc.py on recNNNNN.lsc   → asc/candump/csv/verify
```

The host does not record and has no CLI: the device SD card, `--dump`, or
tcpdump/SavvyCAN own capture; analysis happens in third-party tools.

## Build and test

```bash
cd host
cmake -S . -B build && cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure        # 9 tests
cmake -S . -B build-asan -DLINSIGHT_SANITIZE=ON   # ASan/UBSan
```

Tests cover the shared parser (including fuzz), record layout, the firmware
ring drop-oldest policy, HELLO anchoring/dedupe, the pacing scheduler,
`lsc.py` output shape (byte-locked to the former C formats), and an
end-to-end bridge test (passthrough format, time anchoring, PING/PONG,
dedupe, generation-change flood, clean SIGTERM).

## Run

```bash
sudo ./build/linsightd --port 8888 --vcan vcan0 [--dump cap.log|-] \
                        [--pace SPEED] [--pace-replay]
```

- `--vcan IFACE` — forward accepted (de-duplicated) records to SocketCAN,
  paced by device timestamps so cycle times in SavvyCAN/PlotJuggler are real.
  A raw CAN socket needs `CAP_NET_RAW`. Late records pop immediately
  (reconnect backlog compresses instead of stalling); a full interface queue
  increments `vcan_dropped` in the 1 Hz stderr stats line.
- `--pace SPEED` — pacing speed factor (default 1.0 = wall-clock 1:1). `0`
  restores unpaced burst writes. The factor scales intervals only with
  `--pace-replay`; live wall pacing follows device time.
- `--pace-replay` — replay historical gaps 1:1 (including backlog delays)
  instead of collapsing them to live time.
- `--dump PATH|-` — live candump text (`(sec.usec) can0 123#AABB...`), the
  same format as `lsc.py export candump`; flushed per batch. Works without
  root and doubles as a testable output.

The device pings every 5 s; a silent session is closed after 30 s. The 1 Hz
stats line reports `recs/dump/vcan/vcan_dropped/paceq`.

## SD black-box tool: tools/lsc.py

The device writes `recNNNNN.lsc` (32-byte header + 20-byte records) to its
SD card; copy the file out and process it here:

```bash
python3 ../tools/lsc.py export csv     rec00001.lsc -o rec00001.csv
python3 ../tools/lsc.py export candump rec00001.lsc -o rec00001.log
python3 ../tools/lsc.py export asc     rec00001.lsc -o rec00001.asc
python3 ../tools/lsc.py verify rec00001.lsc [--strict]
python3 ../tools/lsc.py import rec00001.lsc -o anchored.lsc [--boot-wall EPOCH]
```

- `export` filters: `--id ID`, `--ext|--std`, `--from EPOCH`, `--to EPOCH`
  (from inclusive, to exclusive). Time filters need a wall anchor: run
  `import` first on uptime-only files. Output is byte-identical to the
  historical `linsight-cli export` formats (can-utils candump, Vector ASC 9.0,
  CSV `wall_ms,uptime_ms,seq,id,ext,rtr,dlc,data_hex`).
- `verify` checks header, record length, time order and sequence gaps;
  `--strict` makes a truncated tail or any gap a failure.
- `import` re-anchors uptime timestamps to wall time (`--boot-wall` for an
  exact epoch, else mtime-based with a warning).
- Exit codes: `0` success, `1` verify `--strict` failure, `2` usage/IO error.

## Ecosystem tools (analysis front-ends)

LinSight is the capture/bridge layer; interactive decoding and plotting are
handed to established tools. GUI tools run under WSLg:

| Need | Tool | Data path |
|---|---|---|
| Live trace, DBC decode, signal plots | [SavvyCAN](https://www.savvycan.com/) | SocketCAN on the `--vcan` interface, or File → Import of `lsc.py export asc/candump/csv` |
| Quick terminal glance | can-utils | `candump vcan0` (or `linsightd --dump -`) |
| Frame-level forensics | Wireshark | `tcpdump -i vcan0 -w cap.pcapng`, open the capture on Windows |
| Multi-signal time plots | PlotJuggler | ROS 2 topic `/can_rx` (`ros2 run plotjuggler plotjuggler`), or import the CSV |

```bash
sudo ip link add vcan0 type vcan && sudo ip link set vcan0 up
sudo ./build/linsightd --port 8888 --vcan vcan0      # CAP_NET_RAW
python3 ../tools/lsc.py export asc rec00001.lsc -o capture.asc   # offline import
```

## SocketCAN / ROS 2

`--vcan IFACE` opens `PF_CAN/CAN_RAW` and writes each accepted record as a
`struct can_frame`; `LINSIGHT_REC_FLAG_EXT`, `_RTR` and `_ERR` map to
`CAN_EFF_FLAG`, `CAN_RTR_FLAG` and `CAN_ERR_FLAG`. Pacing uses the device
timestamps (1:1 wall time by default); the epoll loop never blocks on a full
interface queue.

The minimal ROS 2 Humble package in `../ros2_ws/src/linsight_ros2` reads that
interface and publishes `/can_rx` (`linsight_ros2/msg/CanFrame`) and a 1 Hz
`/linsight/stats` (`BusStats`). See its `README_ROS2.md` for vcan setup,
`colcon build`, permissions and verification.

## Install as a service

```bash
sudo cmake --install build            # /usr/local by default
sudo systemctl daemon-reload
sudo systemctl enable --now linsightd
journalctl -u linsightd -f
```

Installs `linsightd`, the systemd unit and `/etc/default/linsightd`
(`LINSIGHTD_ARGS=--port 8888`, with `--vcan`/`--dump` examples in comments).
The unit uses `DynamicUser`, `ProtectSystem=strict` and `CAP_NET_RAW` (for
optional `--vcan`); the daemon keeps no state on disk.

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
```

For counter-checked soaks use `../tools/can_soak.py` (AF_CAN generator, 32-bit
payload counter) and compare against the device SD capture or a `--dump`/`--vcan`
recording with `lsc.py verify` / `can_soak.py --verify`.
