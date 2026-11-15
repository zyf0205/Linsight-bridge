#!/usr/bin/env python3
"""Shared .lsc binary-log helpers for the LinSight Python toolchain.

Layout (mirrors host/src/canlog.c / canlog.h and protocol/frame.h):
  32-byte header: "LSC1" | u16 version | u32 bitrate | u32 flags |
                  u16 reserved | u64 unix seconds at uptime 0 | u32 boot_count |
                  u32 anchor_ms (millisecond remainder of the wall anchor)
  followed by packed 20-byte records, little-endian:
                  seq u16 | ts_ms u32 | id u32 | flags u8 | dlc u8 | data[8]

This module is pure stdlib and allocation-free in spirit: it only streams and
formats, mirroring the C reader (host/src/canlog.c) so tools/can_soak.py,
tools/lsc.py and the host tests share one implementation.
"""

import struct

LSC_MAGIC = b"LSC1"
LSC_HEADER_SIZE = 32
LSC_VERSION = 1
# seq u16 | ts_ms u32 | id u32 | flags u8 | dlc u8 | data 8s
LSC_REC = struct.Struct("<HIIBB8s")

# Header flags (canlog.h).
LSC_FLAG_TS_UPTIME_ONLY = 0x1  # start_wall is not a wall clock
LSC_FLAG_ANCHOR_MS_VALID = 0x2  # anchor_ms is meaningful
LSC_FLAG_BOOT_VALID = 0x4  # boot_count is meaningful

# Record flags (protocol/frame.h).
LINSIGHT_REC_FLAG_EXT = 0x01
LINSIGHT_REC_FLAG_RTR = 0x02
LINSIGHT_REC_FLAG_ERR = 0x04
LINSIGHT_DLC_MAX = 8

# magic, version, bitrate, flags, reserved, start_wall_s, boot_count, anchor_ms
_HEADER = struct.Struct("<4sHIIHQII")


def header_bytes(bitrate, flags, start_wall_s, boot_count, anchor_ms):
    """Pack the 32-byte file header (canlog.c lsc_writer_open_ex layout)."""
    return _HEADER.pack(LSC_MAGIC, LSC_VERSION,
                        bitrate & 0xFFFFFFFF, flags & 0xFFFFFFFF, 0,
                        start_wall_s & 0xFFFFFFFFFFFFFFFF,
                        boot_count & 0xFFFFFFFF, anchor_ms & 0xFFFFFFFF)


def read_header(f):
    """Read and validate the 32-byte header from binary file `f`.

    Returns (start_wall_ms, flags, boot_count, bitrate). Raises ValueError on
    a short or corrupt header. An anchor_ms > 999 is rejected when the file
    claims it is valid (LSC_FLAG_ANCHOR_MS_VALID); an untrusted anchor is
    clamped to 0 exactly like canlog.c lsc_read_header().
    """
    hdr = f.read(LSC_HEADER_SIZE)
    if len(hdr) != LSC_HEADER_SIZE or hdr[:4] != LSC_MAGIC:
        raise ValueError("short or invalid .lsc header")
    _magic, version, bitrate, flags, _reserved, start_wall_s, boot_count, anchor_ms = \
        _HEADER.unpack(hdr)
    if version != LSC_VERSION:
        raise ValueError("unsupported .lsc version")
    if flags & LSC_FLAG_ANCHOR_MS_VALID and anchor_ms > 999:
        raise ValueError("invalid millisecond anchor")
    if anchor_ms > 999:
        anchor_ms = 0  # untrusted or older file (canlog.c lsc_read_header)
    return start_wall_s * 1000 + anchor_ms, flags, boot_count, bitrate


def iter_records(f):
    """Yield (seq, ts_ms, id, flags, dlc, data) tuples from binary `f`.

    Streams one 20-byte record at a time from the current position; a
    truncated tail record is tolerated as EOF (canlog.c lsc_reader_next).
    dlc is clamped to 8 like the C reader so callers never index past data.
    """
    while True:
        raw = f.read(LSC_REC.size)
        if len(raw) != LSC_REC.size:
            return  # EOF or truncated tail record
        seq, ts_ms, can_id, flags, dlc, data = LSC_REC.unpack(raw)
        if dlc > LINSIGHT_DLC_MAX:
            dlc = LINSIGHT_DLC_MAX
        yield seq, ts_ms, can_id, flags, dlc, data


def frame_valid(seq, ts_ms, can_id, flags, dlc, data):
    """Per-frame sanity check (tools/can_soak.py verify_file record rules)."""
    return (flags & ~0x07) == 0 and dlc <= LINSIGHT_DLC_MAX and \
        can_id <= (0x1FFFFFFF if flags & LINSIGHT_REC_FLAG_EXT else 0x7FF)


def candump_line(rec, start_wall_ms):
    """Format one record as a can-utils candump log line.

    Byte-level replica of canlog.c canlog_format_candump_ms() (no trailing
    newline): "(sec.usec) can0 ID#DATA", extended IDs use %08X, RTR is "#R".
    """
    _seq, ts_ms, can_id, flags, dlc, data = rec
    wall_ms = start_wall_ms + ts_ms
    sec, usec = divmod(wall_ms, 1000)
    usec *= 1000
    ident = can_id & 0x1FFFFFFF
    if flags & LINSIGHT_REC_FLAG_EXT:
        line = f"({sec}.{usec:06d}) can0 {ident:08X}#"
    else:
        line = f"({sec}.{usec:06d}) can0 {ident:03X}#"
    if flags & LINSIGHT_REC_FLAG_RTR:
        return line + "R"
    for i in range(min(dlc, LINSIGHT_DLC_MAX)):
        line += f"{data[i]:02X}"
    return line
