# Build 71 published SDK proofs

All three serial native fixtures passed on the exact coherent Build 71 archive, with the twenty-command pre-launch inventory checked and every owned process closed:

- [Formatter/default/configured zero](../results/plugin-identity-format-native-20261003-064911-596900/result.json), 13.54 seconds; owned PIDs 25796 and 1532.
- [Address/admission](../results/plugin-net-native-20261003-064934-807400/result.json), 9.54 seconds; owned PIDs 12492 and 27404.
- [DNS/UDP](../results/plugin-udp-native-20261003-065001-226100/result.json), 16.35 seconds; owned PID 9944 and both listeners closed. All 22 native cases passed.

After the four timing cases closed, both candidates passed current-source, binary, fixture and artifact validation and root reviewed their exact contracts. The network candidate was published first and the formatter candidate merged only its three entries afterward. The [network index](evidence/build71-sdk-network/index.json) records ten complete and nine partial entries; the [formatter index](evidence/build71-sdk-identity-format/index.json) records two complete exports and one partial dvar. Their validation files preserve each per-entry verdict.

The completed publication sequence was:

```powershell
python scripts/record_plugin_network_evidence.py --archive results/build71-source-and-binary-hashes.json --net-result results/plugin-net-native-20261003-064934-807400/result.json --udp-result results/plugin-udp-native-20261003-065001-226100/result.json --publish
python scripts/record_plugin_identity_format_evidence.py --archive results/build71-source-and-binary-hashes.json --result results/plugin-identity-format-native-20261003-064911-596900/result.json --publish
python scripts/capabilities.py --write
```

The existing network tool checks and records ten complete address/DNS contracts plus nine partial IPv4 transport, ban and callback contracts. Its candidate contains the current complete configuration. The new formatter tool prepares a scoped three-entry update: two complete formatter contracts and partial `sv_usesteam64id` persistence coverage. It includes actual engine wrappers/admin registration, the separately hashed `native/steam_identity_format.hpp` definitions, synchronized dvar ownership, the actual rebuilt C fixture, observed per-phase markers, native logs, and the real standalone suite result. It never promotes the other four conversion exports from their aggregate parser/GUID marker.

`scripts/record_plugin_identity_format_evidence.py` was written after the Build 71 archive, syntax-checked and run against the actual results. Its proof artifacts hash the writer independently and identify it as post-archive tooling. The immutable archive was preserved; the writer was not compiled into it. Root's canonical source verifier separately hashes the reviewed native helper so changing it invalidates both implementation and stale proof.

The regenerated census returned the expected failing release-gate exit code 1 with no verifier errors: 2,126 total, twelve proven, 2,114 pending, zero excluded and 33 unresolved opaque entries. Neither proof group establishes full public parity, native IPv6 game transport, official authentication or archived dvar persistence through a saved-config restart.

The failed Build 70 UDP artifact remains at `results/plugin-udp-native-20261003-064105-860500/result.json`. Its missing command was caused by a twenty-first fixture registration; the pinned server owns a twenty-element command array, while the adapted production registry safely rejects overflow. The fixture-only repair shares `khz_fixture_identity format` with the existing identity command and preserves the production limit. Six pure registration checks and MSVC C syntax passed before Build 71. Historic Build 70 successes and the failed UDP run remain immutable and are not reused as current Build 71 proof.
