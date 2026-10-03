"""STAGED ONLY: create independent GUID goldens from the pinned public salt.

Do not run during a timing/other exclusive fixture window. This uses Python's
PBKDF2 oracle and never imports the adapted identity implementation. It writes
only new staged files, refuses differing existing output, and is not a native
behavior test or an official authentication oracle.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "upstream/cod4x_server/src/sapi.c"
OUTPUT = ROOT / "config/staged/identity_guid_goldens.inc"
METADATA = ROOT / "config/staged/identity_guid_goldens.json"
VECTORS = (
    "00000000000000000000000000000000",
    "ffffffffffffffffffffffffffffffff",
    "000102030405060708090a0b0c0d0e0f",
    "0f0e0d0c0b0a09080706050403020100",
    "5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a",
    "a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5",
    "0123456789abcdef0123456789abcdef",
)


def immutable_create(path: Path, payload: bytes) -> None:
    if path.exists():
        if path.read_bytes() != payload:
            raise RuntimeError(f"Refusing differing existing staged oracle: {path}")
        return
    with path.open("xb") as stream:
        stream.write(payload)


def main() -> None:
    source = SOURCE.read_bytes()
    match = re.search(rb"static\s+const\s+unsigned\s+char\s+playerid_salt\[\]\s*=\s*\{([^}]+)\}", source)
    if not match:
        raise RuntimeError("Pinned public GUID salt declaration missing")
    salt = bytes(int(value, 16) for value in re.findall(rb"0x([0-9a-fA-F]{2})", match[1]))
    if len(salt) != 256:
        raise RuntimeError(f"Unexpected pinned salt length {len(salt)}")
    records = []
    for text in VECTORS:
        derived = hashlib.pbkdf2_hmac("sha256", bytes.fromhex(text), salt, 100, dklen=4)
        account = int.from_bytes(derived, "little")
        player = (32 << 56) | (1 << 52) | (1 << 32) | account
        records.append({"guid": text, "derived_hex": derived.hex(), "account": account, "expected": player})
    # This pre-existing golden was independently derived with .NET in the
    # identity suite; disagreement blocks output, rather than updating it.
    if records[-1]["expected"] != 2310346614158149229:
        raise RuntimeError("Independent pre-existing .NET golden disagreement")
    header = "/* Independent Python PBKDF2-SHA256/100 golden data; staged, not proof. */\n"
    header += "static const struct identity_guid_golden identity_guid_goldens[] = {\n"
    header += "".join(f'    {{"{record["guid"]}", {record["expected"]}ULL}},\n' for record in records)
    header += "};\n"
    metadata = {
        "schema": 1, "staged_only": True, "semantic_proof": False,
        "official_provider_authentication": False,
        "oracle": "Python hashlib PBKDF2-HMAC-SHA256, iterations100, output4, little-endian account",
        "public_source": {"path": SOURCE.relative_to(ROOT).as_posix(), "sha256": hashlib.sha256(source).hexdigest()},
        "salt_bytes": 256, "salt_sha256": hashlib.sha256(salt).hexdigest(),
        "records": records,
    }
    immutable_create(OUTPUT, header.encode("ascii"))
    immutable_create(METADATA, (json.dumps(metadata, indent=2) + "\n").encode("utf-8"))
    print(json.dumps({"staged_goldens": len(records), "header": OUTPUT.relative_to(ROOT).as_posix(), "semantic_proof": False}))


if __name__ == "__main__":
    main()
