#!/usr/bin/env python3
"""LinSight .lsc tooling: export / verify / import.

Python replacement for the linsight-cli export/verify/import subcommands
(host/src/canlog.c, host/src/import.c). The three export formats are
byte-identical to the C exporter.

Examples:
    python3 tools/lsc.py export asc|candump|csv FILE.lsc [-o OUT]
                              [--id ID] [--ext|--std] [--from EPOCH] [--to EPOCH]
    python3 tools/lsc.py verify FILE.lsc [--strict]
    python3 tools/lsc.py import SRC.lsc -o DST.lsc [--boot-wall EPOCH]
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from lscfile import (LSC_FLAG_TS_UPTIME_ONLY, LSC_FLAG_ANCHOR_MS_VALID,
                     LINSIGHT_DLC_MAX, LINSIGHT_REC_FLAG_EXT, LINSIGHT_REC_FLAG_RTR,
                     LSC_HEADER_SIZE, LSC_REC,
                     candump_line, header_bytes, iter_records, read_header)

USAGE = ("usage: lsc.py export asc|candump|csv FILE.lsc [-o OUT]\n"
         "                         [--id ID] [--ext|--std] [--from EPOCH] [--to EPOCH]\n"
         "       lsc.py verify FILE.lsc [--strict]\n"
         "       lsc.py import SRC.lsc -o DST.lsc [--boot-wall EPOCH]\n")

_WEEKDAYS = ("Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat")
_MONTHS = ("Jan", "Feb", "Mar", "Apr", "May", "Jun",
           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")


class LscError(Exception):
    """A user-facing import failure (mapped to lsc_import's err buffer)."""


def usage():
    print(USAGE, end="", file=sys.stderr)


# ------------------------------------------------------------- arg parsing
# Faithful subset of the C helpers (host/cli/cli.c): strtoul base 0 for --id,
# strtoull base 10 for epochs.

_DIGITS = {10: "0123456789", 8: "01234567", 16: "0123456789abcdefABCDEF"}
_SPACE = " \t\n\v\f\r"


def _strtoull_base0(s):
    i = 0
    while i < len(s) and s[i] in _SPACE:
        i += 1
    neg = False
    if i < len(s) and s[i] in "+-":
        neg = s[i] == "-"
        i += 1
    if s[i:i + 2].lower() == "0x" and i + 2 < len(s) and s[i + 2] in _DIGITS[16]:
        base = 16
        i += 2
    elif i < len(s) and s[i] == "0":
        base = 8  # "09" stops after "0" and fails the trailing check below
    else:
        base = 10
    start = i
    while i < len(s) and s[i] in _DIGITS[base]:
        i += 1
    if i == start or i != len(s):
        return None
    v = int(s[start:i], base)
    return -v if neg else v


def parse_u32(s, max_v):
    v = _strtoull_base0(s)
    return None if v is None or v > max_v else v


def parse_u64(s):
    i = 0
    while i < len(s) and s[i] in _SPACE:
        i += 1
    if i < len(s) and s[i] == "-":
        return None  # strtoull wraps; a negative epoch is never useful
    if i < len(s) and s[i] == "+":
        i += 1
    start = i
    while i < len(s) and s[i] in _DIGITS[10]:
        i += 1
    if i == start or i != len(s):
        return None
    return int(s[start:i], 10)


# ------------------------------------------------------------------ export

class _Filter:
    def __init__(self):
        self.id_valid = False
        self.id = 0
        self.ext = -1  # -1 any, 0 std only, 1 ext only
        self.from_valid = False
        self.from_wall = 0
        self.to_valid = False
        self.to_wall = 0


def format_asc_date(wall_sec):
    """glibc LC_ALL=C strftime('%a %b %e %H:%M:%S %Y') equivalent (canlog.c)."""
    try:
        tm = time.localtime(wall_sec)
    except (OverflowError, OSError, ValueError):
        return "Thu Jan  1 00:00:00 1970"
    return "%s %s %2d %02d:%02d:%02d %d" % (
        _WEEKDAYS[(tm.tm_wday + 1) % 7], _MONTHS[tm.tm_mon - 1], tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec, tm.tm_year)


def _match(rec, flt, start_wall_ms):
    _seq, ts_ms, can_id, flags, _dlc, _data = rec
    if flt.id_valid and can_id != flt.id:
        return False
    if flt.ext >= 0 and (1 if flags & LINSIGHT_REC_FLAG_EXT else 0) != flt.ext:
        return False
    wall = (start_wall_ms + ts_ms) // 1000
    if flt.from_valid and wall < flt.from_wall:
        return False
    if flt.to_valid and wall >= flt.to_wall:
        return False
    return True


def _asc_line(rec):
    _seq, ts_ms, can_id, flags, dlc, data = rec
    ts = ts_ms / 1000.0
    kind = "r" if flags & LINSIGHT_REC_FLAG_RTR else "d"
    if flags & LINSIGHT_REC_FLAG_EXT:
        line = f"{ts:10.6f} 1  {can_id:08X}x"
    else:
        line = f"{ts:10.6f} 1  {can_id:03X}"
    line += f"            Rx   {kind} {dlc}"
    if not flags & LINSIGHT_REC_FLAG_RTR:
        for i in range(min(dlc, LINSIGHT_DLC_MAX)):
            line += f" {data[i]:02X}"
    return line + "\n"


def _csv_line(rec, start_wall_ms):
    seq, ts_ms, can_id, flags, dlc, data = rec
    ext = 1 if flags & LINSIGHT_REC_FLAG_EXT else 0
    rtr = 1 if flags & LINSIGHT_REC_FLAG_RTR else 0
    line = f"{start_wall_ms + ts_ms},{ts_ms},{seq},0x{can_id:X},{ext},{rtr},{dlc},"
    if not rtr:
        for i in range(min(dlc, LINSIGHT_DLC_MAX)):
            line += f"{data[i]:02X}"
    return line + "\n"


def _do_export(fmt, lsc_path, out_path, flt):
    with open(lsc_path, "rb") as f:
        start_wall_ms, _flags, _bc, _br = read_header(f)
        if out_path == "-":
            out, close_out = sys.stdout, False
        else:
            out, close_out = open(out_path, "w", newline="\n"), True
        count = 0
        try:
            if fmt == "asc":
                date = format_asc_date(start_wall_ms // 1000 if start_wall_ms
                                       else int(time.time()))
                out.write(f"date {date}\n")
                out.write("base hex  timestamps absolute\n")
                out.write("internal events logged\n")
                out.write("// version 9.0.0\n")
                out.write(f"Begin Triggerblock {date}\n")
                out.write("   0.000000 Start of measurement\n")
            elif fmt == "csv":
                out.write("wall_ms,uptime_ms,seq,id,ext,rtr,dlc,data_hex\n")
            try:
                for rec in iter_records(f):
                    if not _match(rec, flt, start_wall_ms):
                        continue
                    if fmt == "asc":
                        out.write(_asc_line(rec))
                    elif fmt == "csv":
                        out.write(_csv_line(rec, start_wall_ms))
                    else:
                        out.write(candump_line(rec, start_wall_ms) + "\n")
                    count += 1
            finally:
                if fmt == "asc":
                    out.write("End Triggerblock\n")
        finally:
            if close_out:
                out.close()
    return count


def cmd_export(args):
    if len(args) < 3:
        usage()
        return 2
    fmt, lsc_path = args[1], args[2]
    out_path = "-"
    flt = _Filter()
    i = 3
    while i < len(args):
        a = args[i]
        if a == "-o" and i + 1 < len(args):
            i += 1
            out_path = args[i]
        elif a == "--id" and i + 1 < len(args):
            i += 1
            v = parse_u32(args[i], 0x1FFFFFFF)
            if v is None:
                print(f"lsc: bad --id '{args[i]}'", file=sys.stderr)
                return 2
            flt.id, flt.id_valid = v, True
        elif a == "--ext":
            if flt.ext == 0:
                print("lsc: --ext and --std are mutually exclusive", file=sys.stderr)
                return 2
            flt.ext = 1
        elif a == "--std":
            if flt.ext == 1:
                print("lsc: --ext and --std are mutually exclusive", file=sys.stderr)
                return 2
            flt.ext = 0
        elif a == "--from" and i + 1 < len(args):
            i += 1
            v = parse_u64(args[i])
            if v is None:
                print(f"lsc: bad --from '{args[i]}'", file=sys.stderr)
                return 2
            flt.from_wall, flt.from_valid = v, True
        elif a == "--to" and i + 1 < len(args):
            i += 1
            v = parse_u64(args[i])
            if v is None:
                print(f"lsc: bad --to '{args[i]}'", file=sys.stderr)
                return 2
            flt.to_wall, flt.to_valid = v, True
        else:
            usage()
            return 2
        i += 1
    if flt.from_valid and flt.to_valid and flt.to_wall <= flt.from_wall:
        print("lsc: --to must be greater than --from", file=sys.stderr)
        return 2
    if flt.from_valid or flt.to_valid:
        # Time filters are wall-clock epochs; an uptime-only file has no wall
        # anchor (cmd_export pre-check).
        try:
            with open(lsc_path, "rb") as f:
                _sw, flags, _bc, _br = read_header(f)
        except (OSError, ValueError):
            pass  # export itself reports the failure
        else:
            if flags & LSC_FLAG_TS_UPTIME_ONLY:
                print("lsc: --from/--to need wall-clock timestamps; "
                      "run 'lsc.py import' to anchor the file first",
                      file=sys.stderr)
                return 2
    if fmt not in ("asc", "candump", "csv"):
        print(f"lsc: unknown format '{fmt}' (asc|candump|csv)", file=sys.stderr)
        return 2
    if out_path != "-" and out_path == lsc_path:
        print("lsc: export failed: Invalid argument", file=sys.stderr)
        return 2
    try:
        n = _do_export(fmt, lsc_path, out_path, flt)
    except ValueError as exc:
        print(f"lsc: export failed: {exc}", file=sys.stderr)
        return 2
    except OSError as exc:
        print(f"lsc: export failed: {exc.strerror or exc}", file=sys.stderr)
        return 2
    if out_path != "-":
        print(f"{fmt}: {n} frames -> {out_path}")
    return 0


# ------------------------------------------------------------------ verify

def _file_truncated(path):
    """import.c file_truncated(): size < 32 or a partial trailing record."""
    try:
        size = os.stat(path).st_size
    except OSError:
        return True
    return size < LSC_HEADER_SIZE or (size - LSC_HEADER_SIZE) % LSC_REC.size != 0


def lsc_verify(path, strict):
    """import.c lsc_verify(): returns (rc, info, err)."""
    info = dict(records=0, boot_count=0, bitrate=0, flags=0, start_wall_ms=0,
                gaps=0, missing=0, backwards=0, duplicates=0, ambiguous=0,
                truncated=0, uptime_only=0)
    try:
        f = open(path, "rb")
    except OSError as exc:
        return 2, info, f"bad header in {path}: {exc.strerror or exc}"
    with f:
        try:
            start_wall_ms, flags, boot_count, bitrate = read_header(f)
        except ValueError as exc:
            return 2, info, f"bad header in {path}: {exc}"
        except OSError as exc:
            return 2, info, f"bad header in {path}: {exc.strerror or exc}"
        info.update(flags=flags, boot_count=boot_count, bitrate=bitrate,
                    start_wall_ms=start_wall_ms,
                    uptime_only=int(bool(flags & LSC_FLAG_TS_UPTIME_ONLY)),
                    truncated=int(_file_truncated(path)))
        have_mark = have_ts = False
        last_seq = last_ts = scan_ts = 0
        try:
            for seq, ts_ms, _can_id, _flags, _dlc, _data in iter_records(f):
                if have_ts and ts_ms < scan_ts:
                    info["backwards"] += 1
                scan_ts = ts_ms
                have_ts = True
                if not have_mark:
                    have_mark = True
                    last_ts, last_seq = ts_ms, seq
                elif ts_ms < last_ts:
                    info["duplicates"] += 1
                else:
                    # Modular difference: wrap 0xFFFF -> 0 is +1 (session.c).
                    diff = (seq - last_seq) & 0xFFFF
                    if diff >= 0x8000:
                        diff -= 0x10000
                    if ts_ms == last_ts:
                        if diff == 0 or -1024 < diff < 0:
                            info["duplicates"] += 1  # skipped, marks kept
                        else:
                            last_ts, last_seq = ts_ms, seq
                    else:
                        if diff == 0 or diff < 0:
                            info["ambiguous"] += 1
                        elif diff > 1:
                            info["gaps"] += 1
                            info["missing"] += diff - 1
                        last_ts, last_seq = ts_ms, seq
                info["records"] += 1
        except OSError:
            return 2, info, f"read error in {path}"
    if info["truncated"] and strict:
        return 1, info, "truncated tail record"
    if strict and (info["gaps"] or info["missing"] or info["backwards"]
                   or info["duplicates"]):
        return 1, info, ("sequence/time defects (gaps=%d missing=%d "
                         "backwards=%d dup=%d)" % (info["gaps"], info["missing"],
                                                  info["backwards"],
                                                  info["duplicates"]))
    return 0, info, ""


# ------------------------------------------------------------------ import

def scan_last_ts(path):
    """import.c scan_last_ts(): ts_ms of the last record (0 when empty)."""
    last = 0
    with open(path, "rb") as f:
        read_header(f)
        for _seq, ts_ms, _id, _flags, _dlc, _data in iter_records(f):
            last = ts_ms
    return last


def lsc_import(src, dst, have_boot_wall, boot_wall_sec):
    """import.c lsc_import(): copy records, re-anchor uptime-only logs."""
    if src == dst:
        raise LscError("source and destination must differ")
    try:
        f = open(src, "rb")
    except OSError as exc:
        raise LscError(f"cannot read {src}: {exc.strerror or exc}")
    with f:
        try:
            start_wall_ms, flags, boot_count, bitrate = read_header(f)
        except ValueError as exc:
            raise LscError(f"cannot read {src}: {exc}")
        except OSError as exc:
            raise LscError(f"cannot read {src}: {exc.strerror or exc}")
        wall_valid = not (flags & LSC_FLAG_TS_UPTIME_ONLY)
        info = dict(bitrate=bitrate, boot_count=boot_count, flags=flags,
                    records=0, start_wall_ms=0,
                    uptime_only=int(not wall_valid),
                    truncated=int(_file_truncated(src)))
        if not wall_valid:
            if have_boot_wall:
                start_wall_ms = boot_wall_sec * 1000
            else:
                try:
                    last_ts = scan_last_ts(src)
                except (OSError, ValueError):
                    raise LscError(f"cannot rescan {src}")
                try:
                    mtime_ms = int(os.stat(src).st_mtime) * 1000
                except OSError as exc:
                    raise LscError(f"stat {src}: {exc.strerror or exc}")
                start_wall_ms = mtime_ms - last_ts if mtime_ms > last_ts else 0
            flags &= ~LSC_FLAG_TS_UPTIME_ONLY
        flags |= LSC_FLAG_ANCHOR_MS_VALID

        try:
            out = open(dst, "wb")
        except OSError as exc:
            raise LscError(f"cannot create {dst}: {exc.strerror or exc}")
        try:
            out.write(header_bytes(bitrate, flags, start_wall_ms // 1000,
                                   boot_count, start_wall_ms % 1000))
            it = iter_records(f)
            while True:
                try:
                    rec = next(it)
                except StopIteration:
                    break
                except OSError:
                    raise LscError(f"read failed in {src}")
                try:
                    out.write(LSC_REC.pack(*rec))
                except OSError as exc:
                    raise LscError(f"write failed: {exc.strerror or exc}")
                info["records"] += 1
            out.flush()
        except LscError:
            try:
                out.close()
            except OSError:
                pass
            raise
        except OSError as exc:
            try:
                out.close()
            except OSError:
                pass
            raise LscError(f"write failed: {exc.strerror or exc}")
        try:
            out.close()
        except OSError as exc:
            raise LscError(f"close failed: {exc.strerror or exc}")
        info["start_wall_ms"] = start_wall_ms
    return info


# ---------------------------------------------------------------- sidecars

def sidecar_path(lsc_path, ext):
    """import.c sidecar_path(): 'x.lsc' -> 'x.ext' (suffix stripped)."""
    n = len(lsc_path)
    if n > 4 and lsc_path[n - 4:] == ".lsc":
        n -= 4
    return lsc_path[:n] + "." + ext


def print_sidecars(lsc_path):
    """import.c lsc_print_sidecars(): one met: line and a trg: mark count."""
    try:
        with open(sidecar_path(lsc_path, "met"), "rb") as f:
            line = f.readline(511)  # fgets cap
    except OSError:
        line = b""
    if line:
        cut = len(line)
        for i, b in enumerate(line):
            if b in (0x0D, 0x0A):
                cut = i
                break
        print("met: " + line[:cut].decode("latin-1"))
    try:
        f = open(sidecar_path(lsc_path, "trg"), "rb")
    except OSError:
        return
    with f:
        marks = 0
        while f.readline(511):
            marks += 1
    print(f"trg: {marks} mark(s)")


def cmd_verify(args):
    if len(args) < 2:
        usage()
        return 2
    path = args[1]
    strict = False
    for i in range(2, len(args)):
        if args[i] == "--strict":
            strict = True
        else:
            usage()
            return 2
    rc, info, err = lsc_verify(path, strict)
    print("verify: records=%(records)d boot=%(boot_count)d bitrate=%(bitrate)d "
          "flags=0x%(flags)X uptime_only=%(uptime_only)d gaps=%(gaps)d "
          "missing=%(missing)d backwards=%(backwards)d dup=%(duplicates)d "
          "ambiguous=%(ambiguous)d truncated=%(truncated)d" % info)
    print_sidecars(path)
    if rc != 0:
        print(f"lsc: verify: {err}", file=sys.stderr)
    return rc


def cmd_import(args):
    if len(args) < 2:
        usage()
        return 2
    src = args[1]
    dst = None
    have_boot = False
    boot_wall = 0
    i = 2
    while i < len(args):
        a = args[i]
        if a == "-o" and i + 1 < len(args):
            i += 1
            dst = args[i]
        elif a == "--boot-wall" and i + 1 < len(args):
            i += 1
            v = parse_u64(args[i])
            if v is None:
                print(f"lsc: bad --boot-wall '{args[i]}'", file=sys.stderr)
                return 2
            boot_wall, have_boot = v, True
        else:
            usage()
            return 2
        i += 1
    if dst is None:
        print("lsc: import needs -o OUT.lsc", file=sys.stderr)
        return 2
    try:
        info = lsc_import(src, dst, have_boot, boot_wall)
    except LscError as exc:
        print(f"lsc: import failed: {exc}", file=sys.stderr)
        return 2
    print("import: %d records, boot %d, bitrate %d, anchor %d ms (%s)" % (
        info["records"], info["boot_count"], info["bitrate"],
        info["start_wall_ms"],
        "re-anchored" if info["uptime_only"] else "wall clock preserved"))
    if info["uptime_only"] and not have_boot:
        print("lsc: warning: anchored from file mtime; "
              "use --boot-wall for exact dates", file=sys.stderr)
    print_sidecars(src)
    return 0


def main(argv=None):
    args = sys.argv[1:] if argv is None else argv
    if not args:
        usage()
        return 2
    cmd = args[0]
    if cmd == "export":
        return cmd_export(args)
    if cmd == "verify":
        return cmd_verify(args)
    if cmd == "import":
        return cmd_import(args)
    usage()
    return 2


if __name__ == "__main__":
    sys.exit(main())
