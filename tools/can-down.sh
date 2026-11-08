#!/usr/bin/env bash
# can-down.sh — detach the USB-CAN adapter from WSL (companion to can-up.sh).
#
# Brings can0 down and hands the device back to Windows, so the PCAN can be
# used from Windows tools or safely unplugged.

set -u

USBIPD="/mnt/c/Program Files/usbipd-win/usbipd.exe"

if ip link show can0 >/dev/null 2>&1; then
    echo "[1/2] ip link set can0 down"
    sudo ip link set can0 down || true
else
    echo "[1/2] can0 absent (nothing to bring down)"
fi

echo "[2/2] usbipd detach"
if [ -f "$USBIPD" ]; then
    # Detach any WSL-attached busid; ignore failures when nothing is attached.
    "$USBIPD" list 2>/dev/null | awk '/Attached/ {print $1}' | while read -r busid; do
        echo "  detaching $busid"
        "$USBIPD" detach --busid "$busid" 2>&1 || true
    done
else
    echo "  usbipd not found (skipped)"
fi

echo "Done. can-up.sh brings it back."
