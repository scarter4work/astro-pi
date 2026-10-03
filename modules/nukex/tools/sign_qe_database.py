#!/usr/bin/env python3
"""Sign and publish the QE camera database.

Produces the four artifacts the in-module updater fetches:

    repository/qe_database.json
    repository/qe_database.json.sig
    repository/qe_manifest.json
    repository/qe_manifest.json.sig

Both files are signed. Signing only the database would leave the manifest
forgeable, and the manifest is what the client trusts for the version number
and for the summary shown in the consent prompt -- an attacker able to serve
files could otherwise pin clients to an old version indefinitely.

The private key lives outside this repository (beside the .xssk signing keys)
and is never committed. Generate one with:

    python3 -c "
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    from cryptography.hazmat.primitives import serialization as s
    import os, stat, binascii, pathlib
    k = Ed25519PrivateKey.generate()
    p = pathlib.Path('~/projects/keys/nukex_qe_signing.key').expanduser()
    p.write_bytes(k.private_bytes(s.Encoding.Raw, s.PrivateFormat.Raw, s.NoEncryption()))
    os.chmod(p, stat.S_IRUSR | stat.S_IWUSR)
    print(binascii.hexlify(k.public_key().public_bytes(s.Encoding.Raw, s.PublicFormat.Raw)).decode())
    "

then embed the printed public key in kQEPublicKey in
src/lib/calibration/src/qe_update.cpp.

Usage:
    python3 tools/sign_qe_database.py share/qe_database.json \
        --key ~/projects/keys/nukex_qe_signing.key \
        --db-version 1 \
        --summary "Initial published camera database." \
        --out repository/
"""
import argparse
import base64
import hashlib
import json
import pathlib
import sys
from datetime import datetime, timezone

SCHEMA_VERSION = 1

try:
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
except ImportError:
    sys.exit("error: python3 -m pip install cryptography")


def load_key(path: pathlib.Path) -> Ed25519PrivateKey:
    seed = path.read_bytes()
    if len(seed) != 32:
        sys.exit(f"error: {path} is {len(seed)} bytes; an Ed25519 seed is 32")
    return Ed25519PrivateKey.from_private_bytes(seed)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("database", type=pathlib.Path,
                    help="the shipped share/qe_database.json to publish")
    ap.add_argument("--key", type=pathlib.Path, required=True,
                    help="Ed25519 private key seed (32 raw bytes)")
    ap.add_argument("--db-version", type=int, required=True,
                    help="monotonically increasing; clients refuse a decrease")
    ap.add_argument("--summary", required=True,
                    help="shown verbatim in the consent prompt")
    ap.add_argument("--out", type=pathlib.Path, default=pathlib.Path("repository"))
    args = ap.parse_args()

    body = args.database.read_bytes()

    # Parse before signing: publishing a database the engine cannot load
    # would be discovered only by users, and only after they accepted it.
    try:
        doc = json.loads(body)
    except json.JSONDecodeError as e:
        return fail(f"{args.database} is not valid JSON: {e}")

    for required in ("schema_version", "cameras", "filters"):
        if required not in doc:
            return fail(f"{args.database} has no top-level '{required}'")

    if doc["schema_version"] != SCHEMA_VERSION:
        return fail(f"database schema_version is {doc['schema_version']}, "
                    f"but this tool publishes schema {SCHEMA_VERSION}")

    cameras = doc["cameras"]
    sensors = {c.get("sensor") for c in cameras.values() if c.get("sensor")}

    key = load_key(args.key)
    args.out.mkdir(parents=True, exist_ok=True)

    manifest = {
        "schema_version": SCHEMA_VERSION,
        "db_version": args.db_version,
        "db_sha512": hashlib.sha512(body).hexdigest(),
        "db_bytes": len(body),
        "n_cameras": len(cameras),
        "n_sensors": len(sensors),
        "released_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "summary": args.summary,
    }
    # separators without spaces keeps the signed bytes stable across Python
    # versions; the signature covers exactly what is written.
    manifest_bytes = json.dumps(manifest, separators=(",", ":"),
                                sort_keys=True).encode()

    written = []
    for name, payload in (("qe_database.json", body),
                          ("qe_manifest.json", manifest_bytes)):
        (args.out / name).write_bytes(payload)
        sig = base64.b64encode(key.sign(payload)).decode()
        (args.out / (name + ".sig")).write_text(sig + "\n")
        written += [args.out / name, args.out / (name + ".sig")]

    print(f"db_version   {args.db_version}")
    print(f"cameras      {len(cameras)}")
    print(f"sensors      {len(sensors)}")
    print(f"sha512       {manifest['db_sha512'][:32]}...")
    for p in written:
        print(f"wrote        {p}")
    print()
    print("Verify with the module's own verifier before publishing -- a base64 or")
    print("byte-order disagreement between this tool and the C++ side would")
    print("otherwise ship undetected and reject every update in the field.")
    return 0


def fail(msg: str) -> int:
    print(f"error: {msg}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
