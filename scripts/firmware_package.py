#!/usr/bin/env python3
"""MetaOS firmware repository packaging and signing tool.

Subcommands:
  genkey   Generate an ECDSA P-256 signing keypair. The PRIVATE key is written to
           the chosen output directory (keep it secret, never commit it). The
           PUBLIC key is emitted as a C header for embedding in the firmware.
  package  Build a manifest.json (+ detached manifest.json.sig) for a firmware
           .bin, computing SHA-256/size and reading the app descriptor version.
  verify   Verify a manifest signature against a public key (self-test).

The device verifies the detached signature over the exact manifest.json bytes
using the embedded public key (PSA ECDSA P-256 / SHA-256). The manifest's
firmware_sha256 then chains trust to the .bin. SHA-256 alone proves integrity,
not publisher identity; the signature is what authenticates the publisher.

Example:
  python scripts/firmware_package.py genkey --out ~/metaos-keys \
      --pubkey-header main/firmware/firmware_pubkey.h --key-id 1
  python scripts/firmware_package.py package --bin build/xiaozhi.bin \
      --key ~/metaos-keys/signing.key.pem --key-id 1 \
      --firmware-id metaos --name "MetaOS" --version 2.5.3 \
      --target-board lichuang-dev --channel stable --out /sdcard/metaos/firmware/stable
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import (
    decode_dss_signature,
    encode_dss_signature,
)

APP_DESC_OFFSET = 0x20
APP_DESC_MAGIC = 0xABCD5432


def load_private_key(path: Path) -> ec.EllipticCurvePrivateKey:
    key = serialization.load_pem_private_key(path.read_bytes(), password=None)
    if not isinstance(key, ec.EllipticCurvePrivateKey):
        raise SystemExit("key is not an EC private key")
    if key.curve.name != "secp256r1":
        raise SystemExit("key must be P-256 (secp256r1)")
    return key


def public_raw_xy(key: ec.EllipticCurvePrivateKey) -> bytes:
    nums = key.public_key().public_numbers()
    return nums.x.to_bytes(32, "big") + nums.y.to_bytes(32, "big")


def read_app_desc(bin_path: Path) -> dict:
    data = bin_path.read_bytes()
    if len(data) < APP_DESC_OFFSET + 256 or data[0] != 0xE9:
        raise SystemExit("not a valid ESP image")
    magic = struct.unpack_from("<I", data, APP_DESC_OFFSET)[0]
    if magic != APP_DESC_MAGIC:
        raise SystemExit("app descriptor magic invalid")
    # esp_app_desc_t: magic(4) secure_version(4) reserv(4) version[32] project_name[32] ...
    version = data[APP_DESC_OFFSET + 16:APP_DESC_OFFSET + 48].split(b"\0")[0].decode()
    project = data[APP_DESC_OFFSET + 48:APP_DESC_OFFSET + 80].split(b"\0")[0].decode()
    return {"version": version, "project_name": project}


def cmd_genkey(args) -> int:
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    key = ec.generate_private_key(ec.SECP256R1())
    priv_path = out / "signing.key.pem"
    priv_path.write_bytes(
        key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption(),
        )
    )
    try:
        priv_path.chmod(0o600)
    except OSError:
        pass
    xy = public_raw_xy(key)
    header = Path(args.pubkey_header)
    header.parent.mkdir(parents=True, exist_ok=True)
    hexbytes = [f"0x{b:02x}" for b in xy]
    # Wrap by whole bytes (12 per line), never split a hex token.
    lines = [", ".join(hexbytes[i:i + 12]) for i in range(0, len(hexbytes), 12)]
    header.write_text(
        "#pragma once\n"
        "#include <stdint.h>\n\n"
        "// ECDSA P-256 public key (raw X||Y, 64 bytes) for firmware manifest\n"
        "// verification. PUBLIC key only — the private key is never stored here.\n"
        f"// Key id: {args.key_id}. Generate a production key with:\n"
        "//   python scripts/firmware_package.py genkey --out <secret-dir> ...\n"
        f"#define METAOS_FW_KEY_ID {args.key_id}\n"
        "#define METAOS_FW_PUBKEY_PROVISIONED 1\n\n"
        "static const uint8_t kMetaosFwPublicKey[64] = {\n    "
        + ",\n    ".join(lines)
        + "\n};\n",
        encoding="utf-8",
    )
    print(f"private key: {priv_path}  (KEEP SECRET, do not commit)")
    print(f"public header: {header}  (key id {args.key_id})")
    return 0


def cmd_package(args) -> int:
    bin_path = Path(args.bin)
    data = bin_path.read_bytes()
    sha = hashlib.sha256(data).hexdigest()
    desc = read_app_desc(bin_path)
    version = args.version or desc["version"]
    bin_name = args.bin_name or bin_path.name
    manifest = {
        "manifest_version": 1,
        "firmware_id": args.firmware_id,
        "firmware_name": args.name,
        "version": version,
        "build_number": args.build_number,
        "target_chip": args.target_chip,
        "target_board": args.target_board,
        "min_bootloader_version": args.min_bootloader,
        "firmware_size": len(data),
        "firmware_sha256": sha,
        "file": bin_name,
        "release_channel": args.channel,
        "release_time": args.release_time or "",
        "description": args.description or "",
        "rollback_supported": True,
        "key_id": args.key_id,
        # Only advertise a signature when actually signing; an empty field means
        # unsigned (the device then allows it unless REQUIRE_SIGNATURE is set).
        "signature": "manifest.json.sig" if args.key else "",
    }
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    manifest_bytes = (json.dumps(manifest, indent=2, ensure_ascii=False) + "\n").encode("utf-8")
    (out / "manifest.json").write_bytes(manifest_bytes)
    # Copy the bin into the repository directory under bin_name.
    target_bin = out / bin_name
    if target_bin.resolve() != bin_path.resolve():
        target_bin.write_bytes(data)
    if args.key:
        key = load_private_key(Path(args.key))
        der = key.sign(manifest_bytes, ec.ECDSA(hashes.SHA256()))
        r, s = decode_dss_signature(der)
        sig = r.to_bytes(32, "big") + s.to_bytes(32, "big")
        (out / "manifest.json.sig").write_bytes(sig)
        print(f"signed manifest -> {out / 'manifest.json'} (+ .sig)")
    else:
        print(f"UNSIGNED manifest -> {out / 'manifest.json'} (no --key given)")
    print(f"  {bin_name}: {len(data)} bytes, sha256={sha}, version={version}")
    return 0


def cmd_verify(args) -> int:
    out = Path(args.dir)
    manifest_bytes = (out / "manifest.json").read_bytes()
    sig = (out / "manifest.json.sig").read_bytes()
    if len(sig) != 64:
        raise SystemExit("signature must be 64 bytes (r||s)")
    key = load_private_key(Path(args.key)).public_key()
    r = int.from_bytes(sig[:32], "big")
    s = int.from_bytes(sig[32:], "big")
    der = encode_dss_signature(r, s)
    try:
        key.verify(der, manifest_bytes, ec.ECDSA(hashes.SHA256()))
    except Exception as e:  # noqa: BLE001
        print(f"INVALID signature: {e}")
        return 1
    manifest = json.loads(manifest_bytes)
    bin_path = out / manifest["file"]
    actual = hashlib.sha256(bin_path.read_bytes()).hexdigest()
    if actual != manifest["firmware_sha256"]:
        print("signature OK but firmware SHA-256 MISMATCH")
        return 1
    print("signature OK and firmware SHA-256 matches")
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    g = sub.add_parser("genkey", help="generate a P-256 signing keypair")
    g.add_argument("--out", required=True, help="directory for the PRIVATE key (keep secret)")
    g.add_argument("--pubkey-header", required=True, help="path of the C public-key header to write")
    g.add_argument("--key-id", type=int, default=1)
    g.set_defaults(func=cmd_genkey)

    pk = sub.add_parser("package", help="build (+sign) a firmware manifest")
    pk.add_argument("--bin", required=True)
    pk.add_argument("--bin-name", help="name to store the bin under (default: source name)")
    pk.add_argument("--key", help="private key PEM (omit for unsigned)")
    pk.add_argument("--key-id", type=int, default=1)
    pk.add_argument("--firmware-id", default="metaos")
    pk.add_argument("--name", default="MetaOS")
    pk.add_argument("--version", help="override version (default: from app descriptor)")
    pk.add_argument("--build-number", type=int, default=0)
    pk.add_argument("--target-chip", default="esp32s3")
    pk.add_argument("--target-board", default="lichuang-dev")
    pk.add_argument("--min-bootloader", default="")
    pk.add_argument("--channel", default="stable")
    pk.add_argument("--release-time", default="")
    pk.add_argument("--description", default="")
    pk.add_argument("--out", required=True, help="repository channel directory")
    pk.set_defaults(func=cmd_package)

    v = sub.add_parser("verify", help="verify a manifest signature + bin hash")
    v.add_argument("--dir", required=True)
    v.add_argument("--key", required=True, help="private key PEM (uses its public part)")
    v.set_defaults(func=cmd_verify)

    args = p.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
