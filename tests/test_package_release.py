import importlib.util
from pathlib import Path
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("package_release", ROOT / "tools/package_release.py")
release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release)


class PackageReleaseTests(unittest.TestCase):
    def test_sensitive_and_generated_paths_are_excluded(self):
        for name in (
            ".env.local", "relay.local.json", "wifi.local.json", "sdkconfig.multihop_c6_a",
            "android/local.properties", "firmware/esp32/wifi_credentials.h",
            "server/relay.local.json", "tools/__pycache__/flash_multihop.pyc",
            "captures_out/session.pcap", "dist/previous.zip", "build/native/cache",
        ):
            with self.subTest(name=name):
                self.assertFalse(release.allowed(name))

    def test_archive_contains_prebuilt_images_and_no_companion_sources(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "RetroPicto.zip"
            count, _digest = release.package(output)
            self.assertGreater(count, 20)
            with zipfile.ZipFile(output) as archive:
                names = set(archive.namelist())
                self.assertIn("RetroPicto/firmware/prebuilt/manifest.json", names)
                self.assertIn("RetroPicto/firmware/prebuilt/multihop_c6_a/firmware.bin", names)
                self.assertIn("RetroPicto/python/pictochat/message.py", names)
                self.assertFalse(any("/android/" in name or "/server/" in name for name in names))
                self.assertNotIn("RetroPicto/relay.local.json", names)
                self.assertIn("RetroPicto/SHA256SUMS.txt", names)
                self.assertIsNone(archive.testzip())


if __name__ == "__main__":
    unittest.main()
