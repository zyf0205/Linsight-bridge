#!/usr/bin/env python3
"""Offline tests for tools/lscfile.py and tools/lsc.py; never opens devices.

Run: python3 host/tests/test_lsc.py
"""

import contextlib
import io
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))

import lsc
import lscfile

LSC = TOOLS / "lsc.py"


def setUpModule():
    # The ASC date line must be deterministic: pin the timezone like the
    # golden strings expect (glibc LC_ALL=C formatting is emulated in lsc.py).
    global _OLD_TZ
    _OLD_TZ = os.environ.get("TZ")
    os.environ["TZ"] = "UTC"
    if hasattr(time, "tzset"):
        time.tzset()


def tearDownModule():
    if _OLD_TZ is None:
        os.environ.pop("TZ", None)
    else:
        os.environ["TZ"] = _OLD_TZ
    if hasattr(time, "tzset"):
        time.tzset()


def hdr(version=1, bitrate=500000, flags=6, start_wall_s=1700000000,
        boot=7, anchor=0):
    return struct.pack("<4sHIIHQII", b"LSC1", version, bitrate, flags, 0,
                       start_wall_s, boot, anchor)


def rec(seq, ts, can_id=0x123, flags=0, dlc=2, data=b"\x11\x22"):
    return struct.pack("<HIIBB8s", seq, ts, can_id, flags, dlc,
                       (data + bytes(8))[:8])


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="linsight-lsc-test-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def write(self, name, blob):
        path = self.root / name
        path.write_bytes(blob)
        return path

    def cli(self, argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = lsc.main(argv)
        return rc, out.getvalue(), err.getvalue()

    def read_header(self, path):
        with open(path, "rb") as f:
            return lscfile.read_header(f)


# Golden fixture: std/ext/RTR/ERR, dlc 0..8, ids exercising %03X/%08X.
GOLDEN = (hdr()
          + rec(1, 1500, 0x123, 0, 2, b"\x11\x22")
          + rec(2, 1600, 0x18DAF110, 1, 2, b"\x11\x22")
          + rec(3, 1700, 0x321, 2, 3, b"\x33\x44\x55")
          + rec(4, 1800, 0x7FF, 0, 8, b"\x11\x22\x33\x44\x55\x66\x77\x88")
          + rec(5, 1900, 0x1, 7, 0, b"")
          + rec(6, 2000, 0x0, 4, 1, b"\xFF"))

GOLDEN_CANDUMP = """\
(1700000001.500000) can0 123#1122
(1700000001.600000) can0 18DAF110#1122
(1700000001.700000) can0 321#R
(1700000001.800000) can0 7FF#1122334455667788
(1700000001.900000) can0 00000001#R
(1700000002.000000) can0 000#FF
"""

GOLDEN_ASC = """\
date Tue Nov 14 22:13:20 2023
base hex  timestamps absolute
internal events logged
// version 9.0.0
Begin Triggerblock Tue Nov 14 22:13:20 2023
   0.000000 Start of measurement
  1.500000 1  123            Rx   d 2 11 22
  1.600000 1  18DAF110x            Rx   d 2 11 22
  1.700000 1  321            Rx   r 3
  1.800000 1  7FF            Rx   d 8 11 22 33 44 55 66 77 88
  1.900000 1  00000001x            Rx   r 0
  2.000000 1  000            Rx   d 1 FF
End Triggerblock
"""

GOLDEN_CSV = """\
wall_ms,uptime_ms,seq,id,ext,rtr,dlc,data_hex
1700000001500,1500,1,0x123,0,0,2,1122
1700000001600,1600,2,0x18DAF110,1,0,2,1122
1700000001700,1700,3,0x321,0,1,3,
1700000001800,1800,4,0x7FF,0,0,8,1122334455667788
1700000001900,1900,5,0x1,1,1,0,
1700000002000,2000,6,0x0,0,0,1,FF
"""


class HeaderTests(Base):
    def test_rejects_bad_headers(self):
        cases = [b"LSC1",                      # short header
                 hdr()[:31],                   # short header
                 b"XSC1" + hdr()[4:],          # bad magic
                 hdr(version=2),               # bad version
                 hdr(flags=2, anchor=1000)]    # anchor > 999 while claimed valid
        for index, blob in enumerate(cases):
            with self.subTest(index=index):
                with self.assertRaises(ValueError):
                    lscfile.read_header(io.BytesIO(blob))

    def test_clamps_untrusted_anchor(self):
        # anchor_ms > 999 without ANCHOR_MS_VALID is an old/untrusted file:
        # canlog.c lsc_read_header clamps it to 0 instead of failing.
        start, flags, boot, bitrate = lscfile.read_header(
            io.BytesIO(hdr(flags=0, anchor=1500, start_wall_s=1700000000)))
        self.assertEqual(start, 1700000000000)
        self.assertEqual((flags, boot, bitrate), (0, 7, 500000))

    def test_header_roundtrip_and_layout(self):
        blob = lscfile.header_bytes(500000, 6, 1700000000, 7, 123)
        self.assertEqual(len(blob), 32)
        self.assertEqual(blob[:4], b"LSC1")
        self.assertEqual(struct.unpack_from("<H", blob, 4)[0], 1)
        self.assertEqual(struct.unpack_from("<I", blob, 6)[0], 500000)
        self.assertEqual(struct.unpack_from("<I", blob, 10)[0], 6)
        self.assertEqual(struct.unpack_from("<H", blob, 14)[0], 0)
        self.assertEqual(struct.unpack_from("<Q", blob, 16)[0], 1700000000)
        self.assertEqual(struct.unpack_from("<I", blob, 24)[0], 7)
        self.assertEqual(struct.unpack_from("<I", blob, 28)[0], 123)
        self.assertEqual(lscfile.read_header(io.BytesIO(blob)),
                         (1700000000123, 6, 7, 500000))

    def test_iter_records_tolerates_truncated_tail(self):
        blob = hdr() + rec(1, 1000) + b"1234567890"
        with io.BytesIO(blob) as f:
            lscfile.read_header(f)
            got = list(lscfile.iter_records(f))
        self.assertEqual(len(got), 1)
        self.assertEqual(got[0][0], 1)  # seq
        self.assertEqual(got[0][1], 1000)  # ts_ms

    def test_iter_records_clamps_dlc(self):
        blob = hdr() + rec(1, 1000, dlc=15)
        with io.BytesIO(blob) as f:
            lscfile.read_header(f)
            recs = list(lscfile.iter_records(f))
        self.assertEqual(recs[0][4], 8)

    def test_frame_valid(self):
        ok = lscfile.frame_valid
        self.assertTrue(ok(1, 1, 0x7FF, 0, 8, bytes(8)))
        self.assertTrue(ok(1, 1, 0x1FFFFFFF, 1, 8, bytes(8)))
        self.assertFalse(ok(1, 1, 0x800, 0, 8, bytes(8)))   # std id cap
        self.assertFalse(ok(1, 1, 0x20000000, 1, 8, bytes(8)))  # ext id cap
        self.assertFalse(ok(1, 1, 0x123, 8, 8, bytes(8)))   # flags & ~7
        self.assertFalse(ok(1, 1, 0x123, 0, 9, bytes(8)))   # dlc cap

    def test_candump_line(self):
        start = 1700000000000
        self.assertEqual(
            lscfile.candump_line((1, 1500, 0x123, 0, 2, b"\x11\x22"), start),
            "(1700000001.500000) can0 123#1122")
        self.assertEqual(
            lscfile.candump_line((2, 1600, 0x18DAF110, 1, 2, b"\x11\x22"), start),
            "(1700000001.600000) can0 18DAF110#1122")
        self.assertEqual(
            lscfile.candump_line((3, 1700, 0x321, 2, 3, b"\x33\x44\x55"), start),
            "(1700000001.700000) can0 321#R")
        self.assertEqual(
            lscfile.candump_line((4, 1, 0x1FFFFFFF, 1, 0, b""), 0),
            "(0.001000) can0 1FFFFFFF#")


class ExportGoldenTests(Base):
    def setUp(self):
        super().setUp()
        self.path = self.write("golden.lsc", GOLDEN)

    def export(self, fmt, *extra):
        out = self.root / "out.txt"
        rc, text, err = self.cli(["export", fmt, str(self.path), "-o", str(out),
                                  *extra])
        self.assertEqual(rc, 0, err)
        return out.read_text()

    def test_candump_golden(self):
        self.assertEqual(self.export("candump"), GOLDEN_CANDUMP)

    def test_asc_golden(self):
        self.assertEqual(self.export("asc"), GOLDEN_ASC)

    def test_csv_golden(self):
        self.assertEqual(self.export("csv"), GOLDEN_CSV)

    def test_stdout_default_matches_file(self):
        rc, text, err = self.cli(["export", "candump", str(self.path)])
        self.assertEqual(rc, 0, err)
        self.assertEqual(text, GOLDEN_CANDUMP)


class ExportFilterTests(Base):
    # Wall seconds under test (start_wall_s=1700000000, anchor 0):
    # ts 0/999 -> 1700000000, ts 1000/1999 -> 1700000001, ts 2000+ -> ...002.
    FILTER = (hdr(flags=6, start_wall_s=1700000000)
              + rec(1, 0, 0x100, 0, 1, b"\xAA")
              + rec(2, 999, 0x100, 1, 1, b"\xBB")
              + rec(3, 1000, 0x200, 0, 1, b"\xCC")
              + rec(4, 1999, 0x100, 0, 1, b"\xDD")
              + rec(5, 2000, 0x200, 1, 1, b"\xEE")
              + rec(6, 2500, 0x200, 0, 1, b"\xFF"))

    def setUp(self):
        super().setUp()
        self.path = self.write("filter.lsc", self.FILTER)

    def rows(self, *extra):
        out = self.root / "out.txt"
        rc, text, err = self.cli(["export", "csv", str(self.path), "-o",
                                  str(out), *extra])
        self.assertEqual(rc, 0, err)
        return out.read_text().splitlines()[1:]

    def seqs(self, *extra):
        return [int(row.split(",")[2]) for row in self.rows(*extra)]

    def test_id_filter_is_exact(self):
        self.assertEqual(self.seqs("--id", "0x100"), [1, 2, 4])

    def test_ext_std_filters(self):
        self.assertEqual(self.seqs("--ext"), [2, 5])
        self.assertEqual(self.seqs("--std"), [1, 3, 4, 6])

    def test_from_inclusive_to_exclusive(self):
        self.assertEqual(self.seqs("--from", "1700000000", "--to", "1700000002"),
                         [1, 2, 3, 4])
        self.assertEqual(self.seqs("--from", "1700000001"), [3, 4, 5, 6])
        self.assertEqual(self.seqs("--to", "1700000001"), [1, 2])

    def test_filters_combine(self):
        self.assertEqual(self.seqs("--id", "0x100", "--from", "1700000001"),
                         [4])

    def test_mutually_exclusive_rejected(self):
        for extra in (("--ext", "--std"), ("--std", "--ext")):
            with self.subTest(extra=extra):
                rc, _out, err = self.cli(["export", "csv", str(self.path),
                                          *extra])
                self.assertEqual(rc, 2)
                self.assertIn("mutually exclusive", err)

    def test_inverted_range_rejected(self):
        for frm, to in (("5", "5"), ("6", "5")):
            with self.subTest(frm=frm, to=to):
                rc, _out, err = self.cli(["export", "csv", str(self.path),
                                          "--from", frm, "--to", to])
                self.assertEqual(rc, 2)
                self.assertIn("--to must be greater than --from", err)

    def test_bad_args_rejected(self):
        cases = [(["export", "csv", str(self.path), "--id", "zzz"], "bad --id"),
                 (["export", "csv", str(self.path), "--from", "-1"], "bad --from"),
                 (["export", "csv", str(self.path), "--to", "x"], "bad --to"),
                 (["export", "bin", str(self.path)], "unknown format"),
                 (["export", "csv", str(self.path), "--bogus"], "usage:"),
                 (["export", "csv"], "usage:"),
                 (["export"], "usage:")]
        for argv, want in cases:
            with self.subTest(argv=argv):
                rc, _out, err = self.cli(argv)
                self.assertEqual(rc, 2)
                self.assertIn(want, err)

    def test_refuses_to_overwrite_input(self):
        rc, _out, err = self.cli(["export", "csv", str(self.path), "-o",
                                  str(self.path)])
        self.assertEqual(rc, 2)
        self.assertIn("Invalid argument", err)


class UptimeOnlyTests(Base):
    UPTIME = (hdr(flags=5, start_wall_s=0, boot=42)   # TS_UPTIME_ONLY|BOOT_VALID
              + rec(0, 1000)
              + rec(1, 1100)
              + rec(2, 1200))

    def setUp(self):
        super().setUp()
        self.path = self.write("uptime.lsc", self.UPTIME)

    def test_time_filter_needs_wall_anchor(self):
        rc, _out, err = self.cli(["export", "csv", str(self.path),
                                  "--from", "1700000000"])
        self.assertEqual(rc, 2)
        self.assertIn("import", err)

    def test_time_filter_to_also_rejected(self):
        rc, _out, err = self.cli(["export", "candump", str(self.path),
                                  "--to", "1700000001"])
        self.assertEqual(rc, 2)

    def test_uptime_export_without_filter(self):
        rc, out, err = self.cli(["export", "candump", str(self.path)])
        self.assertEqual(rc, 0, err)
        self.assertEqual(out, "(1.000000) can0 123#1122\n"
                              "(1.100000) can0 123#1122\n"
                              "(1.200000) can0 123#1122\n")


class VerifyTests(Base):
    def verify(self, blob, *extra):
        path = self.write("v.lsc", blob)
        return self.cli(["verify", str(path), *extra])

    def summary(self, out):
        return out.splitlines()[0]

    def test_clean_file(self):
        blob = hdr() + rec(1, 1000) + rec(2, 1100) + rec(3, 1200)
        for extra in ((), ("--strict",)):
            with self.subTest(extra=extra):
                rc, out, err = self.verify(blob, *extra)
                self.assertEqual(rc, 0, err)
                self.assertEqual(
                    self.summary(out),
                    "verify: records=3 boot=7 bitrate=500000 flags=0x6 "
                    "uptime_only=0 gaps=0 missing=0 backwards=0 dup=0 "
                    "ambiguous=0 truncated=0")

    def test_counts_gaps_missing(self):
        blob = hdr() + rec(1, 1000) + rec(2, 1100) + rec(4, 1200)
        rc, out, err = self.verify(blob)
        self.assertEqual(rc, 0)
        self.assertIn("gaps=1 missing=1", self.summary(out))
        rc, out, err = self.verify(blob, "--strict")
        self.assertEqual(rc, 1)
        self.assertIn("gaps=1 missing=1", err)

    def test_counts_duplicates(self):
        blob = hdr() + rec(1, 1000) + rec(1, 1000)
        rc, out, err = self.verify(blob, "--strict")
        self.assertEqual(rc, 1)
        self.assertIn("records=2", self.summary(out))
        self.assertIn("dup=1", self.summary(out))

    def test_counts_backwards(self):
        blob = hdr() + rec(1, 1000) + rec(2, 900)
        rc, out, err = self.verify(blob, "--strict")
        self.assertEqual(rc, 1)
        self.assertIn("backwards=1", self.summary(out))
        self.assertIn("dup=1", self.summary(out))  # ts rollback is skipped

    def test_counts_ambiguous(self):
        for blob in (hdr() + rec(1, 1000) + rec(1, 1100),   # diff 0 across ts
                     hdr() + rec(5, 1000) + rec(3, 1100)):  # diff < 0
            with self.subTest(blob=blob[32:44]):
                rc, out, err = self.verify(blob)
                self.assertEqual(rc, 0)
                self.assertIn("ambiguous=1", self.summary(out))
                # ambiguous alone never fails --strict
                rc, _out, err = self.verify(blob, "--strict")
                self.assertEqual(rc, 0)

    def test_seq_wrap_not_a_gap(self):
        blob = hdr() + rec(0xFFFF, 1000) + rec(0, 1100)
        rc, out, err = self.verify(blob, "--strict")
        self.assertEqual(rc, 0, err)
        self.assertIn("gaps=0 missing=0", self.summary(out))

    def test_same_ms_sequence_rules(self):
        # Consecutive seq in one millisecond: accepted, nothing counted.
        rc, out, _ = self.verify(hdr() + rec(1, 1000) + rec(2, 1000), "--strict")
        self.assertEqual(rc, 0)
        self.assertIn("records=2", self.summary(out))
        # diff -1023 is a reconnect backlog duplicate.
        rc, out, _ = self.verify(hdr() + rec(0, 1000) + rec(64513, 1000),
                                 "--strict")
        self.assertEqual(rc, 1)
        self.assertIn("dup=1", self.summary(out))
        # diff -1024 falls outside the skip window: accepted silently.
        rc, out, _ = self.verify(hdr() + rec(0, 1000) + rec(64512, 1000),
                                 "--strict")
        self.assertEqual(rc, 0)
        self.assertIn("dup=0", self.summary(out))

    def test_truncated_tail(self):
        blob = hdr() + rec(1, 1000) + b"1234567890"
        rc, out, err = self.verify(blob)
        self.assertEqual(rc, 0)
        self.assertIn("truncated=1", self.summary(out))
        rc, out, err = self.verify(blob, "--strict")
        self.assertEqual(rc, 1)
        self.assertIn("truncated tail record", err)
        # Truncation is reported before sequence defects.
        blob = hdr() + rec(1, 1000) + rec(3, 1200) + b"xx"
        rc, _out, err = self.verify(blob, "--strict")
        self.assertEqual(rc, 1)
        self.assertIn("truncated tail record", err)

    def test_header_errors(self):
        for blob in (b"bad header", hdr(version=2)):
            with self.subTest(blob=blob[:8]):
                rc, out, err = self.verify(blob)
                self.assertEqual(rc, 2)
                self.assertIn("records=0 boot=0 bitrate=0 flags=0x0", out)
                self.assertIn("bad header", err)
        rc, out, err = self.cli(["verify", str(self.root / "missing.lsc")])
        self.assertEqual(rc, 2)
        self.assertIn("bad header", err)


class ImportTests(Base):
    WALL = (hdr(flags=6, start_wall_s=1700000000, anchor=123, boot=7)
            + rec(0, 1000) + rec(1, 1100) + rec(2, 1200))
    UPTIME = (hdr(flags=5, start_wall_s=0, boot=42)   # TS_UPTIME_ONLY|BOOT_VALID
              + rec(0, 1000) + rec(1, 1100) + rec(2, 1200))

    def test_wall_valid_preserves_anchor(self):
        src = self.write("wall.lsc", self.WALL)
        dst = self.root / "wall-out.lsc"
        rc, out, err = self.cli(["import", str(src), "-o", str(dst)])
        self.assertEqual(rc, 0, err)
        self.assertEqual(
            out.splitlines()[0],
            "import: 3 records, boot 7, bitrate 500000, "
            "anchor 1700000000123 ms (wall clock preserved)")
        self.assertEqual(self.read_header(dst),
                         (1700000000123, 6, 7, 500000))
        self.assertEqual(dst.read_bytes()[32:], self.WALL[32:])

    def test_boot_wall_anchors_uptime_only(self):
        src = self.write("up.lsc", self.UPTIME)
        dst = self.root / "up-out.lsc"
        rc, out, err = self.cli(["import", str(src), "-o", str(dst),
                                 "--boot-wall", "1700000000"])
        self.assertEqual(rc, 0, err)
        self.assertEqual(
            out.splitlines()[0],
            "import: 3 records, boot 42, bitrate 500000, "
            "anchor 1700000000000 ms (re-anchored)")
        self.assertEqual(self.read_header(dst),
                         (1700000000000, 6, 42, 500000))
        self.assertNotIn("warning", err)
        self.assertEqual(dst.read_bytes()[32:], self.UPTIME[32:])

    def test_mtime_fallback_anchors_uptime_only(self):
        src = self.write("up.lsc", self.UPTIME)
        os.utime(src, (1700000100, 1700000100))
        dst = self.root / "up-out.lsc"
        rc, out, err = self.cli(["import", str(src), "-o", str(dst)])
        self.assertEqual(rc, 0, err)
        # mtime 1700000100 s, last ts 1200 -> anchor 1700000100000 - 1200 ms.
        self.assertEqual(self.read_header(dst),
                         (1700000098800, 6, 42, 500000))
        self.assertIn("warning", err)
        self.assertIn("mtime", err)
        self.assertEqual(dst.read_bytes()[32:], self.UPTIME[32:])

    def test_refuses_same_path(self):
        src = self.write("same.lsc", self.UPTIME)
        rc, _out, err = self.cli(["import", str(src), "-o", str(src)])
        self.assertEqual(rc, 2)
        self.assertIn("source and destination must differ", err)

    def test_missing_dst_rejected(self):
        src = self.write("x.lsc", self.UPTIME)
        rc, _out, err = self.cli(["import", str(src)])
        self.assertEqual(rc, 2)
        self.assertIn("needs -o", err)


class SidecarTests(Base):
    UPTIME_BLOB = (hdr(flags=5, start_wall_s=0, boot=42)
                   + rec(0, 1000) + rec(1, 1100) + rec(2, 1200))

    def test_verify_prints_sidecar_summary(self):
        path = self.write("x.lsc", hdr() + rec(1, 1000))
        (self.root / "x.met").write_text('{"device_id":"AABB","boot":7}\n')
        (self.root / "x.trg").write_text('{"reason":"host"}\n{"reason":"ui"}\n')
        rc, out, err = self.cli(["verify", str(path)])
        self.assertEqual(rc, 0, err)
        self.assertIn('met: {"device_id":"AABB","boot":7}', out)
        self.assertIn("trg: 2 mark(s)", out)

    def test_no_sidecars_no_lines(self):
        path = self.write("x.lsc", hdr() + rec(1, 1000))
        rc, out, err = self.cli(["verify", str(path)])
        self.assertEqual(rc, 0, err)
        self.assertNotIn("met:", out)
        self.assertNotIn("trg:", out)

    def test_import_summarizes_src_sidecars(self):
        src = self.write("src.lsc", self.UPTIME_BLOB)
        (self.root / "src.met").write_text('{"device_id":"ZZ"}\n')
        dst = self.root / "dst.lsc"
        rc, out, err = self.cli(["import", str(src), "-o", str(dst),
                                 "--boot-wall", "1"])
        self.assertEqual(rc, 0, err)
        self.assertIn('met: {"device_id":"ZZ"}', out)

    def test_sidecar_path_strips_lsc_suffix_only(self):
        self.assertEqual(lsc.sidecar_path("a.lsc", "met"), "a.met")
        self.assertEqual(lsc.sidecar_path("dir/b.lsc", "trg"), "dir/b.trg")
        self.assertEqual(lsc.sidecar_path("a", "met"), "a.met")
        self.assertEqual(lsc.sidecar_path(".lsc", "met"), ".lsc.met")


class SmokeTests(Base):
    """Process-level checks: sys.exit(main()) really sets exit codes."""

    def test_subprocess_verify_and_time_filter_exit_codes(self):
        good = self.write("good.lsc", hdr() + rec(1, 1000) + rec(2, 1100))
        result = subprocess.run([sys.executable, str(LSC), "verify", str(good)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("records=2", result.stdout)
        up = self.write("up.lsc", hdr(flags=5, start_wall_s=0) + rec(0, 1000))
        result = subprocess.run([sys.executable, str(LSC), "export", "csv",
                                 str(up), "--from", "5"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("import", result.stderr)


if __name__ == "__main__":
    unittest.main()
