#!/usr/bin/env bash
# can-down.sh — detach the USB-CAN adapter from WSL (companion to can-up.sh).
#
# Brings can0 down and hands the device back to Windows, so the PCAN can be
# used from Windows tools or safely unplugged.
# Usage: ./can-down.sh [BUSID] (default 2-1, matching can-up.sh).

set -u

BUSID="${1:-2-1}"

USBIPD="/mnt/c/Program Files/usbipd-win/usbipd.exe"

if ip link show can0 >/dev/null 2>&1; then
    echo "[1/2] ip link set can0 down"
    sudo ip link set can0 down || true
else
    echo "[1/2] can0 absent (nothing to bring down)"
fi

echo "[2/2] usbipd detach"
if [ -f "$USBIPD" ]; then
    echo "  detaching $BUSID"
    "$USBIPD" detach --busid "$BUSID" 2>&1 || true
else
    echo "  usbipd not found (skipped)"
fi

echo "Done. can-up.sh brings it back."
