"""Unapplied native timestep proposals; this module never writes engine files.

Run explicitly with --write-patches only when review output is wanted.
The native contracts import proposed_sources() and compile extracted functions,
not a separate implementation of the proposed native algorithms.
"""

from __future__ import annotations

import argparse
import difflib
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
ITEMS = Path("engine/src/game/g_items.cpp")
HELICOPTER = Path("engine/src/game_mp/g_scr_helicopter.cpp")
MISSILE = Path("engine/src/game/g_missile.cpp")


def replace_exact(source: str, old: str, new: str, expected: int = 1) -> str:
    actual = source.count(old)
    if actual != expected:
        raise ValueError(f"proposal source changed: expected {expected} exact matches, found {actual}: {old!r}")
    return source.replace(old, new)


def function_bounds(source: str, name: str) -> tuple[int, int]:
    """Extract an actual definition, ignoring braces in comments and literals."""
    pattern = rf"(?m)^[ \t]*(?:void|int|float|double|int32_t)\s+(?:__cdecl\s+)?{re.escape(name)}\s*\("
    found = list(re.finditer(pattern, source))
    if len(found) != 1:
        raise ValueError(f"expected exactly one native definition for {name}, found {len(found)}")
    start = found[0].start()
    opening = source.find("{", found[0].end())
    if opening < 0:
        raise ValueError(f"missing native body for {name}")
    depth, index, state = 0, opening, "code"
    while index < len(source):
        char = source[index]
        other = source[index + 1] if index + 1 < len(source) else ""
        if state in ("string", "char"):
            if char == "\\":
                index += 2
                continue
            if (state == "string" and char == '"') or (state == "char" and char == "'"):
                state = "code"
        elif state == "line":
            if char == "\n":
                state = "code"
        elif state == "block":
            if char == "*" and other == "/":
                state = "code"
                index += 2
                continue
        else:
            if char == "/" and other == "/":
                state = "line"
                index += 2
                continue
            if char == "/" and other == "*":
                state = "block"
                index += 2
                continue
            if char == '"':
                state = "string"
            elif char == "'":
                state = "char"
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return start, index + 1
        index += 1
    raise ValueError(f"unclosed native body for {name}")


def native_function(source: str, name: str) -> str:
    start, end = function_bounds(source, name)
    return source[start:end]


def replace_function(source: str, name: str, replacement: str) -> str:
    start, end = function_bounds(source, name)
    return source[:start] + replacement + source[end:]


def proposed_items(source: str) -> str:
    body = native_function(source, "G_RunItem")
    body = replace_exact(
        body,
        "    float dot; // [esp+B8h] [ebp-4h]",
        """    float dot; // [esp+B8h] [ebp-4h]
#ifdef COD4X1KHZ
    const int itemStepMsec = level.frametime > 0 ? level.frametime : 1;
    bool needsSupportProbe = false;
#else
    const int itemStepMsec = 50;
#endif""",
    )
    body = replace_exact(body, "level.time + 50, origin", "level.time + itemStepMsec, origin")
    body = replace_exact(
        body,
        """        if (Vec3LengthSq(diff) < 0.1000000014901161)
            origin[2] = origin[2] - 1.0;""",
        """#ifdef COD4X1KHZ
        needsSupportProbe = Vec3LengthSq(diff) < 0.1000000014901161;
#else
        if (Vec3LengthSq(diff) < 0.1000000014901161)
            origin[2] = origin[2] - 1.0;
#endif""",
    )
    body = replace_exact(body, "ent->s.lerp.pos.trDuration = 50;", "ent->s.lerp.pos.trDuration = itemStepMsec;")
    body = replace_exact(
        body,
        "Vec3Scale(ent->s.lerp.pos.trDelta, 20.0, ent->s.lerp.pos.trDelta);",
        "Vec3Scale(ent->s.lerp.pos.trDelta, 1000.0 / itemStepMsec, ent->s.lerp.pos.trDelta);",
    )
    body = replace_exact(
        body,
        """        SV_LinkEntity(ent);
        G_RunThink(ent);""",
        """#ifdef COD4X1KHZ
        // Probe support independently; a clear probe is not additional falling motion.
        if (needsSupportProbe && tr.fraction >= 1.0f)
        {
            trace_t supportTrace;
            float supportEnd[3];
            supportEnd[0] = ent->r.currentOrigin[0];
            supportEnd[1] = ent->r.currentOrigin[1];
            supportEnd[2] = ent->r.currentOrigin[2] - 1.0f;
            const int supportPassEntity = ent->r.ownerNum.isDefined()
                ? ent->r.ownerNum.entnum() : ENTITYNUM_NONE;
            G_TraceCapsule(&supportTrace, ent->r.currentOrigin, ent->r.mins,
                ent->r.maxs, supportEnd, supportPassEntity, mask);
            if (supportTrace.fraction < 0.009999999776482582
                && supportTrace.normal[2] > 0.0f)
            {
                Vec3Lerp(ent->r.currentOrigin, supportEnd, supportTrace.fraction, endpos);
                tr = supportTrace;
            }
        }
#endif
        SV_LinkEntity(ent);
        G_RunThink(ent);""",
    )
    return replace_function(source, "G_RunItem", body)


def proposed_helicopter(source: str) -> str:
    # These are the actual MP spawn, first-think and recurring-think deadlines.
    # Shared physics already uses this positive interval; no tuning is changed.
    return replace_exact(source, "nextthink = level.time + 50;", "nextthink = level.time + G_VehicleFrameMsec();", 3)


def proposed_missile(source: str) -> str:
    body = native_function(source, "JavelinRotateVelocity")
    body = replace_exact(
        body,
        "    len = (1.0 - turnDiff / 180.0 * missileJavTurnDecel->current.value) * len;",
        """#ifdef COD4X1KHZ
    // This is stock 50 ms speed retention, not a retention to repeat every 1 ms.
    const double stockRetention = 1.0 - turnDiff / 180.0 * missileJavTurnDecel->current.value;
    const double step = MissileStepSeconds();
    const double retention = step == double(0.05f)
        ? stockRetention : pow(stockRetention, step / double(0.05f));
    len = retention * len;
#else
    len = (1.0 - turnDiff / 180.0 * missileJavTurnDecel->current.value) * len;
#endif""",
    )
    return replace_function(source, "JavelinRotateVelocity", body)


def proposed_sources() -> dict[Path, tuple[str, str]]:
    changes = {}
    for path, transform in (
        (ITEMS, proposed_items),
        (HELICOPTER, proposed_helicopter),
        (MISSILE, proposed_missile),
    ):
        before = (ROOT / path).read_text(encoding="utf-8-sig")
        after = transform(before)
        changes[path] = (before, after)
    return changes


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write-patches", action="store_true", help="write review patches only under staged/timestep")
    args = parser.parse_args()
    if not args.write_patches:
        parser.error("explicit --write-patches is required; this utility never applies engine changes")
    output = ROOT / "staged/timestep"
    output.mkdir(parents=True, exist_ok=True)
    for relative, (before, after) in proposed_sources().items():
        patch = "".join(difflib.unified_diff(
            before.splitlines(keepends=True), after.splitlines(keepends=True),
            fromfile="a/" + relative.as_posix(), tofile="b/" + relative.as_posix(),
        ))
        destination = output / (relative.stem + ".patch")
        destination.write_text(patch, encoding="utf-8", newline="\n")
        print(destination)


if __name__ == "__main__":
    main()
