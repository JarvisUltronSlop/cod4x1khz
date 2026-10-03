# Identity proposal for source review

These files were added after the frozen Build74 archive. Production sources, the loaded SDK, scanner tool hashes, and current proof configuration remain unchanged by this proposal. No native parser, provider failure, or save/restart proof is created here.

`prepare_identity_proposal.py` verifies exactly the three original Build74 source hashes before preparing a unified patch and proposed full sources beneath `proposal/`. It never applies the patch. It uses the adjacent field-reader/parser/GUID-operation/GUID-command fragments and independently assembled case data. Run it and its focused tests only after the build owner closes the active native timing/fixture window:

```powershell
python config/staged/identity/parser_contract_cases_tests.py
python config/staged/identity/prepare_identity_proposal_tests.py
python config/staged/identity/prepare_identity_proposal.py
```

The output changes only `khz_identity.cpp`, its internal header and the fixture dispatcher, adding one native C case file. The existing twenty top-level plugin command registrations remain intact. The internal native diagnostic command is available only with `COD4X1KHZ && KISAK_MP && COD4X1KHZ_IDENTITY_PROBE`; the last macro defaults off. Root owns its build flag and guarded `KHZ_InitIdentityProbe()` initialization hook. Shared `Algorithm`, SHA256, P256 and persistent-key implementations are untouched. The adapter lives in GUID-local helpers, with thread-local scoped fault state and actual CNG cleanup.

Future loaded-DLL parser invocation is `khz_fixture_identity parse`. The generated literal C expectations cover every alias/default instance, allowed universe/account bounds, explicit instances, all decimal universe/type combinations, leading ASCII whitespace/plus, error corrections, and original-length127/128 boundaries. The old dispatcher-refusal harness needs the reviewed additional `|parse` suffix in the next source checkpoint; do not alter a frozen Build74 helper to make an old artifact appear current.

Future GUID command is `khz_identity_guid_provider`, main/control only and outside `com_inServerFrame`, with both benchmark/test-controls required before any crypto call or TLS installation. Four real production GUID calls cover normal before, open failure, derive failure after a real open, and normal after. Both failure scopes also run actual independent SHA256/P256 checks that may not touch GUID counters. Expected cleanup/reset/counter markers must be checked alongside the existing loaded-DLL247 GUID cases; a missing marker or failure is not provider evidence.

`persistence_harness_contract.json` defines a thirty-second three-process native save/reload fixture: independent fresh default1, fresh configured0 with native save, and the exact same saved file reloaded via native `exec` with no set override. It requires exact normal exits and unchanged saved bytes/hash. This is the dedicated explicit-configuration route, not automatic client-profile persistence. The detailed source boundaries and safety adaptations are in `docs/staged-identity-parser-provider-persistence.md`.

The initial three case-data checks passed before the expanded matrix. The expanded data/preparer/native syntax are pending validation after the timing window; neither a generated patch nor passing pure case-data checks establish actual native behavior. Root reviews and authorizes production integration/builds and every scoped native proof.
