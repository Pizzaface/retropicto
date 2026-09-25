import contextlib
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import flash_multihop


class FlashHelperTests(unittest.TestCase):
    def test_all_bundled_images_match_manifest_hashes_and_sizes(self):
        manifest = json.loads(flash_multihop.MANIFEST.read_text(encoding="utf-8"))
        self.assertEqual(len(manifest["entries"]), 6)
        for entry in manifest["entries"]:
            with self.subTest(environment=entry["environment"]):
                selected, folder = flash_multihop.load_entry(entry["board_key"], entry["role"])
                self.assertEqual(selected["chip"], entry["chip"])
                for image in selected["files"].values():
                    data = (folder / image["file"]).read_bytes()
                    self.assertEqual(hashlib.sha256(data).hexdigest(), image["sha256"])
                    self.assertEqual(len(data), image["size_bytes"])

    def test_default_preview_never_opens_serial_or_runs_esptool(self):
        output = io.StringIO()
        with patch.object(sys, "argv", ["flash_multihop.py", "--board", "c6", "--role", "a"]), \
             patch.object(flash_multihop.subprocess, "run") as run, \
             contextlib.redirect_stdout(output):
            self.assertEqual(flash_multihop.main(), 0)
        run.assert_not_called()
        self.assertIn("no serial port was opened", output.getvalue())

    def test_flash_id_mismatch_stops_before_write(self):
        entry, _ = flash_multihop.load_entry("c6", "a")
        with patch.object(flash_multihop.subprocess, "run", return_value=subprocess.CompletedProcess(
            args=[], returncode=2, stdout="", stderr="This chip is esp32, not esp32c6"
        )) as run:
            with self.assertRaisesRegex(RuntimeError, "Chip identification failed"):
                flash_multihop.detected_flash_mb(entry, "PORT")
        self.assertIn("esp32c6", run.call_args.args[0])
        self.assertEqual(run.call_count, 1)

    def test_undersized_flash_is_rejected(self):
        entry, _ = flash_multihop.load_entry("s3", "b")
        output = "Detected flash size: 4MB\n"
        with patch.object(flash_multihop.subprocess, "run", return_value=subprocess.CompletedProcess(
            args=[], returncode=0, stdout=output, stderr=""
        )):
            with self.assertRaisesRegex(RuntimeError, "needs at least"):
                flash_multihop.detected_flash_mb(entry, "PORT")


if __name__ == "__main__":
    unittest.main()
