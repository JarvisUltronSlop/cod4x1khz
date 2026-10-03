"""Record reviewed current-build network evidence; never launch a game or test.

Requires actual passing native results bound to the selected coherent archive.
Writes a candidate by default; --publish updates only the reviewed19 entries.
Historical build-scoped artifacts remain intact.
"""
from pathlib import Path
import hashlib
import importlib.util
import json
import re
import argparse

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--archive', required=True)
parser.add_argument('--net-result', required=True)
parser.add_argument('--udp-result', required=True)
parser.add_argument('--publish', action='store_true', help='Replace only reviewed current capability entries after native proof')
args = parser.parse_args()

def local_path(name):
    path = Path(name)
    if not path.is_absolute(): path = ROOT / path
    path = path.resolve()
    if not path.is_relative_to(ROOT): raise ValueError('Evidence paths must remain inside project')
    return path.relative_to(ROOT).as_posix()

archive_path = local_path(args.archive)
net_path, udp_path = local_path(args.net_result), local_path(args.udp_result)
archive = json.loads((ROOT / archive_path).read_text(encoding='utf-8-sig'))
build = archive.get('build')
assert type(build) is int and build > 0 and archive.get('tests', {}).get('passed') is True
OUTPUT_RELATIVE = f'docs/evidence/build{build}-sdk-network'
OUTPUT = ROOT / OUTPUT_RELATIVE
if OUTPUT.exists() and not args.publish:
    raise FileExistsError('Build-scoped evidence already exists; use --publish only to verify and publish an unchanged candidate')

def write_immutable(path, value):
    content = (json.dumps(value, indent=2) + '\n').encode('utf-8')
    if path.exists():
        if path.read_bytes() != content:
            raise RuntimeError('Refusing to overwrite changed build-scoped evidence: ' + str(path))
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)

def digest(path):
    return hashlib.sha256((ROOT / path).read_bytes()).hexdigest()

def read(path):
    return json.loads((ROOT / path).read_text(encoding="utf-8-sig"))

def reference(path):
    return {"path": path, "sha256": digest(path)}

# The newer coherent archives include actual fixture .inc files directly.
net, udp = map(read, (net_path, udp_path))
server_hash = archive["binary_sha256"]["runtime/cod4x1khz_server.exe"]
dll_hash = archive["binary_sha256"]["runtime/plugins/khz_regression.dll"]
assert digest("runtime/cod4x1khz_server.exe") == server_hash
assert digest("runtime/plugins/khz_regression.dll") == dll_hash
for result in (net, udp):
    assert result["passed"] and result["owned_processes_exited"]
    assert result.get("build") == build
    assert local_path(result["build_archive"]) == archive_path
    assert not result.get("archive_mismatches")
    assert result["sha256"][str((ROOT / archive_path).resolve())] == digest(archive_path)
    for path, expected in result["sha256"].items():
        local = Path(path).relative_to(ROOT).as_posix()
        assert digest(local) == expected, local
assert "cases=40 failures=0 sdk=2" in net["cases"][0]["evidence"]
assert "cases=8 failures=0 cleanup=1" in net["cases"][1]["evidence"]
for path in ("sdk/tests/native_net_address_probe.inc", "sdk/tests/native_udp_probe.inc", "sdk/tests/regression_plugin.c"):
    assert digest(path) == archive["source_sha256"][path], path

WRAPPER = "engine/src/khz/khz_plugin_net.inl"
ADDRESS = "engine/src/khz/khz_plugin_net_address.cpp"
HOST = "engine/src/khz/khz_plugins.cpp"
DNS = "engine/src/khz/khz_plugin_dns.cpp"
NET_FIXTURE = "sdk/tests/native_net_address_probe.inc"
UDP_FIXTURE = "sdk/tests/native_udp_probe.inc"
log_path = local_path(archive["tests"]["log"])
log = (ROOT / log_path).read_text(encoding="utf-8", errors="replace")
assert re.search(r"Test #\d+: khz_plugin_net_address.*Passed", log)
assert re.search(r"Test #\d+: khz_plugin_dns.*Passed", log)

def implementation(path, symbol):
    expected = archive["source_sha256"][path]
    assert digest(path) == expected, path
    lines = (ROOT / path).read_text(encoding="utf-8").splitlines()
    matching = [i for i, line in enumerate(lines, 1) if re.search(r"\b" + re.escape(symbol) + r"\s*\(", line)]
    assert matching, (path, symbol)
    return {"path": path, "sha256": expected, "line": matching[0], "symbol": symbol}

def native_case(name, fixture=NET_FIXTURE, result=net_path, assertions=1):
    text = (ROOT / fixture).read_text(encoding="utf-8")
    assert '"' + name + '"' in text, name
    return {"name": name, "passed": True, "assertions": assertions,
            "evidence": reference(result), "compiled_fixture": reference(fixture),
            "attestation": "The rebuilt C DLL reported zero failures for all checks in this fixture; this named check is present in its archived compiled source."}

def actual_case(index, assertions=1):
    item = udp["cases"][index]
    assert item["passed"]
    return {"name": item["name"], "passed": True, "assertions": assertions,
            "evidence": reference(udp_path), "result_case_index": index,
            "observed": item["evidence"]}

def standalone_case(name, source, test):
    assert digest(source) == archive["source_sha256"][source]
    return {"name": name, "passed": True, "assertions": 1,
            "evidence": reference(log_path), "test": test,
            "fixture_source": reference(source),
            "attestation": f"Build{build} CTest reported this real native suite passed. The named behavioral group is reviewed in its archived test source; this is supplemental to loaded-DLL checks."}

all_entries = []
config_path = "config/public-capability-evidence.json"
config = read(config_path)
config.setdefault("capabilities", {})
census = read("docs/public-capabilities.json")
census_entries = {entry["id"]: entry for entry in census["capabilities"]}

def record(name, kind, contract, implementations, cases, pending=(), limits=()):
    capability = "server:" + kind + ":" + name
    assert capability in census_entries, capability
    sources = {item["path"]: item["sha256"] for item in implementations}
    artifact = {
        "kind": "semantic_regression", "schema": 1, "build": build, "protocol": 10002,
        "sdk_version": 2, "capability_id": capability, "passed": True,
        "coverage": "partial" if pending else "reviewed_capability",
        "semantic_contract": contract, "cases": cases,
        "source_sha256": sources, "binary_sha256": {"server": server_hash},
        "regression_dll_sha256": dll_hash,
        "build_archive": reference(archive_path),
        "compiled_fixture_source_archive": reference(archive_path),
        "limitations": list(limits), "pending_semantics": list(pending),
        "official_authentication_proven": False, "ipv6_game_transport_proven": False,
        "full_public_feature_parity": False,
        "review": "Reviewed source implementation and actual native behavior; no declaration, registration, source-name match, stub or performance result is treated as semantic evidence."
    }
    relative = OUTPUT_RELATIVE + "/" + name + ".json"
    write_immutable(ROOT / relative, artifact)
    claim = {"semantic_contract": contract, "implementation": implementations}
    if pending:
        # The existing complete-evidence validator deliberately keeps these
        # applicable entries pending. No scope exclusion hides IPv6 work.
        claim["partial_regression"] = [reference(relative)]
        claim["pending_semantics"] = list(pending)
    else:
        claim["regression"] = [reference(relative)]
    previous = config["capabilities"].get(capability)
    assert previous is None or previous.get("semantic_contract") == contract, capability
    config["capabilities"][capability] = claim
    all_entries.append({"capability_id": capability, "coverage": artifact["coverage"],
                        "artifact": reference(relative), "pending_semantics": list(pending)})

def address_record(name, helper, contract, names, supplement_group=None, limits=()):
    cases = [native_case(case) for case in names]
    if supplement_group:
        cases.append(standalone_case(supplement_group, "engine/tests/khz_plugin_net_address_tests.cpp", "khz_plugin_net_address"))
    record(name, "sdk_export", contract, [implementation(WRAPPER, name), implementation(ADDRESS, helper)], cases, limits=limits)

address_record("Plugin_NET_CompareAdr", "Compare",
    "Compare typed public32 IPv4/TCP or IPv6/TCP6 bytes and unsigned network-order ports; loopback ignores port. Exact family is required; socket/scope are ignored as in the pinned comparison. Bad/null addresses fail.",
    ["native_full_vs_base"], "Full/base, exact-family, loopback, ignored scope/socket and bad/null comparison fixtures")
address_record("Plugin_NET_CompareBaseAdr", "CompareBase",
    "Compare exact public address family and the full IPv4 or IPv6 byte width while ignoring port/socket/scope; loopback compares equal. Bad/null addresses fail.",
    ["native_full_vs_base"], "Typed base comparisons and full-width IPv4/IPv6/null fixtures")
address_record("Plugin_NET_CompareBaseAdrMask", "CompareBase",
    "Compare exact family and the first N address bits, including partial bytes; negative or excessive N clamps to full 32/128-bit width. A zero mask still requires matching family. Loopback compares equal.",
    ["native_ipv4_mask_boundaries", "native_ipv6_mask_boundaries", "native_exact_family_comparison", "native_export_prefix_self"],
    "Independent changed-prefix-bit fixtures at IPv4/IPv6 byte and partial-byte boundaries")
address_record("Plugin_NET_AdrToString", "FormatAddress",
    "Return per-thread text for a full IPv4/TCP or bracketed IPv6/TCP6 address with unsigned port and numeric scope; retain pinned loopback/bot and null-literal behavior.",
    ["native_nonmt_full", "native_null_format_literal"], "Full IPv4/IPv6/scoped golden text, loopback/bot/null and unsupported types")
address_record("Plugin_NET_AdrToStringShort", "FormatAddress",
    "Return per-thread address text without a port, preserving IPv6 numeric scope and pinned loopback/bot/null spelling.",
    ["native_nonmt_short"], "Short address, scoped IPv6, special/null address formatting")
address_record("Plugin_NET_AdrToStringMT", "FormatAddress",
    "Format a full IPv4/TCP or bracketed IPv6/TCP6 address into the caller's bounded buffer and return it. Preserve numeric scope, unsigned ports, pinned minimum-buffer error text and null-address literal return; do not overwrite sentinels.",
    ["native_mt_ipv6_full", "native_scope_format", "native_mt_buffer_bounds"], "Full golden formats, minimum buffer/sentinels, null output/address and invalid family")
address_record("Plugin_NET_AdrToStringShortMT", "FormatAddress",
    "Format caller-owned bounded address text without a port, preserving IPv6 numeric scope and special/null return behavior through the common bounded formatter.",
    ["native_mt_ipv6_short"], "IPv4/scoped IPv6 short text and shared formatter buffer/null guards")
address_record("Plugin_NET_AdrMaskToStringMT", "FormatAddress",
    "Produce pinned masked IPv4 text retaining two octets or masked IPv6 text retaining 48 bits, plus unsigned port, using the caller's bounded formatter buffer.",
    ["native_mt_ipv4_mask", "native_mt_ipv6_mask"], "Independent IPv4/IPv6 masked golden strings and shared bounded formatter guards")
address_record("Plugin_IsLANAddress", "IsLAN",
    "Classify loopback, pinned IPv4 private/127 prefixes and IPv6 link-local/ULA prefixes as LAN; otherwise compare exact-family bytes against copied local-interface masks. Reject bad addresses and invalid catalogs without scanning.",
    ["native_ipv6_link_local_lan", "native_ipv6_ula_lan", "native_ipv4_private_lan", "native_documentation_ipv6_non_lan"],
    "LAN prefix boundaries and independent IPv4/IPv6 copied-interface mask membership fixtures",
    limits=["Loaded-DLL cases exercise prefix classification; synthetic interface mask membership is independently native-tested. This run does not prove an actual OS interface change/refresh/restart race."])

parse_cases = [native_case(name) for name in (
    "native_numeric_ipv4_unsigned_port", "native_numeric_ipv6_public_bytes",
    "native_forced_v6_mapped_literal_default_port", "native_numeric_scope",
    "native_dns_pending_never_reported_success", "native_malformed_port_clears_output")]
parse_cases += [actual_case(4), actual_case(17), actual_case(19), actual_case(21)]
parse_cases += [standalone_case("All numeric IPv4/IP6/TCP/TCP6/UNSPEC, scopes, unsigned/zero/default ports and malformed copied endpoint fixtures", "engine/tests/khz_plugin_net_address_tests.cpp", "khz_plugin_net_address"),
                standalone_case("Actual localhost DNS IPv4/IP6/TCP/TCP6/UNSPEC, immediate slot reuse, cancellation/reload, bounded admission and five-second deadline fixtures", "engine/tests/khz_plugin_dns_tests.cpp", "khz_plugin_dns")]
record("Plugin_NET_StringToAdr", "sdk_export",
    "Parse bounded numeric IPv4/IPv6/TCP/TCP6/UNSPEC with explicit network-order port (return1), default28960 (return2), IPv4-mapped IPv6 and numeric scope; malformed input returns0 with cleared public output. Hostnames return success only after real resolution in a pinned SDK worker, using a five-second bounded resolver with generation retirement. Main/tick hostname requests fail and clear output instead of blocking. IPv4 public sock is the configured native socket, all other public families use0; no DNS result implies a connected TCP socket.",
    [implementation(WRAPPER, "Plugin_NET_StringToAdr"), implementation(ADDRESS, "ParseNumeric"), implementation(DNS, "Resolve")], parse_cases,
    limits=["Rebuilt source-compatible SDK requires job/thread context for blocking DNS; native owner calls return failure.",
            "Loaded DLL resolves localhost as IP4/IP6/TCP; TCP6 and UNSPEC actual resolver paths have independent native backend evidence. No internet DNS/service availability or IPv6 game transport is claimed."])

ipv6_udp_pending = ["Native core IPv6 UDP socket, addressing, delivery and lifecycle integration remain unimplemented and untested."]
record("Plugin_UdpSendData", "sdk_export",
    "For current IPv4 custom pair, validate bounded datagram arguments and module generation, send through the configured native server socket, preserve actual source port, publish its public sock and return true only for actual kernel acceptance. Direct send bypasses OnUdpNetSend. Pinned API IPv6 transport is still applicable and pending.",
    [implementation(WRAPPER, "Plugin_UdpSendData"), implementation("engine/src/win32/win_net.cpp", "KHZ_NativeUdpSend")],
    [actual_case(index) for index in (1, 2, 3, 4, 16, 20, 21)], pending=ipv6_udp_pending,
    limits=["Actual payloads are exact known ASCII byte strings, not an embedded-NUL/arbitrary-binary runtime fixture.", "Broadcast/multicast, socket-close race and kernel would-block failures were not exercised by these native loopback runs."])
record("Plugin_ServerPacketEvent", "sdk_export",
    "For current IPv4 custom pair, copy address and packet bytes into a bounded host-owned mailbox and defer native dispatch to outer owner service. Recheck module generation/session/serverId/epoch, bypass the physical receive callback and retain native protocol10002/local-key proof validation. Disabled module generations are dropped. Native IPv6 input remains applicable and pending.",
    [implementation(WRAPPER, "Plugin_ServerPacketEvent"), implementation(WRAPPER, "pluginNetworkService")],
    [actual_case(index) for index in (13, 14, 15, 16, 18, 19, 21)], pending=ipv6_udp_pending,
    limits=["This changes immediate pinned dispatch into deferred owned dispatch for the custom 1ms clock.", "Native map/session transition, mailbox overflow and arbitrary binary payload runtime injection are not proven by this loaded-DLL run; the separate mailbox suite covers bounded copy/FIFO storage."])
record("OnUdpNetEvent", "sdk_callback",
    "For current physical IPv4 server datagrams, pass explicit public32 endpoint fields and mutable owned bytes to enabled plugins on native owner service before parsing. Honor veto, propagate byte/address edits and bypass the callback for plugin-injected packets. Native IPv6 event producers remain pending.",
    [implementation(WRAPPER, "KHZ_PluginUdpReceive"), implementation("engine/src/qcommon/common.cpp", "Com_EventLoop")],
    [actual_case(index) for index in (5, 6, 7, 8, 16, 19)], pending=ipv6_udp_pending,
    limits=["Loaded-DLL fixture verifies three IPv4 receive variants and owner/layout counters. Receive callback recursion/depth rejection and malformed edited endpoints are not exercised here."])
record("OnUdpNetSend", "sdk_callback",
    "For current normal native IPv4 server sends, pass public32 destination and mutable bytes through the main-owner callback, honor veto and apply payload/destination edits to the real datagram. Direct SDK send bypasses this event. Native IPv6 event producers remain pending.",
    [implementation(WRAPPER, "KHZ_PluginUdpSend"), implementation("engine/src/qcommon/net_chan_mp.cpp", "NET_SendPacket")],
    [actual_case(index) for index in (9, 10, 11, 12, 16, 20)], pending=ipv6_udp_pending,
    limits=["Actual callbacks cover RCON print datagrams; full arbitrary binary payloads, callback recursion and invalid edited destinations need additional fixtures."])

ipban_pending = ["Actual IPv6 IP-ban storage and admission matching remain applicable and pending; current adapter rejects IPv6 without truncating or replacing an IPv4 ban."]
for name, helper, checks in (
    ("Plugin_PlayerAddBanByip", "KHZ_AdminAddIpBan", ("native_public32_ipban_remaining_text", "native_rejected_ipv6_cannot_replace_ipv4_ban", "native_ipban_permanent_request_bounded_four_hours")),
    ("Plugin_PlayerBannedByip", "KHZ_AdminFormatIpBanMessage", ("native_public32_ipban_remaining_text", "native_ipban_distinct_address", "native_ipban_base_address_ignores_port", "native_ipban_message_bounds", "native_ipv6_ipban_pending_without_truncation")),
    ("Plugin_RemoveBanByip", "KHZ_AdminRemoveIpBan", ("native_ipban_remove_exact_address", "native_ipban_distinct_address"))):
    record(name, "sdk_export",
        "Current native IPv4 public32 IP-ban adapter preserves base-address identity, remaining-time reason text, configured NAT expiry bounds, bounded outputs and exact removal. All fixture bans are removed. IPv6 bans remain applicable and pending.",
        [implementation(HOST, name), implementation("engine/src/khz/khz_admin_server.cpp", helper)],
        [native_case(case) for case in checks], pending=ipban_pending,
        limits=["Native fixture tests real temporary bans and the configured four-hour cap for permanent requests; actual clock expiry is independently policy-tested, not waited out in this run."])

for name, index, contract in (
    ("OnPlayerConnect", 3, "Current admitted native IPv4 human receives a source-compatible callback containing an explicit public32 endpoint copy, current configured socket and original human endpoint. Native20 proof/connection ownership remains unchanged. IPv6 admission callbacks remain pending."),
    ("OnPlayerWantReservedSlot", 4, "Current native IPv4 private-slot human receives a public32 address callback and an output grant pointer; granting the actual callback admits that human to the configured reserved slot. IPv6 reserved admission remains pending.")):
    item = net["cases"][index]
    record(name, "sdk_callback", contract,
        [implementation(HOST, "KHZ_PluginEvent"), implementation("engine/src/server_mp/sv_client_mp.cpp", "SV_DirectConnect")],
        [{"name": item["name"], "passed": True, "assertions": 1, "evidence": reference(net_path), "result_case_index": index, "observed": item["evidence"]},
         {"name": "Actual admitted private-slot human reached CA_ACTIVE9", "passed": True, "assertions": 1, "evidence": reference(net_path), "result_case_index": 2},
         {"name": "Module unloaded after real callback scopes", "passed": True, "assertions": 1, "evidence": reference(net_path), "result_case_index": 5}],
        pending=["Native core IPv6 human admission and corresponding public callback endpoint remain applicable and pending."],
        limits=["No authentication-provider compatibility, connect denial/userinfo mutation or reserved-denial fixture is claimed by this particular run."])

candidate_path = OUTPUT / "capability-config-candidate.json"
write_immutable(candidate_path, config)
index = {"schema": 1, "build": build, "full_public_feature_parity": False,
         "binary_sha256": {"server": server_hash, "regression_dll": dll_hash},
         "build_archive": reference(archive_path), "compiled_fixture_source_archive": reference(archive_path),
         "native_results": [reference(net_path), reference(udp_path)], "entries": all_entries,
         "complete_evidence_entries": sum(item["coverage"] == "reviewed_capability" for item in all_entries),
         "partial_pending_entries": sum(item["coverage"] == "partial" for item in all_entries),
         "note": "Complete evidence is limited to reviewed address operations and adapted pinned DNS. Partial IPv4 transport/ban/admission evidence remains outside the validator's complete-regression field; the applicable whole capability stays pending. Any changed current binary or implementation invalidates current proof and requires the native gate again."}
write_immutable(OUTPUT / "index.json", index)

# Validate these entries only: this avoids a full upstream inventory during
# another agent's timing run and does not regenerate their owned manifest.
spec = importlib.util.spec_from_file_location("capabilities", ROOT / "scripts/capabilities.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
checked = {}
for item in all_entries:
    identity = item["capability_id"]
    status, issues = module.verify_evidence(ROOT, census_entries[identity], config["capabilities"][identity], config["binaries"])
    expected = "pending" if item["coverage"] == "partial" else "proven"
    assert status == expected, (identity, status, issues)
    checked[identity] = {"status": status, "reasons": issues}
write_immutable(OUTPUT / "validation.json", {"build": build, "entries": checked})
if args.publish:
    (ROOT / config_path).write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8", newline="\n")
print(json.dumps({"published": bool(args.publish), "candidate": candidate_path.relative_to(ROOT).as_posix(), "complete_evidence_entries": index["complete_evidence_entries"], "partial_pending_entries": index["partial_pending_entries"], "full_public_feature_parity": False}))
