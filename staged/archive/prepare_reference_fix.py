"""Prepare diagnostic-only native oracle repair; do not apply/build/launch.

The real archive decoder is already correct. Its standalone oracle must also
initialize the list-reference state after MSG_BeginReading at both sites.
Every original byte/field/float comparison remains, with first-index details.
"""
from pathlib import Path
import difflib
import json

STAGE = Path(__file__).resolve().parent
PROJECT = STAGE.parent.parent
RELATIVE = "engine/src/khz/khz_archive_probe.inl"


def proposed(text):
    prefix = "        msg_t read; MSG_InitReadOnly(&read, image->wire, wire.cursize); MSG_BeginReading(&read);"
    if text.count(prefix) != 2:
        raise ValueError("Refuse changed client/entity standalone read initializers")
    changed = text.replace(prefix, prefix + "\n        MSG_ClearLastReferencedEntity(&read);")
    client = "        const auto number = MSG_ReadEntityIndex(&read, 6);\n        MSG_ReadDeltaClient"
    client_new = ("        const auto number = MSG_ReadEntityIndex(&read, 6);\n"
                  "        if (number != index) {\n"
                  "            char detail[160]; std::snprintf(detail, sizeof(detail),\n"
                  '                "native_oracle_client_index row=%d expected=%d actual=%d", i, index, number);\n'
                  "            ok = fail(detail); break;\n        }\n        MSG_ReadDeltaClient")
    entity = "        const auto number = MSG_ReadEntityIndex(&read, 10);\n        MSG_ReadDeltaArchivedEntity"
    entity_new = ("        const auto number = MSG_ReadEntityIndex(&read, 10);\n"
                  "        if (number != index) {\n"
                  "            char detail[160]; std::snprintf(detail, sizeof(detail),\n"
                  '                "native_oracle_entity_index row=%d expected=%d actual=%d", i, index, number);\n'
                  "            ok = fail(detail); break;\n        }\n        MSG_ReadDeltaArchivedEntity")
    for old, new in ((client, client_new), (entity, entity_new)):
        if changed.count(old) != 1:
            raise ValueError("Refuse changed actual standalone index decode anchor")
        changed = changed.replace(old, new, 1)
    return changed


if __name__ == "__main__":
    original = (PROJECT / RELATIVE).read_text(encoding="utf8")
    changed = proposed(original)
    destination = STAGE / "reference_reset.patch"
    destination.write_text("".join(difflib.unified_diff(original.splitlines(True), changed.splitlines(True),
                           fromfile="a/" + RELATIVE, tofile="b/" + RELATIVE)), encoding="utf8")
    print(json.dumps({"prepared": True, "applied": False, "patch": str(destination),
                      "native_codec_changed": False, "comparisons_weakened": False}, indent=2))
