"""Conservative pinned-source capability census and fail-closed parity gate.

Run with --write to regenerate docs/public-capabilities.{json,md}. The default
prints a summary and returns 1 while applicable capabilities lack reviewed
implementation and semantic regression proof. --self-test uses isolated fixtures.
This is a source inventory, not an automatic equivalence claim.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from finite_registration_enrollment import enroll_finite_registrations

EXTENSIONS = {".c", ".cc", ".cpp", ".h", ".hpp", ".asm", ".s", ".def"}
CALL_RE = re.compile(r"\b(Cmd_Add(?:P|H|Server)?Command(?:Internal|Generic)?|Cvar_Register\w+|Dvar_Register\w+|Scr_AddFunction|Scr_AddMethod|Scr_AddClassField|GScr_LoadScriptAndLabel|(?:GScr|Scr)_AddFieldsFor\w+|SetJump|SetCall|Sys_PatchImageWithBlock|Patch_Memset|Com_Memset|Com_Memcpy)\s*\(")
STRING_RE = re.compile(r'^(?:L|u8|u|U)?"((?:\\.|[^"\\])*)"$')


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def strip_comments(source: str, assembly: bool = False) -> str:
    """Blank comments without shifting byte offsets, quotes, or line numbers."""
    out = list(source)
    i = 0
    while i < len(source):
        if source[i] in "\"'":
            quote = source[i]
            i += 1
            while i < len(source):
                if source[i] == "\\" and not assembly:
                    i += 2
                elif source[i] == quote:
                    i += 1
                    break
                else:
                    i += 1
        elif (assembly and source[i] == ";") or source.startswith("//", i):
            end = source.find("\n", i)
            end = len(source) if end < 0 else end
            out[i:end] = " " * (end - i)
            i = end
        elif source.startswith("/*", i) and not assembly:
            end = source.find("*/", i + 2)
            end = len(source) if end < 0 else end + 2
            for j in range(i, end):
                if source[j] not in "\r\n":
                    out[j] = " "
            i = end
        else:
            i += 1
    return "".join(out)


def mask_literals(source: str) -> str:
    return re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                  lambda m: "".join("\n" if c == "\n" else " " for c in m.group()), source)


def inactive_zero(source: str) -> str:
    """Remove only provably inactive literal branches; include unknown macros."""
    stack: list[tuple[bool, bool | None]] = []
    active = True
    output = []
    for line in source.splitlines(keepends=True):
        match = re.match(r"\s*#\s*(if|ifdef|ifndef|else|elif|endif)\b(.*)", line)
        was_active = active
        if match:
            directive, condition = match.group(1), match.group(2).strip()
            if directive in {"if", "ifdef", "ifndef"}:
                literal = condition == "1" if directive == "if" and condition in {"0", "1"} else None
                stack.append((active, literal))
                active = active and literal is not False
            elif directive in {"else", "elif"} and stack:
                parent, literal = stack[-1]
                if directive == "else":
                    active = parent and literal is not True
                elif literal is True:
                    active = False
                else:
                    next_literal = condition == "1" if condition in {"0", "1"} else None
                    active = parent and next_literal is not False
                    stack[-1] = (parent, next_literal if literal is not None else None)
            elif directive == "endif" and stack:
                active = stack.pop()[0]
        output.append(line if was_active and not match else "".join(c if c in "\r\n" else " " for c in line))
    return "".join(output)


def balanced(source: str, start: int, opener="(", closer=")") -> tuple[str, int]:
    depth = 0
    i = start
    quote = None
    while i < len(source):
        char = source[i]
        if quote:
            if char == "\\":
                i += 2
                continue
            if char == quote:
                quote = None
        elif char in "\"'":
            quote = char
        elif char == opener:
            depth += 1
        elif char == closer:
            depth -= 1
            if depth == 0:
                return source[start + 1:i], i + 1
        i += 1
    raise ValueError(f"Unclosed {opener} at offset {start}")


def arguments(body: str) -> list[str]:
    result = []
    depth = 0
    quote = None
    begin = 0
    i = 0
    while i < len(body):
        char = body[i]
        if quote:
            if char == "\\":
                i += 2
                continue
            if char == quote:
                quote = None
        elif char in "\"'":
            quote = char
        elif char in "([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
        elif char == "," and depth == 0:
            result.append(body[begin:i].strip())
            begin = i + 1
        i += 1
    result.append(body[begin:].strip())
    return result


def literal(expression: str) -> str | None:
    match = STRING_RE.fullmatch(expression.strip())
    if not match:
        return None
    # Registration names are ASCII; preserve uncommon C escapes explicitly.
    return match.group(1).replace(r'\"', '"').replace(r"\\", "\\")


FORMAL_TYPE = re.compile(r'^(?:(?:const|volatile)\s+)*(?:void|char|int|unsigned|signed|short|long|float|double|bool|byte|DWORD|XAssetType|struct|union|enum)\b')
EXPRESSION_WORD = re.compile(r'\b(?:return|co_return|throw|if|else|while|for|switch|case|sizeof|alignof|new|delete|do|goto)\b')
TYPE_HEAD = re.compile(r'(?:[A-Za-z_]\w*|::|\*|&|\s)+$')


def _strip_attributes(head):
    """Remove known attribute invocations without guessing their contents."""
    attribute = re.compile(r'\b(?:__declspec|__attribute__|alignas)\s*\(')
    while match := attribute.search(head):
        cursor, depth = match.end(), 1
        while cursor < len(head) and depth:
            depth += (head[cursor] == '(') - (head[cursor] == ')')
            cursor += 1
        if depth:
            return None
        head = head[:match.start()] + ' ' + head[cursor:]
    return head


def registration_is_declaration(masked, start, end, args):
    """Exclude confident prototypes/definitions; retain uncertain call sites.

    `masked` must preserve the offsets of the comment/inactive/literal-masked
    source. `start` is CALL_RE.start(), `end` is balanced()'s closing-paren end,
    and args are arguments(body). A call assignment is never a declaration.
    This is deliberately narrower than a C/C++ parser; unknown syntax is kept
    as inventory for manual review, rather than silently dropped.
    """
    tail = masked[end:].lstrip()
    if tail.startswith('{'):
        return True
    first = args[0].strip() if args else ''
    # Formal type keywords cannot be registration-name expressions. Include
    # void: it is the exact reason six no-argument prototypes were miscounted.
    if first and FORMAL_TYPE.match(first):
        return True
    before = masked[:start]
    boundary = max(before.rfind(';'), before.rfind('{'), before.rfind('}')) + 1
    head = re.sub(r'^[\t ]*#[^\n]*', '', before[boundary:], flags=re.M).strip()
    if not head or EXPRESSION_WORD.search(head) or '=' in head:
        return False
    head = _strip_attributes(head)
    if head is None:
        return False
    # A return type, qualifiers, calling convention, namespace, and pointers
    # form a declaration head. Operators/commas/parentheses make it uncertain.
    if not TYPE_HEAD.fullmatch(head) or not re.search(r'[A-Za-z_]\w*', head):
        return False
    if tail.startswith(';'):
        return True
    suffix = _strip_attributes(tail)
    return suffix is not None and suffix.lstrip().startswith(';')


def census(text: str, relative: str, domain: str) -> list[dict]:
    assembly = Path(relative).suffix.lower() in {".asm", ".s"}
    clean = strip_comments(text, assembly)
    if not assembly:
        clean = inactive_zero(clean)
    masked = mask_literals(clean)
    entries = []

    def add(kind, name, offset, via, details=None, resolved=True):
        line = clean.count("\n", 0, offset) + 1
        entries.append({"kind": kind, "name": name, "resolved": resolved,
                        "source": {"path": relative, "line": line, "via": via,
                                   "details": details or ""}})

    if not assembly:
        for match in CALL_RE.finditer(masked):
            api = match.group(1)
            body, end = balanced(clean, match.end() - 1)
            args = arguments(body)
            # Exclude definitions/prototypes, rather than counting formal names.
            if registration_is_declaration(masked, match.start(), end, args):
                continue
            if api.startswith("Cmd_Add"):
                kind, which = "command", 0
            elif api.startswith(("Cvar_Register", "Dvar_Register")):
                kind, which = "dvar", 0
            elif api in {"Scr_AddFunction", "Scr_AddMethod"}:
                kind, which = ("gsc_global" if api == "Scr_AddFunction" else "gsc_method"), 0
            elif api == "Scr_AddClassField":
                kind, which = "gsc_field", 1
            elif api == "GScr_LoadScriptAndLabel":
                kind, which = "gsc_callback", 1
            elif "AddFieldsFor" in api:
                add("gsc_field_registry", api, match.start(), api, "Opaque registry requires a named-field review.", False)
                continue
            else:
                # Normal Com_Memcpy/Memset calls are not binary patch sites.
                if api in {"Com_Memset", "Com_Memcpy"} and not re.search(r"0x[0-9a-fA-F]{5,}", args[0]):
                    continue
                add("semantic_patch_site", f"{relative}:{clean.count(chr(10), 0, match.start()) + 1}:{api}",
                    match.start(), api, " ".join(body.split()))
                continue
            name = literal(args[which]) if len(args) > which else None
            if name is None:
                name = f"dynamic@{relative}:{clean.count(chr(10), 0, match.start()) + 1}"
            add(kind, name, match.start(), api, " ".join(body.split()), name is not None and not name.startswith("dynamic@"))

        # Statically initialized command, method and field descriptor tables.
        array_re = re.compile(r"\b(\w*(?:commands?|cmds|functions?|methods?|fields))\s*\[[^;{}]*\]\s*=\s*\{", re.I)
        for match in array_re.finditer(masked):
            table = match.group(1)
            body, _ = balanced(clean, match.end() - 1, "{", "}")
            before = clean[max(0, match.start() - 100):match.start()]
            if "netField" in before or "fielddef_t" in before:
                continue
            kind = "gsc_field" if "field" in table.lower() else "gsc_method" if "method" in table.lower() else "gsc_global" if "function" in table.lower() else "command"
            for row in re.finditer(r'\{\s*("(?:\\.|[^"\\])*")\s*,', body):
                add(kind, literal(row.group(1)), match.end() + row.start(), f"table:{table}")

        declarations = re.sub(r"^[\t ]*#[^\n]*", lambda m: " " * len(m.group()), masked, flags=re.M)
        for match in re.finditer(r"\bP_P_F\b[^;{}]*?\b(\w+)\s*\(", declarations):
            add("sdk_export", match.group(1), match.start(), "P_P_F public SDK export")
        if Path(relative).name == "function_declarations.h":
            for match in re.finditer(r"\b(Plugin_\w+)\s*\(", masked):
                _, end = balanced(clean, match.end() - 1)
                if clean[end:].lstrip().startswith(";"):
                    add("sdk_export", match.group(1), match.start(), "public SDK declaration header")
        else:
            for match in re.finditer(r"\b__cdecl\b[^;{}\n]*?\b(Plugin_\w+)\s*\(", declarations):
                add("sdk_export", match.group(1), match.start(), "plugin SDK declaration/export")
        for match in re.finditer(r"\b(?:Q_EXPORT|__declspec\s*\(\s*dllexport\s*\))\s+[^;{}\n]*?\b(\w+)\s*\(", declarations):
            add("sdk_export", match.group(1), match.start(), "DLL export")
        for match in re.finditer(r"\bPCL\s+[^;{}\n]*?\b(On\w+)\s*\(", masked):
            add("sdk_callback", match.group(1), match.start(), "plugin callback declaration")
        callbacks = re.search(r"\bPHandler_Events\s*\[[^;{}]*\]\s*=\s*\{", masked)
        if callbacks:
            body, _ = balanced(clean, callbacks.end() - 1, "{", "}")
            for match in re.finditer(r'"(On\w+)"', body):
                add("sdk_callback", match.group(1), callbacks.end() + match.start(), "PHandler_Events")
        for match in re.finditer(r"\b((?:Patch|[A-Z][A-Za-z0-9]*_Patch)[A-Za-z0-9_]*)\s*\([^;{}]*\)\s*\{", masked):
            add("semantic_patch_function", match.group(1), match.start(), "patch function definition")
        # Active direct absolute-address writes are separate semantic review sites.
        for match in re.finditer(r"\*\s*\([^;\n]*?\)\s*0x[0-9a-fA-F]{5,}\s*=\s*[^;]+;", masked):
            add("semantic_patch_site", f"{relative}:{clean.count(chr(10), 0, match.start()) + 1}:absolute_write", match.start(), "absolute_write", " ".join(clean[match.start():match.end()].split()))
    else:
        strings = {m.group(1): m.group(2) for m in re.finditer(r'^\s*(\w+):\s*db\s*"([^"\n]*)"\s*,\s*0\s*$', clean, re.M)}
        for match in re.finditer(r"^\s*(\w*fields):\s*dd\s+([^\n]+)", clean, re.M | re.I):
            # Weapon/physics/vehicle asset parsers also have 'fields' tables.
            # Only a script class registration makes these public GSC fields.
            if not re.search(r"^\s*call\s+Scr_AddClassField\s*$", clean, re.M):
                continue
            for label in re.findall(r"\b_cstring_\w+\b", match.group(2)):
                if label in strings:
                    add("gsc_field", f"{Path(relative).stem}:{strings[label]}", match.start(), f"asm table:{match.group(1)}", label)
        for match in re.finditer(r"^\s*call\s+(Cmd_Add\w+|Cvar_Register\w+|Dvar_Register\w+|Scr_AddClassField)\s*$", clean, re.M):
            api = match.group(1)
            prefix = clean[max(0, match.start() - 900):match.start()].splitlines()[-12:]
            slot = r"\[esp\+(?:0x)?4\]" if api == "Scr_AddClassField" else r"\[esp\]"
            assignments = [m.group(1) for line in prefix if (m := re.search(r"mov\s+(?:dword\s+)?" + slot + r",\s*(_cstring_\w+)", line))]
            name = strings.get(assignments[-1]) if assignments else None
            kind = "command" if api.startswith("Cmd_Add") else "gsc_field" if api == "Scr_AddClassField" else "dvar"
            add(kind, name or f"dynamic@{relative}:{clean.count(chr(10), 0, match.start()) + 1}", match.start(), f"asm call:{api}", "\n".join(prefix), bool(name))
        for match in re.finditer(r"^\s*global\s+(Plugin_\w+)\s*$", clean, re.M):
            add("sdk_export", match.group(1), match.start(), "assembly SDK export")
        # pluginexports.asm exposes more public names than the declaration
        # header. Expand actual invocations, retaining alias targets as source
        # provenance; macro definitions and commented invocations are excluded.
        for match in re.finditer(r"^\s*pexport\s+([A-Za-z_]\w*)\s*$", clean, re.M):
            add("sdk_export", "Plugin_" + match.group(1), match.start(), "assembly pexport", match.group(1))
        for match in re.finditer(r"^\s*ralias\s+([A-Za-z_]\w*)\s*,\s*([A-Za-z_]\w*)\s*$", clean, re.M):
            add("sdk_export", match.group(1), match.start(), "assembly ralias", "target=" + match.group(2))
    for entry in entries:
        entry["id"] = f"{domain}:{entry['kind']}:{entry['name']}"
        entry["domain"] = domain
    return entries


def project_file(root: Path, name: str) -> Path:
    path = (root / name).resolve()
    if not path.is_relative_to(root.resolve()) or not path.is_file():
        raise ValueError(f"Missing or out-of-project evidence/source file: {name}")
    return path


def hashed_json(root: Path, reference: dict) -> dict:
    path = project_file(root, reference["path"])
    raw = path.read_bytes()
    if digest(raw) != reference.get("sha256"):
        raise ValueError(f"Evidence SHA256 mismatch: {reference['path']}")
    return json.loads(raw)


def verify_evidence(root: Path, entry: dict, claim: dict, binaries: dict) -> tuple[str, list[str]]:
    if not claim:
        return "pending", ["No reviewed implementation and semantic regression evidence."]
    issues = []
    if claim.get("applicable") is False:
        try:
            review = hashed_json(root, claim["scope_review"])
            if not claim.get("reason") or review.get("kind") != "scope_review" or review.get("capability_id") != entry["id"] or review.get("applicable") is not False or not review.get("reason"):
                raise ValueError("Scope exclusion lacks a matching reviewed reason.")
        except (KeyError, TypeError, ValueError, OSError) as exc:
            return "pending", [str(exc)]
        return "excluded", [claim["reason"]]
    implementation = claim.get("implementation", [])
    hashes = {}
    if not implementation or not claim.get("semantic_contract"):
        issues.append("Missing reviewed implementation or semantic contract.")
    for source in implementation:
        try:
            path = project_file(root, source["path"])
            raw = path.read_bytes()
            relative = path.relative_to(root.resolve()).as_posix()
            if (source["path"] != relative
                    or relative.split("/", 1)[0] not in {"engine", "native"}
                    or path.suffix.lower() not in EXTENSIONS | {".inc", ".inl"}
                    or digest(raw) != source.get("sha256")):
                raise ValueError("Implementation must be hashed canonical editable engine/native source.")
            text = strip_comments(raw.decode("utf-8", errors="replace"))
            line = source.get("line", 0)
            if not isinstance(line, int) or line < 1 or line > len(text.splitlines()) or not source.get("symbol") or not re.search(r"\b" + re.escape(source["symbol"]) + r"\b", mask_literals(text)):
                raise ValueError("Implementation provenance has an invalid line/symbol.")
            hashes[source["path"]] = digest(raw)
        except (KeyError, TypeError, ValueError, OSError) as exc:
            issues.append(str(exc))
    regressions = claim.get("regression", [])
    if not regressions:
        issues.append("Missing semantic regression artifacts.")
    for reference in regressions:
        try:
            proof = hashed_json(root, reference)
            if proof.get("kind") != "semantic_regression" or proof.get("capability_id") != entry["id"] or proof.get("passed") is not True or not proof.get("semantic_contract"):
                raise ValueError("Artifact is not passing semantic regression evidence for this exact capability.")
            cases = proof.get("cases", [])
            if not cases or any(case.get("passed") is not True or not case.get("name") or not isinstance(case.get("assertions"), int) or isinstance(case["assertions"], bool) or case["assertions"] < 1 for case in cases):
                raise ValueError("Regression evidence needs concrete passing assertion cases.")
            if not hashes or any(proof.get("source_sha256", {}).get(name) != value for name, value in hashes.items()):
                raise ValueError("Regression source hashes do not match the reviewed implementation.")
            binary = project_file(root, binaries[entry["domain"]])
            if proof.get("binary_sha256", {}).get(entry["domain"]) != digest(binary.read_bytes()):
                raise ValueError("Regression binary hash is not the current relevant release binary.")
        except (KeyError, TypeError, ValueError, OSError) as exc:
            issues.append(str(exc))
    if not entry["resolved"]:
        issues.append("Dynamic/opaque upstream registration still needs a named-capability census.")
    return ("pending", issues) if issues else ("proven", [])


def inventory(root: Path, config: dict) -> dict:
    capabilities = {}
    files = []
    errors = []
    for domain, upstream in sorted(config["upstreams"].items()):
        checkout = root / upstream["path"]
        try:
            revision = subprocess.check_output(["git", "-C", str(checkout), "rev-parse", "HEAD"], text=True).strip()
            if revision != upstream["revision"]:
                errors.append(f"{domain} checkout revision differs from the pin: {revision}")
            listing = subprocess.check_output(["git", "-C", str(checkout), "ls-tree", "-r", "-z", upstream["revision"]])
        except (OSError, subprocess.CalledProcessError) as exc:
            errors.append(f"Cannot inspect {domain} pinned source: {exc}")
            continue
        for record in listing.split(b"\0"):
            if not record:
                continue
            metadata, name_bytes = record.split(b"\t", 1)
            name = name_bytes.decode("utf-8")
            if Path(name).suffix.lower() not in EXTENSIONS or not any(name == prefix or name.startswith(prefix.rstrip("/") + "/") for prefix in upstream["source_roots"]):
                continue
            path = checkout / name
            relative = path.relative_to(root).as_posix()
            try:
                raw = path.read_bytes()
            except OSError as exc:
                errors.append(f"Missing pinned source {relative}: {exc}")
                continue
            pinned_blob = metadata.split()[2].decode()
            git_blob = lambda b: hashlib.sha1(b"blob " + str(len(b)).encode() + b"\0" + b).hexdigest()
            if pinned_blob not in {git_blob(raw), git_blob(raw.replace(b"\r\n", b"\n"))}:
                errors.append(f"Modified pinned source: {relative}")
            files.append({"path": relative, "sha256": digest(raw), "pinned_blob": pinned_blob})
            try:
                discovered = census(raw.decode("utf-8", errors="replace"), relative, domain)
            except ValueError as exc:
                errors.append(f"Incomplete source parse {relative}: {exc}")
                continue
            for entry in discovered:
                identifier = entry.pop("id")
                source = entry.pop("source")
                source["sha256"] = digest(raw)
                source["upstream_url"] = f"{upstream['url']}/blob/{upstream['revision']}/{name}#L{source['line']}"
                if identifier not in capabilities:
                    capabilities[identifier] = {"id": identifier, **entry, "provenance": []}
                capabilities[identifier]["provenance"].append(source)
    # Named source enrollment cannot grant a semantic status or exclusion. The
    # exact reviewed families fail closed before claim/evidence verification.
    capabilities, finite_errors = enroll_finite_registrations(
        capabilities, root, config["upstreams"].get("server", {}))
    errors.extend(finite_errors)
    for identifier in config.get("capabilities", {}):
        if identifier not in capabilities:
            errors.append(f"Evidence refers to an undiscovered capability: {identifier}")
    counts = Counter()
    for identifier, entry in sorted(capabilities.items()):
        claim = config.get("capabilities", {}).get(identifier, {})
        status, issues = verify_evidence(root, entry, claim, config["binaries"])
        entry.update(status=status, applicable=status != "excluded", reasons=issues)
        if claim:
            entry["evidence"] = claim
        entry["provenance"].sort(key=lambda p: (p["path"], p["line"], p["via"]))
        counts[status] += 1
    files.sort(key=lambda item: item["path"])
    return {"schema": 1, "scope": config["scope"], "upstreams": config["upstreams"],
            "inventory_contract": "Conservative public C/C++/assembly source census including the SDK union of declarations, P_P_F entrypoints and pluginexports.asm pexport/ralias invocations; comments and literal inactive branches excluded; unknown conditional branches included; dynamic names and opaque registries stay pending. No declaration or stub counts as tested parity.",
            "source_files": files, "source_census_sha256": digest(json.dumps(files, sort_keys=True).encode()),
            "summary": {"total": len(capabilities), "source_files": len(files), "proven": counts["proven"], "pending": counts["pending"], "excluded": counts["excluded"], "unresolved": sum(not e["resolved"] for e in capabilities.values()), "release_gate_passed": bool(capabilities) and not errors and counts["pending"] == 0},
            "errors": sorted(errors), "capabilities": [capabilities[key] for key in sorted(capabilities)]}


def markdown(report: dict) -> str:
    summary = report["summary"]
    rows = ["# Public CoD4x capability evidence", "", report["scope"], "", report["inventory_contract"], "",
            f"Release gate: **{'PASS' if summary['release_gate_passed'] else 'FAIL'}**. {summary['total']} capabilities; {summary['proven']} proven, {summary['pending']} pending, {summary['excluded']} reviewed exclusions; {summary['unresolved']} dynamic or opaque entries.", "",
            "A failing gate is expected until applicable behavior has implementation and semantic regression proof. Performance benchmarks alone do not establish CoD4x feature parity.", ""]
    if report["errors"]:
        rows.extend(["## Census/evidence errors", "", *[f"- {error}" for error in report["errors"]], ""])
    rows.extend(["## Inventory", "", "| Capability | State | Pinned source |", "| --- | --- | --- |"])
    for entry in report["capabilities"]:
        source = entry["provenance"][0]
        name = entry["id"].replace("|", "\\|").replace("`", "'")
        rows.append(f"| `{name}` | {entry['status']} | [{source['path']}:{source['line']}]({source['upstream_url']}) |")
    return "\n".join(rows) + "\n"


class ScannerTests(unittest.TestCase):
    def test_comments_quotes_inactive_and_arguments(self):
        source = '''// Cmd_AddCommand("fake", nope);
/* Cvar_RegisterBool("fake2", 0, 0, ""); */
Cmd_AddCommand("real", Fn);
Cvar_RegisterString("url", "https://site/a//b", 0, "/* literal */");
#if 0
Scr_AddFunction("disabled", Fn, 0);
#else
Scr_AddFunction("enabled", Fn, 0);
#endif
Scr_AddMethod("nested", Fn(foo(1, 2)), 0);
'''
        found = census(source, "src/fixture.c", "server")
        self.assertEqual({item["name"] for item in found}, {"real", "url", "enabled", "nested"})
        self.assertEqual(next(item for item in found if item["name"] == "real")["source"]["line"], 3)

    def test_opaque_and_sdk_and_patch(self):
        source = '''Cmd_AddCommand(name, Fn);
GScr_AddFieldsForClient();
P_P_F void Plugin_Do(int x) { }
PCL void OnFrame();
SetJump(0x123456, NewBehavior);
// SetJump(0x654321, CommentOnly);
void Cmd_AddCommand(const char *name, void *fn);
'''
        found = census(source, "src/fixture.c", "server")
        self.assertEqual(len(found), 5)
        self.assertEqual(sum(not e["resolved"] for e in found), 2)

    def test_sdk_union_preserves_live_exports_and_alias_provenance(self):
        assembly = '''%macro pexport 1
global Plugin_%1
%endmacro
; pexport CommentOnly
pexport FS_FreeFile
pexport Scr_AllocArray
pexport unzSetPassword
ralias Plugin_Milliseconds, Sys_Milliseconds
ralias Plugin_AlternateAddress, Native_Address
'''
        found = census(assembly, "src/asmsource/pluginexports.asm", "server")
        self.assertEqual({e["name"] for e in found}, {"Plugin_FS_FreeFile", "Plugin_Scr_AllocArray", "Plugin_unzSetPassword", "Plugin_Milliseconds", "Plugin_AlternateAddress"})
        alias = next(e for e in found if e["name"] == "Plugin_AlternateAddress")
        self.assertEqual(alias["source"]["details"], "target=Native_Address")
        self.assertEqual(alias["source"]["line"], 9)
        source = '''P_P_F qboolean Plugin_IsLoaded(char *name) { }
P_P_F version_t *Plugin_GetVersion(char *name) { }
P_P_F void AlternatePublicName(int x) { }
// P_P_F void CommentOnly() { }
__cdecl void Plugin_FS_FreeFile(void *buffer);
#define P_P_F __attribute__((visibility("default")))
#define P_P_F __declspec(dllexport)
'''
        self.assertEqual({e["name"] for e in census(source, "src/plugin_exports.c", "server")}, {"Plugin_IsLoaded", "Plugin_GetVersion", "AlternatePublicName", "Plugin_FS_FreeFile"})
        header = '''uint64_t Plugin_GUID2PlayerID(const char* guid);
__cdecl void Plugin_FS_FreeFile(void* bytes);
/* Example: Plugin_NotPublic(1); */
'''
        self.assertEqual({e["name"] for e in census(header, "plugins/function_declarations.h", "server")}, {"Plugin_GUID2PlayerID", "Plugin_FS_FreeFile"})

    def test_gate_rejects_names_and_benchmarks_accepts_semantic_evidence(self):
        with tempfile.TemporaryDirectory(prefix="khz-capabilities-") as temporary:
            root = Path(temporary)
            (root / "engine").mkdir()
            source = root / "engine/feature.cpp"
            source.write_text("void Implemented() { DoRealWork(); }\n")
            binary = root / "client.exe"
            binary.write_bytes(b"fixture binary")
            entry = {"id": "client:command:example", "domain": "client", "resolved": True}
            claim = {"semantic_contract": "Concrete fixture behavior", "implementation": [{"path": "engine/feature.cpp", "sha256": digest(source.read_bytes()), "line": 1, "symbol": "Implemented"}]}
            self.assertEqual(verify_evidence(root, entry, claim, {"client": "client.exe"})[0], "pending")
            proof = {"kind": "benchmark", "passed": True}
            artifact = root / "proof.json"
            artifact.write_text(json.dumps(proof))
            claim["regression"] = [{"path": "proof.json", "sha256": digest(artifact.read_bytes())}]
            self.assertEqual(verify_evidence(root, entry, claim, {"client": "client.exe"})[0], "pending")
            proof = {"kind": "semantic_regression", "capability_id": entry["id"], "passed": True, "semantic_contract": "Concrete fixture behavior", "cases": [{"name": "result and invalid input", "passed": True, "assertions": 2}], "source_sha256": {"engine/feature.cpp": digest(source.read_bytes())}, "binary_sha256": {"client": digest(binary.read_bytes())}}
            artifact.write_text(json.dumps(proof))
            claim["regression"][0]["sha256"] = digest(artifact.read_bytes())
            self.assertEqual(verify_evidence(root, entry, claim, {"client": "client.exe"})[0], "proven")
            source.write_text("void Implemented() { return; }\n")
            self.assertEqual(verify_evidence(root, entry, claim, {"client": "client.exe"})[0], "pending")

    def test_native_helper_is_required_hashed_implementation(self):
        with tempfile.TemporaryDirectory(prefix="khz-capabilities-") as temporary:
            root = Path(temporary)
            for directory in ("engine", "native", "config", "upstream"):
                (root / directory).mkdir()
            wrapper = root / "engine/wrapper.cpp"
            wrapper.write_text("void Wrapped() { NativeHelper(); }\n")
            helper = root / "native/helper.hpp"
            helper.write_text("inline void NativeHelper() { DoRealWork(); }\n")
            binary = root / "client.exe"
            binary.write_bytes(b"fixture binary")
            entry = {"id": "client:command:helper", "domain": "client", "resolved": True}
            implementation = [
                {"path": "engine/wrapper.cpp", "sha256": digest(wrapper.read_bytes()), "line": 1, "symbol": "Wrapped"},
                {"path": "native/helper.hpp", "sha256": digest(helper.read_bytes()), "line": 1, "symbol": "NativeHelper"},
            ]
            proof = {"kind": "semantic_regression", "capability_id": entry["id"], "passed": True,
                     "semantic_contract": "Native helper and wrapper behavior",
                     "cases": [{"name": "bounded helper result", "passed": True, "assertions": 2}],
                     "source_sha256": {source["path"]: source["sha256"] for source in implementation},
                     "binary_sha256": {"client": digest(binary.read_bytes())}}
            artifact = root / "proof.json"
            artifact.write_text(json.dumps(proof))
            claim = {"semantic_contract": proof["semantic_contract"], "implementation": implementation,
                     "regression": [{"path": "proof.json", "sha256": digest(artifact.read_bytes())}]}
            self.assertEqual(verify_evidence(root, entry, claim, {"client": "client.exe"})[0], "proven")
            helper.write_text("inline void NativeHelper() { ChangedWork(); }\n")
            self.assertEqual(verify_evidence(root, entry, claim, {"client": "client.exe"})[0], "pending")
            implementation[1]["sha256"] = digest(helper.read_bytes())
            self.assertEqual(verify_evidence(root, entry, claim, {"client": "client.exe"})[0], "pending")
            proof["source_sha256"]["native/helper.hpp"] = implementation[1]["sha256"]
            artifact.write_text(json.dumps(proof))
            claim["regression"][0]["sha256"] = digest(artifact.read_bytes())
            self.assertEqual(verify_evidence(root, entry, claim, {"client": "client.exe"})[0], "proven")
            for name in ("config/helper.hpp", "upstream/helper.hpp", "native/helper.json", "engine/../native/helper.hpp"):
                if ".." not in name:
                    (root / name).write_bytes(helper.read_bytes())
                rejected = {"path": name, "sha256": digest(helper.read_bytes()), "line": 1, "symbol": "NativeHelper"}
                claim["implementation"] = [implementation[0], rejected]
                proof["source_sha256"] = {implementation[0]["path"]: implementation[0]["sha256"], name: rejected["sha256"]}
                artifact.write_text(json.dumps(proof))
                claim["regression"][0]["sha256"] = digest(artifact.read_bytes())
                self.assertEqual(verify_evidence(root, entry, claim, {"client": "client.exe"})[0], "pending", name)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--evidence", default="config/public-capability-evidence.json")
    parser.add_argument("--write", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(ScannerTests)
        return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1
    root = args.root.resolve()
    config = json.loads(project_file(root, args.evidence).read_text(encoding="utf-8"))
    if config.get("schema") != 1:
        raise ValueError("Unsupported capability evidence schema.")
    report = inventory(root, config)
    if args.write:
        (root / "docs").mkdir(exist_ok=True)
        (root / "docs/public-capabilities.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline="\n")
        (root / "docs/public-capabilities.md").write_text(markdown(report), encoding="utf-8", newline="\n")
    print(json.dumps({**report["summary"], "errors": report["errors"]}, indent=2, sort_keys=True))
    return 0 if report["summary"]["release_gate_passed"] else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"Capability audit error: {error}", file=sys.stderr)
        raise SystemExit(2)
