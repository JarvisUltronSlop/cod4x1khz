# Actual Build 73 classifier/GUID fixture

The archive-bound harness is `scripts/plugin_identity_contract_native_probe.py`. Build 73 native execution passed in 12.4725 seconds, with normal owned cleanup. PID 2016 used port 28990 and exited code zero after the explicit `quit`; the DLL unloaded first. [The actual result](C:/Users/jack/Desktop/cod4x1khz/results/plugin-identity-contract-native-20261003-074025-734500/result.json) binds source, DLL, server, package, oracle, and native-log hashes to the unchanged Build 73 archive.

The harness suite ran 11 tests: 10 passed, one actual symlink-creation case was skipped on this host, and there were no failures. Lexical traversal, mocked Windows reparse-ancestor refusal, fresh direct benchmark-home enrollment, strict markers, and exactly seven distinct independently passed oracle records passed. [The supplement](C:/Users/jack/Desktop/cod4x1khz/results/build73-identity-harness-supplement.json) preserves the source/log hashes and skip. This is separate from the immutable compiled-source archive.

For a future rerun, first obtain an exclusive native slot from the build owner:

```powershell
python scripts/plugin_identity_contract_native_probe.py --build-archive results/build73-source-and-binary-hashes.json --oracle-review results/identity-contract-review-20261003-072018-9729663/result.json --seconds 30
```

The harness checks the exact deployed dedicated/DLL/package and relevant compiled sources, the twenty-command inventory, independently hashed oracle sources/logs and agreement between all seven Python/.NET goldens. It chooses a free localhost port with a previously absent project benchmark home. It does not override `sv_usesteam64id`; the existing actual formatter subcommand must observe default one, pass its three scoped marker counts and restore the setting.

The actual `classify` command reported exactly 32,838 cases and zero failures for each exported classifier: 32,768 independently assembled field cases, 64 independent bit flips, and six literal goldens. The actual `guid` command reported 247 cases and zero failures: 21 golden/case-variant checks, 160 invalid-byte checks, 64 wrong lengths, null, and a terminated-prefix check. Input guard buffers were checked by the compiled fixture. Duplicate/missing/noncanonical markers, partial counts, failure details, changed helper/log/fixture bytes or an authentication flag fail the harness.

The harness also verifies unknown-subcommand/excess-argument refusals without execution, the original no-argument identity oracle, actual module unload and normal exit after its own `quit`. It touches only its exact owned process; forced termination is retained as cleanup failure. The maximum thirty seconds includes cleanup. The fixture runs one dedicated server with zero bots and no headless human or rendering/performance workload.

[The classifier/GUID candidate index](C:/Users/jack/Desktop/cod4x1khz/docs/evidence/build73-sdk-identity-contract/index.json) has two complete classifier contract candidates and partial GUID coverage. [The formatter candidate index](C:/Users/jack/Desktop/cod4x1khz/docs/evidence/build73-sdk-identity-format/index.json) covers both formatter exports from the same real run: selected99, explicit64 96, and Steam3 128 checks all passed. Both live dvar modes were exercised from a fresh default-one home; a separate configured-zero startup and saved-config persistence were not tested on this binary and remain partial dvar requirements.

Candidates require root review before publication. A cryptographic-provider error was not injected and remains explicit. The parser's compatibility corrections, official service authentication, and whole SDK parity remain outside this batch. Build 71's previous twelve proofs are historical after the deployed binary change and require current-build rebinding before a release claim. Public evidence configuration was not modified by these candidate writers.
