"""Prepare an exact review patch under staged/archive; NEVER apply/build/launch.

Run only outside exclusive measurements. Every existing-source anchor must be
unique. This tool has no apply option; root must review and explicitly apply the
emitted patch after authorizing native changes. EOF fragments retain original TU
ownership and never become extra independently compiled native definitions.
"""
from __future__ import annotations
import difflib
import hashlib
import json
from pathlib import Path

STAGE = Path(__file__).resolve().parent
PROJECT = STAGE.parent.parent
GUARD = "defined(COD4X1KHZ) && defined(KISAK_MP) && defined(KISAK_DEDICATED)"
TARGET = "engine/src/khz/"


def replace_once(text, before, after, path):
    if text.count(before) != 1:
        raise ValueError(f"Refuse stale/ambiguous source anchor: {path}: {before[:100]!r}")
    return text.replace(before, after, 1)


def include(fragment):
    return f'\n#if {GUARD}\n#include "khz/{fragment}"\n#endif\n'


def prepare():
    replacements = {}
    originals = {}
    staged = {}
    fragments = {
        "native_probe.h": "khz_archive_probe.h",
        "native_probe.inl": "khz_archive_probe.inl",
        "native_probe_profile.inl": "khz_archive_probe_profile.inl",
        "native_probe_boundary.h": "khz_archive_probe_boundary.h",
        "native_probe_host.inl": "khz_archive_probe_host.inl",
        "native_probe_plugins.inl": "khz_archive_probe_plugins.inl",
        "native_probe_content.inl": "khz_archive_probe_content.inl",
        "native_probe_content_server.inl": "khz_archive_probe_content_server.inl",
        "native_probe_screenshot.inl": "khz_archive_probe_screenshot.inl",
        "native_probe_web.inl": "khz_archive_probe_web.inl",
        "native_probe_admin.inl": "khz_archive_probe_admin.inl",
        "native_probe_commands.inl": "khz_archive_probe_commands.inl",
    }
    for source, destination in fragments.items():
        target = TARGET + destination
        if (PROJECT / target).exists():
            raise ValueError("Refuse existing production destination: " + target)
        content = (STAGE / source).read_text(encoding="utf8")
        for old, new in fragments.items():
            content = content.replace('"' + old + '"', '"' + new + '"')
        replacements[target] = content
        staged[source] = hashlib.sha256((STAGE / source).read_bytes()).hexdigest()

    def load(path):
        original = (PROJECT / path).read_text(encoding="utf8")
        originals[path] = original
        return original

    def edit(path, before, after):
        current = replacements[path] if path in replacements else load(path)
        replacements[path] = replace_once(current, before, after, path)

    def prepend(path):
        current = replacements[path] if path in replacements else load(path)
        replacements[path] = include("khz_archive_probe_boundary.h") + current

    def eof(path, fragment):
        current = replacements[path] if path in replacements else load(path)
        replacements[path] = current.rstrip() + "\n" + include(fragment)

    # Actual source-owned callback and command registration are in khz_host.
    host = TARGET + "khz_host.cpp"
    edit(host, "bool commandCaptureTruncated = false;",
         "bool commandCaptureTruncated = false;\n"
         f"#if {GUARD}\n"
         "bool beginArchiveProbeOwnedCommand(const Packet&);\n"
         "void endArchiveProbeOwnedCommand();\n#endif")
    before = "        Cmd_ExecuteSingleCommand(0, 0, executableCommand);\n        Com_EndRedirect();"
    after = (f"#if {GUARD}\n"
             "        if (!beginArchiveProbeOwnedCommand(value)) {\n"
             "            Com_EndRedirect(); fail(\"Invalid direct owned command scope.\"); return;\n"
             "        }\n#endif\n        Cmd_ExecuteSingleCommand(0, 0, executableCommand);\n"
             f"#if {GUARD}\n        endArchiveProbeOwnedCommand();\n#endif\n        Com_EndRedirect();")
    edit(host, before, after)
    eof(host, "khz_archive_probe_host.inl")

    # Never-loaded uses persistent registry history, not cleared slot metadata.
    registry = "native/plugin_registry.hpp"
    anchor = "    unsigned RetiredScripts() const"
    edit(registry, anchor,
         "    std::uint64_t LoadGeneration() const { return generation_; }\n" + anchor)
    eof(TARGET + "khz_plugins.cpp", "khz_archive_probe_plugins.inl")

    content = TARGET + "khz_content.cpp"
    edit(content, "std::thread worker;", "std::thread worker;\n"
         f"#if {GUARD}\nstd::atomic<bool> archiveProbeWorkerEverStarted{{false}};\n#endif")
    edit(content, "    worker = std::thread(work);",
         f"#if {GUARD}\n    archiveProbeWorkerEverStarted.store(true, std::memory_order_release);\n#endif\n"
         "    worker = std::thread(work);")
    eof(content, "khz_archive_probe_content.inl")
    server_content = TARGET + "khz_content_server.cpp"
    edit(server_content, "bool start() {", f"#if {GUARD}\nbool archiveProbeWorkerEverStarted = false;\n#endif\nbool start() {{")
    edit(server_content, "    try { worker = std::thread(work); }",
         f"#if {GUARD}\n    archiveProbeWorkerEverStarted = true;\n#endif\n"
         "    try { worker = std::thread(work); }")
    eof(server_content, "khz_archive_probe_content_server.inl")
    eof(TARGET + "khz_screenshot_server.cpp", "khz_archive_probe_screenshot.inl")
    eof(TARGET + "khz_webadmin.cpp", "khz_archive_probe_web.inl")
    eof(TARGET + "khz_admin_server.cpp", "khz_archive_probe_admin.inl")
    eof("engine/src/qcommon/cmd.cpp", "khz_archive_probe_commands.inl")
    # Only the dedicated benchmark command's own handler permits execution.
    edit(TARGET + "khz_admin_server.cpp", '    const char* core[] = {"status",',
         '    const char* core[] = {"khz_archive_probe","status",')

    eof(TARGET + "khz_archive.cpp", "khz_archive_probe.inl")
    eof("engine/src/server_mp/sv_snapshot_profile_mp.cpp", "khz_archive_probe_profile.inl")

    # Lifecycle/error hooks are ordinary source hooks, not destructor cleanup.
    shutdown = "engine/src/server_mp/sv_init_mp.cpp"
    prepend(shutdown)
    edit(shutdown, "void __cdecl SV_Shutdown(const char *finalmsg)\n{",
         "void __cdecl SV_Shutdown(const char *finalmsg)\n{\n"
         f"#if {GUARD}\n    KHZ_ArchiveProbeAbortBeforeServerShutdown();\n"
         "    KHZ_ArchiveProbeOwnedCommandUnwind();\n#endif")
    common = "engine/src/qcommon/common.cpp"
    prepend(common)
    edit(common, "void Com_ErrorCleanup()\n{",
         "void Com_ErrorCleanup()\n{\n"
         f"#if {GUARD}\n    KHZ_ArchiveProbeAbortBeforeServerShutdown();\n"
         "    KHZ_ArchiveProbeOwnedCommandUnwind();\n#endif")
    runtime = TARGET + "khz_runtime.cpp"
    prepend(runtime)
    edit(runtime, '    resetStats();\n}\nvoid KHZ_WaitFrame()',
         f'    resetStats();\n#if {GUARD}\n'
         '    if (!KHZ_InitArchiveCodecProbeCommand())\n'
         '        Com_Error(ERR_FATAL, "Could not install authenticated archive codec probe");\n'
         '#endif\n}\nvoid KHZ_WaitFrame()')

    patch = []
    for path, changed in sorted(replacements.items()):
        original = originals.get(path, "")
        patch.extend(difflib.unified_diff(original.splitlines(True), changed.splitlines(True),
                     fromfile=("a/" + path) if path in originals else "/dev/null",
                     tofile="b/" + path))
    output = STAGE / "production_integration.patch"
    output.write_text("".join(patch), encoding="utf8", newline="\n")
    # Type-check a COPY of the exact proposed original TUs. This is never an
    # engine checkout or game build input; it remains under our staging owner.
    prepared = STAGE / "proposed-native"
    for path, content in replacements.items():
        destination = prepared / path
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(content, encoding="utf8", newline="\n")
    plan = {"schema": 1, "applied": False, "game_build": False,
            "patch": str(output), "source_owned_fragment_targets": fragments,
            "production_sources": {path: hashlib.sha256((PROJECT / path).read_bytes()).hexdigest()
                                   for path in originals},
            "staged_sources": staged,
            "native_link_required": True,
            "full_world_restore": False, "authoritative_replay": False,
            "integration_guards": GUARD,
            "prepared_native_root": str(prepared),
            "prepared_cpp": [path for path in originals if path.endswith(".cpp")],
            "notes": ["No new native implementation TU/CMake source: all definitions stay at actual owner EOF.",
                      "Both game targets already link these native sources; only dedicated gets definitions/calls.",
                      "Root must review/apply patch, compile/link both targets, and archive exact source/binary evidence.",
                      "Object type checks cannot establish runtime cleanup or codec correctness."]}
    (STAGE / "production_integration_plan.json").write_text(json.dumps(plan, indent=2), encoding="utf8")
    print(json.dumps({"prepared": True, "applied": False, "patch": str(output),
                      "existing_sources": len(originals), "new_files": len(fragments)}, indent=2))


if __name__ == "__main__":
    prepare()
