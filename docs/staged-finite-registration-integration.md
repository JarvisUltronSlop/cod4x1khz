# Reviewed finite-registration enrollment

This layer resolves source identity only and grants no semantic status or exclusion. Root accepted its actual-source dry-run and authorized script-only production integration. `scripts/finite_registration_enrollment.py` is imported by `scripts/capabilities.py` immediately after raw aggregation and before configuration/evidence validation. All49 field IDs already existed; the four console-filter names were absent. The six prototype artifacts were removed independently and remain outside this enrollment.

## Exact source families

| Family | Verified native registration/table mapping | Reviewed pinned file SHA256 |
| --- | --- | --- |
| 19 HUD fields | `_g_hudelem.asm:2536` is `fields`, seven 32-bit words per row (`0x1c` stride); registration at2469 passes class1 and row index. Exact tail is a seven-word null row plus four padding words. Existing ID prefix `server:gsc_field:_g_hudelem:`. | `9ba8cc2682225f9c1ee38d6a05c56a0295e7c470fb2cd85d7550cae77658a11f` |
| 10 entity fields | `_g_spawn_mp.asm:1649` is `ent_fields`, four words per row (`0x10` stride); registration at1628 passes class0/index. Exact tail is a four-word null row plus four padding words. The function then jumps to client-field registration. Existing prefix `server:gsc_field:_g_spawn_mp:`. | `9f365ea26b1ef5c08bdbaea8c1f9a352f07f5b050855fb00b92bd38b2f27c38d` |
| 20 client fields | `g_client_fields.cpp:422–445`, registration460 passes class0 and index OR`0xc000`. Terminal null-name row is not a field. Existing prefix `server:gsc_field:`. | `4ade79eb14277621bb45275e3b81953262df50837b3bf0a252bea64e4cd74382` |
| Four console filters | `_con_channels.asm:535` and733 use format `con_gameMsgWindow%dFilter` at876, loop indices0..3, defaults table842 and flags`0x29`. Both call sites describe one four-name family. New IDs `server:dvar:con_gameMsgWindow0Filter`..`3Filter`. | `1ea388e11dd2709f4fbefc7ee66af9fc2f8b4622ec2312a395424bc9e09f0c66` |

These exact hashes/layouts/call coordinates passed actual-source validation in `results/build73-staged-finite-source-dryrun.json`. The filter parser scopes the reviewed call to its actual bounded loop: `Con_InitChannels` has another named string registration at497 that remains unchanged. Blank-line-leading regex matches and trailing ASM comments cannot shift the registration coordinate or fake the entity tail edge. Exact names/opaque IDs are listed in the staged JSON; full behavior review remains `docs/staged-opaque-capability-review.md`.

## Safe integration

Add a small reviewed resolution layer after raw registration discovery and before final aggregation. Keep raw entries available. Each enrollment must carry the exact pinned revision/file hash, registration symbol/line, table or format symbol, complete expected name list, class/row layout or loop bound, and the opaque IDs it consumes. A file-hash change, missing call/table/format, changed layout/loop limit, missing/duplicate name, unexpected name, or absent opaque ID must leave the source opaque and report a review error. Never silently trust stale line numbers or accept a free-form name list as proof.

The production insertion is exactly:

```python
capabilities, finite_errors = enroll_finite_registrations(
    capabilities, root, config["upstreams"].get("server", {}))
errors.extend(finite_errors)
```

All families validate before the returned dictionary changes. The caller's raw dictionary is unchanged; no free-form configuration can override the fixed reviewed consumption sets or status. The existing `verify_evidence` still computes every semantic status afterwards. Future fixture tool provenance must independently hash the imported module as well as the scanner.

For the three field tables, compare all parsed table names to the reviewed finite list and existing namespaced IDs. Append the actual runtime registration and registry-initialization provenance to every affected named field. Then retire only the five duplicated opaque field records: three dynamic registration sites plus `GScr_AddFieldsForEntity` and `GScr_AddFieldsForHudElems`. Preserve each field's kind, individual setter/getter metadata and pending semantic requirement. The entity registry's tail jump to `GScr_AddFieldsForClient` must also remain an explicit registration edge to the client family; do not flatten client masked offsets into entity offsets.

For console filters, independently resolve both formatted call sites from the same format/default table and the proven0..3 bound. Create four named, `resolved:true`, `applicable:true`, **pending** dvar entries, each with both initialization call provenances. Retire only the two duplicated opaque console call records. Preserve defaults `gamenotify obituary`, `boldgame`, `subtitle`, empty and numeric flags`0x29`. Those source-discovery values do not prove filtering or configuration behavior.

Given the verified presence of all49 existing fields and absence of the four filters, this narrow change should remove seven opaque records, add four named entries, and reduce total capability count by three. From root's corrected2120/27 baseline, the expected inventory would be2117 total and20 unresolved; existing semantic proven/pending counts must be recalculated from actual evidence, not copied or increased by enrollment. A diff must show exactly those seven removals/four additions and provenance updates to the49 existing fields. No exclusion should be introduced.

Keep `GScr_AddFieldsForRadiant`/`Scr_AddFieldsForFile` opaque pending their asset-driven runtime-key family review. This finite table enrollment does not resolve those two loaders, shellshock channel names, generic command/dvar registries, plugin registration lifetime, or other remaining dynamic entries.

## Focused regression requirements before application

Tests should use small isolated source strings and reviewed enrollment records, without runtime assets or game launch:

1. Exact19/10/20 tables produce the existing49 IDs, correct class/row-mask mappings, merged provenance, and no new proven status. Entity-to-client registration edge survives.
2. The two console call sites expand to precisely four names once, all pending, with both call provenances and exact defaults/flags.
3. Mutate name, count, sentinel, stride, field mask, call class, format, loop bound, default table, file hash, revision, or source registration coordinate: resolution fails closed, leaving the opaque requirement visible.
4. Reject duplicate/missing/extra names, unrelated consumed IDs, cross-family IDs, and attempted `proven`/`excluded` statuses in an inventory resolution.
5. Declaration-filter correction remains separate; SDK/field inventories still retain their actual declaration definitions. Existing proof validity continues to require current implementation/binary/fixture evidence.
6. Compare against the preserved manifest: seven specified opaque removals, four named additions,49 provenance updates, all unrelated entries and claims unchanged.

The console source resides in the server upstream inventory, while its operational UI filtering is client-facing in the shared foundation. Preserve the existing manifest domain convention, but future semantic evidence must explicitly bind the actual rendered client and relevant pair binaries and test native HUD/channel behavior. A dedicated-server dvar-exists query alone cannot establish that contract.

## Actual offline validation

The production module passed twelve isolated source/enrollment tests, six tests against the actual pinned source bytes, and all five existing scanner tests. Logs are `results/build73-finite-production-unit-tests.log`, `build73-finite-production-native-source-tests.log`, and `build73-finite-production-scanner-tests.log`. The pre-integration real-source dry-run verified exactly7 removals/4 additions/49 provenance changes,2117 total/20 unresolved, with four old proof statuses preserved. Native tests additionally reject altered padding, comment-only fake guards, changed tail edges, loop bounds and displaced call coordinates.

The actual production regeneration `results/build73-finite-production-census.log` has no inventory errors and no exclusions; its release gate correctly remains false. It reports2117 pending/0 proven because the independently changed `khz_plugins.cpp` source for the pending Build74 no longer matches the four historical Build73 proof hashes. That source invalidation is unrelated to enrollment and must not be waived. Configuration was left byte-identical. The prior2120 JSON/Markdown ledger and configuration are preserved at `results/build73-capabilities-before-finite-enrollment.{json,md}` and `results/build73-evidence-config-before-finite-enrollment.json`.
