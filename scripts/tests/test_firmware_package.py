"""Host-side tests for the firmware repository packaging/signing tool.

Run: python -m unittest discover -s scripts/tests -v
Requires the `cryptography` package (present in the ESP-IDF venv).
"""
import hashlib
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "firmware_package.py"
APP_DESC_OFFSET = 0x20
APP_DESC_MAGIC = 0xABCD5432


def make_image(version: str = "9.9.9", project: str = "xiaozhi", payload: bytes = b"") -> bytes:
    """Build a minimal ESP image with a valid app descriptor for the tool to parse."""
    desc = bytearray(256)
    struct.pack_into("<I", desc, 0, APP_DESC_MAGIC)
    desc[16:16 + len(version)] = version.encode()
    desc[48:48 + len(project)] = project.encode()
    header = bytearray(APP_DESC_OFFSET)
    header[0] = 0xE9  # image magic
    return bytes(header) + bytes(desc) + payload


def run_tool(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args],
        capture_output=True, text=True,
    )


class FirmwarePackageTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.keydir = self.tmp / "keys"
        self.pubheader = self.tmp / "firmware_pubkey.h"
        r = run_tool("genkey", "--out", str(self.keydir),
                     "--pubkey-header", str(self.pubheader), "--key-id", "7")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.privkey = self.keydir / "signing.key.pem"
        self.assertTrue(self.privkey.exists())

    def tearDown(self):
        self._tmp.cleanup()

    def test_genkey_pubkey_header(self):
        text = self.pubheader.read_text(encoding="utf-8")
        self.assertIn("#define METAOS_FW_KEY_ID 7", text)
        self.assertIn("#define METAOS_FW_PUBKEY_PROVISIONED 1", text)
        self.assertIn("kMetaosFwPublicKey[64]", text)
        # 64 hex bytes present
        self.assertEqual(text.count("0x"), 64)
        # private key is PEM EC
        self.assertIn("PRIVATE KEY", self.privkey.read_text(encoding="utf-8"))

    def test_package_unsigned(self):
        binp = self.tmp / "fw.bin"
        binp.write_bytes(make_image(payload=b"hello firmware"))
        out = self.tmp / "repo" / "stable"
        r = run_tool("package", "--bin", str(binp), "--out", str(out),
                     "--target-board", "lichuang-dev", "--channel", "stable")
        self.assertEqual(r.returncode, 0, r.stderr)
        manifest = json.loads((out / "manifest.json").read_text())
        self.assertEqual(manifest["firmware_size"], binp.stat().st_size)
        self.assertEqual(manifest["firmware_sha256"],
                         hashlib.sha256(binp.read_bytes()).hexdigest())
        self.assertEqual(manifest["version"], "9.9.9")  # from app descriptor
        self.assertEqual(manifest["target_board"], "lichuang-dev")
        self.assertFalse((out / "manifest.json.sig").exists())
        self.assertTrue((out / "fw.bin").exists())

    def test_sign_verify_roundtrip(self):
        binp = self.tmp / "fw.bin"
        binp.write_bytes(make_image(payload=b"signed firmware bytes"))
        out = self.tmp / "repo" / "stable"
        r = run_tool("package", "--bin", str(binp), "--key", str(self.privkey),
                     "--key-id", "7", "--out", str(out))
        self.assertEqual(r.returncode, 0, r.stderr)
        sig = (out / "manifest.json.sig").read_bytes()
        self.assertEqual(len(sig), 64)  # raw r||s
        v = run_tool("verify", "--dir", str(out), "--key", str(self.privkey))
        self.assertEqual(v.returncode, 0, v.stdout + v.stderr)

    def test_verify_rejects_tampered_manifest(self):
        binp = self.tmp / "fw.bin"
        binp.write_bytes(make_image(payload=b"abc"))
        out = self.tmp / "repo" / "stable"
        run_tool("package", "--bin", str(binp), "--key", str(self.privkey), "--out", str(out))
        # Tamper with the manifest after signing.
        mpath = out / "manifest.json"
        m = json.loads(mpath.read_text())
        m["description"] = "tampered"
        mpath.write_text(json.dumps(m))
        v = run_tool("verify", "--dir", str(out), "--key", str(self.privkey))
        self.assertNotEqual(v.returncode, 0)

    def test_verify_rejects_tampered_bin(self):
        binp = self.tmp / "fw.bin"
        binp.write_bytes(make_image(payload=b"original"))
        out = self.tmp / "repo" / "stable"
        run_tool("package", "--bin", str(binp), "--key", str(self.privkey), "--out", str(out))
        # Replace the stored bin with different content (hash no longer matches).
        (out / "fw.bin").write_bytes(make_image(payload=b"tampered"))
        v = run_tool("verify", "--dir", str(out), "--key", str(self.privkey))
        self.assertNotEqual(v.returncode, 0)

    def test_package_rejects_non_image(self):
        binp = self.tmp / "notimage.bin"
        binp.write_bytes(b"this is not an ESP image")
        out = self.tmp / "repo"
        r = run_tool("package", "--bin", str(binp), "--out", str(out))
        self.assertNotEqual(r.returncode, 0)


if __name__ == "__main__":
    unittest.main()
