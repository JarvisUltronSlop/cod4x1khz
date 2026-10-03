#!/usr/bin/env python3
"""Read-only native Physics hook enrollment audit; no gameplay or restore proof."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re

FILES = (
    "physics/phys_ode.cpp", "physics/phys_contacts.cpp",
    "physics/phys_coll_capsulebrush.cpp", "physics/phys_world_collision.cpp",
    "physics/ode/ode.cpp", "physics/ode/collision_kernel.cpp",
    "physics/ode/collision_kernel.h", "physics/ode/collision_space.cpp",
    "physics/ode/collision_std.cpp", "physics/ode/collision_transform.cpp",
    "physics/ode/collision_quadtreespace.cpp", "physics/ode/util.cpp",
    "physics/ode/quickstep.cpp", "physics/ode/error.cpp",
    "physics/ode/timer.cpp", "physics/ode/joint.cpp",
)
PARTS = (
    ("physics/phys_ode.cpp", "khz_server_physics_checkpoint.inl"),
    ("physics/ode/ode.cpp", "khz_server_ode_checkpoint.inl"),
    ("physics/ode/collision_kernel.cpp", "khz_server_ode_colliders_checkpoint.inl"),
    ("physics/ode/timer.cpp", "khz_server_ode_timers_checkpoint.inl"),
    ("physics/phys_coll_capsulebrush.cpp", "khz_server_physics_capsule_checkpoint.inl"),
    ("physics/ode/error.cpp", "khz_server_ode_errors_checkpoint.inl"),
    ("physics/ode/quickstep.cpp", "khz_server_ode_quickstep_checkpoint.inl"),
    ("physics/phys_contacts.cpp", "khz_server_physics_steps_checkpoint.inl"),
)
PROFILE = {
    "_WIN32": True, "WIN32": True, "_MSC_VER": True, "__cplusplus": True,
    "KISAK_MP": True, "KISAK_DEDICATED": True,
    "COD4X1KHZ_SERVER_CHECKPOINT": True, "USE_POOL_ALLOCATOR": True,
    "dSINGLE": True,
}
LEXICAL = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/')
GUARD = re.compile(r"\bKHZ_SERVER_PHYSICS_GUARD_(VOID|VALUE|FATAL)\s*\(\s*([A-Za-z_][A-Za-z_0-9]*)\s*(?:,\s*([^)]*))?\)")

def comments_removed(source: str) -> str:
    # Keep strings and offsets/line numbers; replace comments with whitespace.
    return LEXICAL.sub(lambda m: re.sub(r"[^\n]", " ", m[0]) if m[0].startswith(("//", "/*")) else m[0], source)

def condition(expression: str, macros: dict[str, bool]) -> bool:
    # Deliberately small Boolean-only preprocessor profile for these source files.
    # No eval(), subprocess, include expansion or compiler invocation.
    expression = re.sub(r"defined\s*\(\s*(\w+)\s*\)|defined\s+(\w+)",
                        lambda m: "1" if macros.get(m[1] or m[2], False) else "0", expression)
    tokens = re.findall(r"&&|\|\||!|\(|\)|\w+|\S", expression)
    index = 0

    def atom() -> bool:
        nonlocal index
        if index == len(tokens):
            raise ValueError("incomplete Boolean preprocessor expression")
        token = tokens[index]
        index += 1
        if token == "!":
            return not atom()
        if token == "(":
            value = either()
            if index == len(tokens) or tokens[index] != ")":
                raise ValueError("unbalanced preprocessor expression")
            index += 1
            return value
        if token.isdigit():
            return int(token) != 0
        if re.fullmatch(r"[A-Za-z_]\w*", token):
            return macros.get(token, False)
        raise ValueError(f"unsupported preprocessor token: {token}")

    def both() -> bool:
        nonlocal index
        value = atom()
        while index < len(tokens) and tokens[index] == "&&":
            index += 1
            right = atom()  # Parse even when the left operand is false.
            value = value and right
        return value

    def either() -> bool:
        nonlocal index
        value = both()
        while index < len(tokens) and tokens[index] == "||":
            index += 1
            right = both()
            value = value or right
        return value

    value = either()
    if index != len(tokens):
        raise ValueError("unsupported remainder in preprocessor expression")
    return value

def active_source(source: str, profile: dict[str, bool] | None = None, *, preserve_includes: bool = False) -> str:
    """Retain only Windows dedicated opt-in branches, preserving text offsets."""
    source = comments_removed(source)
    macros = dict(PROFILE if profile is None else profile)
    stack: list[list[bool]] = []  # parent-active, any-branch-taken, current-active
    output: list[str] = []
    continuation = False
    for line in source.splitlines(keepends=True):
        active = stack[-1][2] if stack else True
        directive = re.match(r"\s*#\s*(\w+)\s*(.*)", line)
        if continuation:
            continuation = line.rstrip().endswith("\\")
            output.append(re.sub(r"[^\n]", " ", line))
            continue
        if directive:
            kind, body = directive.groups()
            body = body.strip()
            if kind in ("if", "ifdef", "ifndef"):
                branch = condition(body, macros) if kind == "if" else macros.get(body, False)
                if kind == "ifndef":
                    branch = not branch
                stack.append([active, branch, active and branch])
            elif kind == "elif":
                if not stack:
                    raise ValueError("orphan elif")
                parent, taken, _ = stack[-1]
                branch = condition(body, macros)
                stack[-1] = [parent, taken or branch, parent and not taken and branch]
            elif kind == "else":
                if not stack:
                    raise ValueError("orphan else")
                parent, taken, _ = stack[-1]
                stack[-1] = [parent, True, parent and not taken]
            elif kind == "endif":
                if not stack:
                    raise ValueError("orphan endif")
                stack.pop()
            elif active and kind == "define":
                match = re.match(r"(\w+)(.*)", body)
                if match:
                    name, value = match.groups()
                    macros[name] = value.strip() != "0"
            elif active and kind == "undef":
                macros.pop(body, None)
            continuation = line.rstrip().endswith("\\")
            output.append(line if kind == "include" and active and preserve_includes else re.sub(r"[^\n]", " ", line))
        else:
            output.append(line if active else re.sub(r"[^\n]", " ", line))
    if stack:
        raise ValueError("unterminated preprocessor branch")
    return "".join(output)

def audit(root: Path) -> dict:
    src = root / "engine" / "src"
    header_path = src / "khz" / "khz_server_physics_guards.h"
    header = comments_removed(header_path.read_text(encoding="utf-8-sig"))
    enum_match = re.search(r"enum class Site : unsigned\s*\{([\s\S]*?)\};", header)
    names_match = re.search(r"Names\[Count\]\s*=\s*\{([\s\S]*?)\};", header)
    if not enum_match or not names_match:
        raise ValueError("native hook catalog missing")
    ids = [item.strip() for item in enum_match[1].split(",") if item.strip()]
    if not ids or ids.pop() != "Count" or len(set(ids)) != len(ids):
        raise ValueError("invalid or duplicate native hook catalog IDs")
    names = re.findall(r'"([^"]+)"', names_match[1])
    if len(names) != len(ids):
        raise ValueError("native hook name/ID count differs")
    if len(ids) != 246:
        raise ValueError(f"reviewed native hook count changed: {len(ids)} != 246")
    failures: list[str] = []
    source_hashes = {}
    entries = {}
    dependencies = {
        "deps/ode/common.h": ("USE_POOL_ALLOCATOR",),
        "deps/ode/config.h": ("dSINGLE",),
    }
    for filename, required in dependencies.items():
        path = root / "engine" / filename
        text = comments_removed(path.read_text(encoding="utf-8-sig"))
        for macro in required:
            if not re.search(r"^\s*#\s*define\s+" + re.escape(macro) + r"\b", text, re.M):
                failures.append(f"reviewed source profile changed: {filename} no longer defines {macro}")
        if filename.endswith("config.h") and re.search(r"^\s*#\s*define\s+dTRIMESH_ENABLED\b", text, re.M):
            failures.append("reviewed empty native ODE scope does not cover enabled trimeshes")
        source_hashes[filename] = hashlib.sha256(path.read_bytes()).hexdigest()
    cmake = (root / "engine" / "scripts" / "common_files.cmake").read_text(encoding="utf-8-sig")
    for filename in FILES:
        path = src / filename
        raw = path.read_bytes()
        source_hashes[filename] = hashlib.sha256(raw).hexdigest()
        if filename not in cmake:
            failures.append(f"native file not enrolled in common_files.cmake: {filename}")
        active = active_source(raw.decode("utf-8-sig"))
        for match in GUARD.finditer(active):
            kind, identity, value = match.groups()
            line = active.count("\n", 0, match.start()) + 1
            if identity not in ids:
                failures.append(f"unknown hook {identity}: {filename}:{line}")
                continue
            if identity in entries:
                failures.append(f"duplicate active hook {identity}: {filename}:{line}")
            if not active[:match.start()].rstrip().endswith("{"):
                failures.append(f"hook is not first function statement: {filename}:{line}")
            entries[identity] = {
                "id": ids.index(identity), "name": names[ids.index(identity)],
                "file": filename, "line": line, "kind": kind,
                "failure_return": value.strip() if value is not None else None,
            }
    for identity in ids:
        if identity not in entries:
            failures.append(f"catalog hook has no active native entry: {identity}")
    gate = r"#if\s+defined\(COD4X1KHZ_SERVER_CHECKPOINT\)\s*&&\s*defined\(KISAK_DEDICATED\)"
    for part, (filename, fragment) in enumerate(PARTS):
        source = comments_removed((src / filename).read_text(encoding="utf-8-sig"))
        ending = gate + r"\s*#include\s*<khz/" + re.escape(fragment) + r">\s*#endif\s*\Z"
        if not re.search(ending, source):
            failures.append(f"part {part} fragment lacks exact opt-in EOF include: {filename}")
        provider_path = src / "khz" / fragment
        provider = comments_removed(provider_path.read_text(encoding="utf-8-sig"))
        if f"KHZ_ServerPhysicsFenceEnroll({part})" not in provider:
            failures.append(f"part {part} has no actual enrollment")
        if "KHZ_ServerPhysicsFenceIdle(f, text, capacity)" not in provider or "KHZ_ServerPhysicsRequireCommitFence(f)" not in provider:
            failures.append(f"part {part} lacks provider fence guards")
        source_hashes["khz/" + fragment] = hashlib.sha256(provider_path.read_bytes()).hexdigest()
    default_branch = header.split("#else", 1)[1] if "#else" in header else ""
    for kind, args in (("VOID", "site"), ("VALUE", "site, value"), ("FATAL", "site")):
        expected = f"#define KHZ_SERVER_PHYSICS_GUARD_{kind}({args}) do {{ }} while (0)"
        if expected not in default_branch:
            failures.append(f"default-disabled {kind} macro changed")
    for fatal in ("dError", "dDebug"):
        if entries.get(fatal, {}).get("kind") != "FATAL":
            failures.append(f"native fatal {fatal} can incorrectly continue after refusal")
    source_hashes["khz/khz_server_physics_guards.h"] = hashlib.sha256(header_path.read_bytes()).hexdigest()
    for filename in ("khz_server_physics_fence.inl", "khz_server_physics_checkpoint.h", "khz_server_physics_checkpoint_native.h"):
        path = src / "khz" / filename
        source_hashes["khz/" + filename] = hashlib.sha256(path.read_bytes()).hexdigest()
    return {
        "schema": 1, "completed": True, "passed": not failures,
        "scope": "source_enrollment_only", "native_restore_attestation": False,
        "world_replay_attestation": False, "default_opt_in_enabled": False,
        "profile": "MSVC_Win32_dedicated_opt_in_source_branches",
        "required_parts": 255, "guard_entries": len(ids),
        "files": source_hashes, "entries": [entries[x] for x in ids if x in entries],
        "failures": failures,
    }

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    try:
        result = audit(args.root.resolve())
    except (OSError, UnicodeError, ValueError, IndexError) as error:
        result = {"schema": 1, "completed": False, "passed": False,
                  "scope": "source_enrollment_only", "failures": [str(error)]}
    print(json.dumps(result, indent=2))
    return 0 if result["passed"] else 1

if __name__ == "__main__":
    raise SystemExit(main())

