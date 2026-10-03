"""Assemble compile-only native proposals without editing production sources."""
from pathlib import Path
import argparse


def once(text, old, new):
    if text.count(old) != 1:
        raise ValueError("Native proposal anchor is missing or ambiguous: " + old[:80])
    return text.replace(old, new, 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    stage = Path(__file__).resolve().parent
    destination = stage / "typecheck"
    destination.mkdir(exist_ok=True)
    source = args.project / "engine/src/khz"
    profile = (source / "khz_profile.cpp").read_text(encoding="utf-8")
    profile = once(profile, '#include "khz_profile.h"', '#include "khz_profile.h"\n#include "khz_profile_audit.h"\n#include "khz_identity.h"\n#include "khz_client_owner.h"\n#include <qcommon/threads.h>\n#include <unlock_audit_policy.hpp>')
    profile = once(profile, "bool CommitUnlocks()\n{", '#include "khz_profile_audit.inl"\n\nbool CommitUnlocks()\n{')
    anchor = '    live->statWriteNeeded = false;\n    KHZ_ProfileReloadEntitlements();\n    Com_Printf(CON_CHANNEL_CLIENT,\n        "KHZ_UNLOCK_TRANSACTION '
    replacement = '    live->statWriteNeeded = false;\n    KHZ_ProfileReloadEntitlements();\n    CaptureSuccessfulUnlockAudit(original,candidate,plan,record,encoded,profile,backup);\n    Com_Printf(CON_CHANNEL_CLIENT,\n        "KHZ_UNLOCK_TRANSACTION '
    profile = once(profile, anchor, replacement)
    profile = once(profile, "bool KHZ_ProfileCommitAllUnlocks()", '#include "khz_profile_audit_public.inl"\n\nbool KHZ_ProfileCommitAllUnlocks()')
    (destination / "profile.cpp").write_text(profile, encoding="utf-8")
    owner = (source / "khz_client_owner.cpp").read_text(encoding="utf-8")
    (destination / "owner.cpp").write_text(owner + '\n#include "khz_equipment_owner.inl"\n', encoding="utf-8")
    print("Prepared exact-anchor compile-only owner/profile copies under " + str(destination))


if __name__ == "__main__":
    main()
