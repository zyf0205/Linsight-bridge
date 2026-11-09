#!/usr/bin/env python3
"""CLI regressions against private fake sockets, never a running daemon."""

from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest

CLI = sys.argv.pop(1) if len(sys.argv) > 1 else None


class CliTests(unittest.TestCase):
    def test_live_survives_idle_then_receives_frame(self):
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            listener.settimeout(3)
            proc = subprocess.Popen([CLI, "live", "--ctl-port", str(listener.getsockname()[1])],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                connection, _ = listener.accept()
                with connection:
                    self.assertEqual(connection.recv(128), b"live\n")
                    # Exceed the old SO_RCVTIMEO (five seconds).
                    time.sleep(5.4)
                    self.assertIsNone(proc.poll())
                    connection.sendall(b"(1.000000) can0 123#11\n")
                out, err = proc.communicate(timeout=3)
                self.assertEqual(proc.returncode, 0, err)
                self.assertEqual(out, "(1.000000) can0 123#11\n")
            finally:
                if proc.poll() is None:
                    proc.kill()
                proc.communicate()

    def test_live_reports_connection_reset(self):
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            listener.settimeout(3)
            proc = subprocess.Popen([CLI, "live", "--ctl-port", str(listener.getsockname()[1])],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                connection, _ = listener.accept()
                with connection:
                    self.assertEqual(connection.recv(128), b"live\n")
                    connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
                _, err = proc.communicate(timeout=3)
                self.assertNotEqual(proc.returncode, 0)
                self.assertIn("live read failed", err)
            finally:
                if proc.poll() is None:
                    proc.kill()
                proc.communicate()

    def test_verify_failure_prints_initialized_stats(self):
        with tempfile.TemporaryDirectory(prefix="linsight-cli-test-") as tmp:
            invalid = Path(tmp) / "bad.lsc"
            invalid.write_bytes(b"bad header")
            for path in (invalid, Path(tmp) / "missing.lsc"):
                with self.subTest(path=path):
                    result = subprocess.run([CLI, "verify", str(path)], capture_output=True,
                                            text=True, timeout=3)
                    self.assertEqual(result.returncode, 2)
                    self.assertIn("records=0 boot=0 bitrate=0 flags=0x0", result.stdout)
                    self.assertIn("gaps=0 missing=0 backwards=0 dup=0 ambiguous=0", result.stdout)
                    self.assertIn("bad header", result.stderr)


if __name__ == "__main__":
    if CLI is None:
        sys.exit("usage: test_cli.py <linsight-cli>")
    unittest.main()
