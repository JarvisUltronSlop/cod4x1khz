# Staged next identity contracts

The authorized next-build fixture is integrated into `sdk/tests/regression_plugin.c` through the new `native_identity_contract_probe.inc` and `identity_guid_goldens.inc` includes. The production identity/parser implementation and public capability configuration are unchanged. The seven independently generated Python GUID goldens agree with a separate .NET derivation. Focused oracle checks, all six twenty-command registration checks and MSVC x86 C syntax pass in [the offline review](../results/identity-contract-review-20261003-072018-9729663/result.json). That result retains exact source and log hashes. No engine/DLL build or game launch ran, so actual rebuilt-DLL behavior and capability proof remain pending.

## Classifier batch

The two wrappers in `engine/src/khz/khz_plugins.cpp:746–747` call `khz_identity.cpp::SteamIdIndividual`. The pinned definitions are `upstream/cod4x_server/src/sapi.c::SV_SApiSteamIDIndividual` and `SV_SApiSteamIDIndividualSteamOnly`. Both require account type one and instance one. The first permits universe one and private universes 32 through 39; the second permits universe one only. Account bits do not affect classification.

The staged `config/staged/native_identity_contract_probe.inc` calls the actual two exports for all 256 universes, all sixteen type fields, instances zero/one/two/maximum and account zero/maximum. Expected values use the original assembled field inputs, independently of the host decoder. Sixty-four single-bit flips from a fixed literal and six independent literal identifiers supplement the 32,768-case matrix. Each export must report exactly 32,838 cases and zero failures. Unexpected qboolean values also fail; failure output stops after eight details per export.

The existing identity dispatcher now accepts `classify` with no new registration; the fixture and pinned per-plugin limit remain twenty. No-argument aggregate behavior and the existing `format` subcommand are preserved. Unknown subcommands and excess arguments produce an explicit rejection marker. After a coherent build, run on an archive-bound rebuilt DLL/dedicated server, verify direct native output, unload and prove exact owned cleanup. Per-export proof must bind the real wrapper/helper, independent fixture, loaded DLL, current server, case markers and native build/archive. Existing aggregate `conversions=1` output cannot promote these exports.

## GUID batch

Pinned `sapi.c::SV_SApiGUID2PlayerID` derives four bytes using PBKDF2-HMAC-SHA256, the 256-byte public salt and one hundred iterations. The Win32 account is those four bytes in little-endian order; the remaining fields are private universe32/type1/instance1. Current `khz_identity.cpp::GuidToPlayerId` preserves this valid 32-hex mapping with CNG. Its wrapper returns zero for a null pointer. Current invalid hex rejection is a deliberate correction to pinned `strtol` partial/invalid-pair conversion, and CNG failure returns zero rather than the pinned private identifier with account zero.

`config/staged/generate_identity_guid_goldens.py` generated independent Python goldens for zero, all-ff, ascending and descending bytes, 5a/a5 patterns and the existing independently derived .NET golden. It reads only the pinned salt, never imports the adapted implementation, requires exactly 256 salt bytes, checks the old known value and writes immutable staged header/metadata. The separate .NET reviewer reparses the pinned salt and reproduces all seven four-byte outputs. Those reviewed literals are copied into the rebuilt fixture source. Oracle preparation is distinct from actual export execution.

| GUID input | Expected public identifier |
| --- | ---: |
| `00000000000000000000000000000000` | 2310346614281832120 |
| `ffffffffffffffffffffffffffffffff` | 2310346616461335339 |
| `000102030405060708090a0b0c0d0e0f` | 2310346614110419531 |
| `0f0e0d0c0b0a09080706050403020100` | 2310346617253057279 |
| `5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a` | 2310346614304358351 |
| `a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5` | 2310346614399020859 |
| `0123456789abcdef0123456789abcdef` | 2310346614158149229 |

The staged native GUID subcommand checks every generated golden in lower/upper/alternating case, observes input buffers unchanged, rejects five nonhex characters at every position, tests every wrong length from zero through 64, tests null and the valid NUL-terminated-prefix contract. With seven goldens it must report 247 cases and zero failures. It calls the actual public export and emits its own case/failure marker. A crypto-provider failure is explicitly not injected; review whether a deterministic provider-error seam is needed before claiming the entire failure contract. The marker never claims official service authentication.

Reuse `khz_fixture_identity guid`, keeping twenty registrations. A new archive-bound harness should invoke `classify` then `guid` serially on one zero-bot dedicated fixture, with a maximum thirty-second process lifetime and no concurrent performance workload. Require exact per-export counts, immutable source/oracle/archive provenance, plugin unload and owned-process exit. Promote only reviewed complete contracts; parser promotion stays separate.

## Parser choices for later source and fixture review

The production parser remains unchanged. Its current strict numeric helper rejects positive field-leading whitespace and plus signs accepted by pinned `%u`/`%hu` scanning, and currently admits bracketed decimal IDs that pinned dispatch rejects. Apply these routine compatibility choices only at a later coordinated source checkpoint:

- Preserve exact `STEAM_` spelling, optional Steam3 brackets, recognized types U/M/G/A/P/C/g/T/c/L/a, default U/G instance one and other types instance zero, valid universes1–4/32–39 and unsigned decimal64 IDs.
- Preserve positive numeric fields with leading ASCII whitespace accepted by C unsigned scanning and one optional plus sign. Keep decimal64 digit-only, except the existing outer ASCII-space trimming. Do not trim trailing spaces inside fields before a colon; pinned literal separators reject them.
- Reject negatives, decimal overflow, legacy parity outside zero/one, legacy high bits above `UINT32_MAX/2`, instance above twenty bits, unexpected separators and any trailing text. These explicit safety corrections prevent wrap, truncation and cross-field aliasing.
- Require bracketed input to be Steam3; remove the undocumented bracketed-decimal extension. Keep the valid explicit four-field Steam3 instance form as a documented correction to pinned `ParseSteam3ID`'s final conversion-count bug.
- Bound the raw string before truncation/normalization, reject oversized input rather than parsing a truncated prefix, and make exact whitespace/length rules visible in fixtures. The current length check is after outer ASCII-space trimming; the raw-length choice requires source/fixture review.

The parser fixture must enumerate aliases, every permitted universe, default and explicit instance boundaries, positive numeric-field whitespace/plus, malformed signs/overflow, balanced/unbalanced brackets, bracketed decimal refusal, complete trailing-text refusal and raw-length boundaries. It must use independently assembled expected fields; formatter/parser roundtrips alone cannot prove the parser. Type nine remains legal in decimal64 identifiers but has no recognized textual account-type alias in the pinned parser. `Plugin_StringToSteamID` remains pending until this independent contract review and its actual tests pass.
