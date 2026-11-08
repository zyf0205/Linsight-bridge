#!/usr/bin/env bash
# can-up.sh — one-shot USB-CAN bench bring-up for WSL2 (usbipd + peak_usb).
#
# Replaces the manual sequence from CLAUDE.md:
#   usbipd attach --wsl --busid X-Y   (Windows, admin)
#   sudo modprobe peak_usb
#   sudo ip link set can0 type can bitrate 500000
#   sudo ip link set can0 up
#
# Usage:
#   ./can-up.sh                 # default busid 2-1, bitrate 500000
#   ./can-up.sh 2-1 250000      # explicit busid / bitrate
#   ./can-up.sh --status        # show current state only
#
# Notes:
# - `usbipd attach --wsl` needs an *elevated* Windows terminal unless the
#   busid was bound once with `usbipd bind` (then attach works per-user).
#   This script calls usbipd via cmd.exe; if it fails, run the one-liner
#   from an admin PowerShell and re-run this script with --status.
# - modprobe/ip use sudo (password prompt). can-down.sh tears it all down.

set -u

BUSID="${1:-2-1}"
BITRATE="${2:-500000}"

USBIPD="/mnt/c/Program Files/usbipd-win/usbipd.exe"

state() {
    echo "== WSL =="
    lsusb 2>/dev/null | grep -i peak || echo "  no PEAK device visible"
    if ip -br link show can0 >/dev/null 2>&1; then
        echo "  can0: $(ip -br link show can0 | awk '{print $2, $3}')"
    else
        echo "  can0: absent"
    fi
}

if [ "${1:-}" = "--status" ]; then
    if [ -x "$USBIPD" ] || [ -f "$USBIPD" ]; then
        echo "== Windows (usbipd) =="
        "$USBIPD" list 2>/dev/null | grep -Ei "peak|BUSID" || true
    fi
    state
    exit 0
fi

# 1. Attach the device from Windows (no-op when already attached).
if [ -f "$USBIPD" ]; then
    if lsusb 2>/dev/null | grep -qi peak; then
        echo "[1/4] PEAK device already attached to WSL"
    else
        echo "[1/4] usbipd attach --wsl --busid $BUSID"
        if ! "$USBIPD" attach --wsl --busid "$BUSID" 2>&1; then
            echo "ERROR: attach failed. One-time admin setup on Windows:"
            echo "  usbipd bind --busid $BUSID        (admin, once)"
            echo "  usbipd attach --wsl --busid $BUSID (admin if not bound)"
            exit 1
        fi
        # The kernel needs a moment to enumerate the USB device.
        for _ in 1 2 3 4 5; do
            lsusb 2>/dev/null | grep -qi peak && break
            sleep 1
        done
    fi
else
    echo "[1/4] usbipd not found at $USBIPD (attach skipped)"
fi

lsusb 2>/dev/null | grep -qi peak || { echo "ERROR: PEAK device not visible in WSL"; exit 1; }

# 2. Load the driver (idempotent).
echo "[2/4] modprobe peak_usb"
sudo modprobe peak_usb || exit 1

# 3. Configure the bitrate (a running can0 must be set down first).
echo "[3/4] can0 bitrate $BITRATE"
if ip link show can0 >/dev/null 2>&1; then
    sudo ip link set can0 down 2>/dev/null
    sudo ip link set can0 type can bitrate "$BITRATE" || exit 1
else
    echo "  can0 does not exist yet; checking dmesg"
    sudo dmesg | tail -5
    exit 1
fi

# 4. Bring it up.
echo "[4/4] ip link set can0 up"
sudo ip link set can0 up || exit 1

state
echo
echo "Ready. Quick check:    candump -n 5 can0"
echo "Generate test frames:  cangen can0 -n 500 -g 20 -I 0x123 -L 8"
echo "Tear down:             ./can-down.sh"
