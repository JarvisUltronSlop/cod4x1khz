# Native mode inventory adaptation

This is a staged fixture integration recipe. It changes no packaged/native
inputs and launches no game. Build 70 adds `game_types_valid` and `game_types`
to the actual `khz_ui list` response, using `sharedUiInfo.numGameTypes` and
`sharedUiInfo.gameTypes[i].gameType`. The native array has capacity 32 in
`engine/src/ui_mp/ui_mp.h`. The writer is in
`engine/src/khz/khz_ui.cpp`; the same typed entries are used by stock StartServer
and the private `khz_ui host` adapter.

Request the exact list before opening bot menus or hosting:

```
khz_ui list
```

Retain the complete reply beside the fixture's actual process/session/profile
metadata. Require `result == "queried"`, `game_types_valid is True`, and a
nonempty string array with at most 32 entries. Validate each identifier as a
bounded nonempty native game-type token, reject duplicate identifiers under
the native case-insensitive comparison, and require the selected case's
identifier to appear before issuing StartServer. Do not substitute a
hard-coded stock list when the actual list is unavailable or malformed.

The current fixture cases are `dm`, `war`, `sd`, `sab`, `dom`, `koth`. For an
all-six presence gate, require every one of those six to occur in the actual
reply. Report observed extra types separately. A `settings_quick_ctf` menu is
not proof that `ctf` appears in this loaded catalog, and the fixture should
not synthesize a CTF case based on the menu's existence.

Suggested next script seam in `scripts/native_bot_probe.py`:

```
reply = fixture.call("khz_ui list")
# Validate native flag, bounded identifiers, uniqueness and selected mode.
fixture.report["actual_mode_inventory"] = {
    "reply": reply,
    "selected_mode_present": case["gametype"] in reply["game_types"],
    "required_stock_modes_observed": all(mode in reply["game_types"]
                                         for mode in MODES),
    "stock_mode_widget_selection_verified": False,
    "all_mode_gameplay_verified": False,
    "all_objectives_verified": False,
}
```

Add independent validator cases for missing/false validity flags, non-list or
oversized data, empty or invalid tokens, case-insensitive duplicates, missing
selected mode, and valid arrays containing all six stock identifiers plus a
separately reported custom identifier. Test execution and native cases must
remain outside coordinated rendering/performance measurements.

`khz_ui status` and other UI replies also carry these fields, but their
`game_types` array is intentionally empty: only the exact `list` action emits
entries. The actual Regular proof used status/menu/host calls and did not
retain a list reply. Its empty arrays therefore neither prove an empty
catalog nor satisfy the new presence gate.

The native validity flag checks the original count against array capacity;
it is not a formatter-completeness attestation. The JSON writer can stop
emitting types when its bounded buffer has insufficient room. The current
schema does not emit `numGameTypes` or an emitted-count/completeness flag.
Consequently an observed entry proves presence, and observing all six proves
those six were available. An absent entry alone cannot prove that a type is
not installed. For a future complete catalog/absence claim, add actual native
count and emitted-count/completeness metadata in a separately reviewed build
and reject any truncated response.

The host adapter looks up the actual typed map/mode entries and calls native
StartServer. That establishes loaded selection and hosting, not a user's
stock game-type widget path. Continue to require real stock-widget key/action
changes before claiming that product flow. Mode loading, native team
membership, team balance, bot goal/objective selection, objective completion
and elapsed AI difficulty behavior remain separate native evidence gates.
