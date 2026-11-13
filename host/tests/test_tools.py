#!/usr/bin/env python3
"""Offline soak verifier and USB detach regressions; never opens CAN/devices."""

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools"


def load_tool(name):
    spec = importlib.util.spec_from_file_location(name, TOOLS / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


can_soak = load_tool("can_soak")


def header(version=1):
    return struct.pack("<4sHIIHQII", b"LSC1", version, 500000, 0, 0, 0, 0, 0)


def record(counter, ident=0x123, flags=0, dlc=8, pattern=can_soak.PATTERN):
    return can_soak.LSC_REC.pack(counter & 0xFFFF, counter & 0xFFFFFFFF, ident,
                                 flags, dlc, (counter & 0xFFFFFFFF).to_bytes(4, "little") + pattern)


class ToolTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="linsight-tools-test-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def write(self, name, blob):
        path = self.root / name
        path.write_bytes(blob)
        return path

    def test_rejects_header_tail_and_payload_corruption(self):
        cases = [b"LSC1", header(2), header() + b"x", header(),
                 header() + record(0, ident=0x456),
                 header() + record(0, pattern=bytes(4)),
                 header() + record(0, flags=2),
                 header() + record(0, flags=1),
                 header() + record(0, dlc=7),
                 header() + record(0) + record(2)]
        for index, blob in enumerate(cases):
            with self.subTest(index=index):
                path = self.write("bad.lsc", blob)
                with self.assertRaises(ValueError):
                    can_soak.verify_file(path, 0x123)

    def test_streams_and_counts_only_matching_frames(self):
        blob = header() + record(0, ident=0x456) + record(0) + record(1)

        class BoundedRead(io.BytesIO):
            def read(self, size=-1):
                if size < 0:
                    raise AssertionError("unbounded file read")
                return super().read(size)

        with mock.patch("builtins.open", return_value=BoundedRead(blob)):
            result = can_soak.verify_file("memory", 0x123, expect=2)
        self.assertEqual(result, {"records": 3, "matched": 2, "next": 2})
        path = self.write("valid.lsc", blob)
        with self.assertRaisesRegex(ValueError, "matched=2 expected=3"):
            can_soak.verify_file(path, 0x123, expect=3)

    def test_extended_and_counter_wrap(self):
        path = self.write("ext.lsc", header() + record(0xFFFFFFFF, flags=1) + record(0, flags=1))
        result = can_soak.verify_file(path, 0x123, ext=True, first=0xFFFFFFFF, expect=2)
        self.assertEqual(result["next"], 1)
        self.assertEqual(can_soak.build_frame(0x100000000, 0x123)[8:12], bytes(4))

    def test_generator_emits_actual_sent_result_without_can(self):
        fake_socket = mock.Mock()
        args = SimpleNamespace(iface="unused", rate=1000, duration=0.002,
                               id=0x123, ext=False, record=None)
        output = io.StringIO()
        with mock.patch.object(socket, "socket", return_value=fake_socket), \
                mock.patch.object(signal, "signal"), contextlib.redirect_stdout(output):
            self.assertEqual(can_soak.tx(args), 0)
        result_line = next(line for line in output.getvalue().splitlines() if line.startswith("result: "))
        self.assertEqual(json.loads(result_line[8:]), {"sent": 2, "errors": 0, "completed": True})
        self.assertEqual(fake_socket.send.call_count, 2)

    def test_detach_only_requested_busid_with_fake_commands(self):
        calls = self.root / "calls"
        stub = self.root / "usbipd.exe"
        stub.write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "$CALLS"\n')
        stub.chmod(0o700)
        script = (TOOLS / "can-down.sh").read_text().replace(
            'USBIPD="/mnt/c/Program Files/usbipd-win/usbipd.exe"', f'USBIPD="{stub}"')
        # Every external command is fake; no adapter/interface/service is touched.
        script = "ip() { return 1; }\nsudo() { return 97; }\n" + script
        env = dict(os.environ, CALLS=str(calls))
        for args, expected in [([], "detach --busid 2-1"), (["9-8"], "detach --busid 9-8")]:
            if calls.exists():
                calls.unlink()
            subprocess.run(["bash", "-s", "--", *args], input=script, text=True,
                           capture_output=True, env=env, check=True)
            self.assertEqual(calls.read_text().splitlines(), [expected])


if __name__ == "__main__":
    unittest.main()
