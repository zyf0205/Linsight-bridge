# LinSight ROS 2 Humble bridge

Minimal `rclcpp` package that reads accepted frames from the SocketCAN
interface written by `linsightd --vcan` and publishes them to ROS 2.

```text
ESP32-S3 ──Wi-Fi/TCP── linsightd ──AF_CAN── vcan0 ── linsight_bridge_node
                           │                              ├─ /can_rx
                           └─ .lsc writer                 └─ /linsight/stats
```

## 1. Create vcan (WSL2)

`vcan` is a kernel module; the interface disappears when WSL restarts.

```bash
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan
sudo ip link set up vcan0
ip -details link show vcan0
```

If `ip link add` says `File exists`, just run the `ip link set up vcan0` line.

## 2. Build

```bash
source /opt/ros/humble/setup.bash
cd ~/linsight/ros2_ws
colcon build --packages-select linsight_ros2
source install/setup.bash
```

## 3. Run the daemon with SocketCAN forwarding

The raw `AF_CAN` socket needs `CAP_NET_RAW`; in WSL the simplest path is root.
Start `linsightd` as usual and add `--vcan vcan0`:

```bash
cd ~/linsight
sudo ./host/build/linsightd --port 8888 --vcan vcan0
```

`--vcan` is optional. With it, accepted (already de-duplicated) records are
paced onto `vcan0` by device timestamps (`--pace 0` restores burst writes).
A full interface queue increments `vcan_dropped` in the 1 Hz stats line
instead of blocking the device RX path.

The daemon and the ROS node both open a raw CAN socket, so both need
`CAP_NET_RAW`. Run the node with `sudo -E` in the same shell, or grant the
capability to the installed executable:

```bash
sudo setcap cap_net_raw+ep ~/linsight/ros2_ws/install/linsight_ros2/lib/linsight_ros2/linsight_bridge_node
```

## 4. Run the bridge

```bash
# in the shell where /opt/ros/humble/setup.bash and install/setup.bash are sourced
ros2 run linsight_ros2 linsight_bridge_node --ros-args -p interface:=vcan0
```

## 5. Verify

`/can_rx` carries the frame immediately; `/linsight/stats` is a 1 Hz counter
snapshot.

```bash
ros2 topic echo /can_rx
ros2 topic hz /can_rx
ros2 topic echo /linsight/stats
```

With the daemon and node running, send a test frame onto the virtual bus from a
third terminal:

```bash
sudo apt install can-utils          # once
cansend vcan0 123#1122334455667788
ros2 topic echo /can_rx --once
```

The echo should show ID `0x123`, DLC 8 and the eight data bytes. To test the
whole LinSight path, keep the ESP32-S3 streaming to `linsightd`; every accepted
CAN_BATCH record appears on `/can_rx` after the daemon writes it to `vcan0`.

If `ros2 topic list` does not show `/can_rx`, or `ros2 topic echo` waits forever,
WSL's default Fast DDS discovery path may be unavailable. Use CycloneDDS for the
node and every `ros2` CLI command in the same shell:

```bash
export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp
```

## Message types

`CanFrame.msg`:

```text
uint32 id
uint8 dlc
bool is_extended
bool is_rtr
bool is_error
uint8[8] data
```

`BusStats.msg` adds `std_msgs/Header` plus `received`, `published`, `errors` and
`dropped` counters. `id` is the 11-bit or 29-bit identifier without the
SocketCAN flag bits.
