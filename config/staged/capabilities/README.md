# Pending inventory corrections

These files are staged proposals only. Production `scripts/capabilities.py`, configuration, manifest totals, and public evidence are unchanged. No tests or hashes were executed while the Build 73 live performance batch ran.

`registration_call_filter.py` proposes replacing the current inline prototype test after `balanced()`/`arguments()` inside `census()`. It excludes confident declarations/definitions, including the six `(void)` declaration false positives. Assignments, return/nested/conditional calls, real same-name calls, and uncertain macro calls remain discovery candidates. SDK and field declaration inventories remain separately responsible for declarations; this predicate does not remove them from those inventories.

`registration_call_filter_tests.py` is a focused standalone test proposal that reuses production masking and balanced-call helpers, then applies the staged predicate. It includes multiline qualifiers, unknown return types, pointers, calling conventions, attributes, same-name calls, assignments, conditional/nested casts, field declarations and actual calls, and inactive/comment/literal negatives. The proposal deliberately keeps uncertain syntax rather than claiming to parse all C/C++.

`finite-registration-resolution.json` explicitly enrolls the 49 existing named HUD/entity/client field IDs and four newly resolved console-filter dvars. Every resulting semantic status remains `pending`. It includes the source table/registration coordinates and the original opaque IDs to merge. Removal of a declaration-only scanner artifact is an inventory correction, not a user-scope exclusion or proof.

After the build owner ends the live window, run:

```powershell
python config/staged/capabilities/registration_call_filter_tests.py
python tests/plugin_identity_contract_harness_tests.py
```

Root reviews both source mapping and actual focused results before applying the production scanner or resolution enrollment. The complete source review is in `docs/staged-opaque-capability-review.md`.
