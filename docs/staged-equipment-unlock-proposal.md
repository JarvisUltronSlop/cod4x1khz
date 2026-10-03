# Staged connected-unlock and equipment diagnostic

This is an integration proposal, not an implemented endpoint or a live proof.
Build 73 and its native sources remain unchanged. The completed short bot matrix
is separately recorded in [the validation supplement](../results/build73-native-bot-matrix-validation.json).
Its skill/team/control evidence does not establish weapon, combat or objective
acceptance.

## Native data must stay separate

The client profile class IDs are indices into loaded stock stat tables. They are
not indices into `bg_weaponDefs`. A profile primary ID of 25 and a current native
weapon index of 25 are not interchangeable evidence. Compare resolved native
weapon names and model variants against the actual class/table references.

Three sources are required, with their own lifetime metadata:

1. Client profile: actual `LiveStorage_GetStatBuffer()` values and the stock
   class references already exposed by `KHZ_ProfileStatusJSON`. Read this on the
   main owner, using the acknowledged control barrier for connected calls.
2. Client gameplay: the private owner's accepted authoritative snapshot and
   predicted player state, copied during an acknowledged barrier. Include actual
   snapshot message/time and player command time; a renderer's weapon model or
   a configured class cannot substitute for either state.
3. Dedicated authority: received `client_t::stats`, native live `gclient_s::ps`
   and the admitted entity. Only the server can prove that an equipped loadout
   belongs to the validated human admission and has actually spawned.

The server receives class selections during its stats handshake. In
`win_storage.cpp:564`, `LiveStorage_UploadStats()` writes local encrypted mpdata;
it does not resend updated class bytes to an already active server. Stock
`_class::cac_getdata` also caches initialized custom classes. Consequently, a
Create-a-Class action performed after the unlock reconnect needs another real
native reconnect before its server class selection can be proved. Neither local
archive readback nor `all_unlocks=true` proves refreshed server stats.

## Proposed dedicated endpoint

Add new `khz_equipment_inspection.{h,cpp}` only after root approves integration.
The proposed command is:

```
khz_equipment_inspect <slot> <expected-session> <expected-human-generation>
KHZ_EQUIPMENT_INSPECTION { ... }
```

All numeric arguments are strictly bounded. Calls require the actual dedicated
main owner, benchmark and test-control startup flags, an initialized `SS_GAME`
server, a completed tick, `!com_inServerFrame`, no initializing/spawning client,
and a current nonzero custom session. Refuse bots, foreign callers, inactive or
disconnected slots, incomplete stats packets, stale session/generation and
entity/client storage disagreement. Reuse the reviewed identity predicates of
`khz_bot_inspection.inl:129`, without copying its unrelated skill queries.
Specifically, require `CS_ACTIVE`, `CON_CONNECTED`, matching
`client->gentity == &g_entities[slot]`, `entity.client == &level.clients[slot]`,
entity number/clientNum equal to slot, and a positive
`KHZ_AdminClientGeneration(client)`. Keep generation namespaces separate.
`client_t::statPacketsReceived` must be 127 before treating received stats as a
complete class source. Reject an intermission or unspawned player as live
equipment acceptance; a diagnostic may retain its observed state separately.

Copy a fixed bounded record, then recheck tick/session/serverId/admission before
formatting it. No allocation, file I/O, registering weapons, script setters,
native `GiveWeapon`, or synthetic loadout is permitted in this reader. If typed
script class fields are added, require the reviewed idle-VM predicate and read
only existing typed children. Do not call allocating entity/variable getters.

Proposed record fields:

| Group | Actual source and checks |
|---|---|
| Identity | schema, complete/reason, process ID, session, serverId, tick/server time, slot, human generation, entity use count, connect/spawn time, native connection/team/life/health |
| Received profile | stats packet mask, complete received-stat payload hash, class bytes 201..249 and XP2301; these are received values, not client claims |
| Weapon ownership | exact four `ps.weapons` words; enumerate only set indices below both `BG_GetNumWeapons()` and the actual 128-entry arrays |
| Weapon references | bounded copied `WeaponDef::szInternalName`, native index, inventory/offhand type, alternate index; never use `BG_GetWeaponIndexForName` to register missing assets |
| Current state | `ps.weapon`, `weaponstate`, `weaponShotCount`, `offHandIndex`, `offhandSecondary`; current/offhand indices must be checked before dereferencing |
| Ammo | all actual `ps.ammo[128]` and `ps.ammoclip[128]`; per-owned-weapon `iAmmoIndex/iClipIndex` must be validated before indexing, preserving shared ammo/clip identities |
| Perks | raw `ps.perks`, bounded native bit indices/names from `bg_perkNames[PERK_COUNT]`; profile perk stat IDs are not these native bit indices |
| Models/camo | exact `ps.weaponmodels[128]` and model variant for each owned weapon, plus bounded selected gun/world model names if nonnull |
| Optional class trace | actual typed `pers["class"]`, current class and native spawn choice, clearly identifying the script object's source and not assuming a UI label proves the choice |

`WeaponDef::gunXModel` and `worldModel` each contain **16** elements
(`xanim.h:468/560`). Validate the actual model byte against those array lengths
before a lookup. A byte's numeric range alone cannot authorize the dereference.
Null optional models can be reported as null; do not silently substitute model0
and claim that the selected camo was applied. Use a checked JSON writer with
bounded escaped strings and reject truncation. Retain at most one human per
response so the result remains below the existing 65,536-byte diagnostic limit.
Every response explicitly keeps full-world and complete weapon acceptance false.

Root owns runtime/CMake registration. A's separately staged `khz_archive_probe`
adds a distinct admin allowlist entry; equipment registration must be added
without replacing that entry or broadening unrelated command eligibility.

## Client audit seams still needed

The existing transaction prints equal preserved-stat hashes, and its candidate
loop checks every unaffected stat. An independent fixture needs the actual
before/candidate values, not just a 32-bit aggregate hash. Propose a bounded,
benchmark-only audit snapshot recorded inside the real `CommitUnlocks` transaction:
original 8,192-byte stats, actual candidate bytes, 3,498 stock masks, minimum XP,
profile/home identity and backup basename. Export only this immutable completed
record off gameplay ticks into its own confined benchmark home; include exact
lengths/SHA256 and reject stale identity or truncation. The snapshot must capture
the transaction candidate before later ordinary gameplay stats can change it.
The audit is diagnostic storage, not a side channel that grants entitlements.

The fixture compares every stat index 0..3497. Only exact nonzero stock masks and
XP2301 may change; every other stat and every unmasked bit must stay identical.
Validate the persisted unlock record's checksum and stock table contract.
Backups must already be complete before commit, with their actual profile and
memory checksum. Preserve header/checksum handling separately from stat payload
equality when testing restore. Existing `native_profile_probe.py` supplies the
reviewed offset/backup checks but will remain unchanged.

A client equipment reader should copy actual accepted/predicted PS only through
the real private-owner barrier. Include owner generation and accepted session;
do not read the presentation mirror concurrently with its worker. It can return
the same bounded PS equipment fields as the dedicated endpoint, tagged with
`source=accepted_snapshot` or `source=predicted_playerstate`. Test agreement only
after a settled actual spawn and fresh snapshots. Missing observations remain
missing evidence rather than being inferred from `cg.weaponSelect`.

## New isolated fixture sequence

Use a fresh fixture-owned benchmark home, one human and zero bots, a private
native TDM match on Crash, no plugins/downloads, developer0. Preserve previous
benchmark homes and hash interactive preferences. Never import D: or interactive
mpdata. First retain the locked profile and actual active human's received stats,
equipment, admission generation and positive command/snapshot acknowledgements.

1. Invoke the real connected `khz_ui unlockall`. Require exactly one completed
   native transaction and backup, a normal reconnect to the same numeric endpoint
   and exact child handle, a newer human admission and private-owner generation,
   full stats reception, and recovered actual ack/snapshot/prediction/pose/GPU
   progress. Session/serverId should remain the same on this unchanged server
   map. Check every unaffected transaction stat from the completed native audit.
2. Invoke connected unlock again. Its existing handler returns early for saved
   unlocks. Require no second backup/commit/reconnect, unchanged admitted lifetime
   and identical unlock payload. A queued command reply is not this proof.
3. Use observed enabled stock Create-a-Class actions for custom1. Choose an actual
   supported primary/attachment/camo, sidearm, grenade and perk combination;
   retain every compiled action and actual class IDs/references. Resolve allowed
   combinations using the loaded tables and `_class::cac_getdata` validation.
   Avoid grip/GL-perk and smoke/extra-special-grenade combinations in the first
   case; those normalize deliberately and need independent cases.
4. Complete a real native `reconnect` after local class changes. The normal
   `CL_Reconnect_f` queues `connect <numeric-name> reconnect`, preserving the
   owned child; the fixture must observe newer admissions and stats201..209
   exactly matching the saved class. Current `khz_ui console` does **not** admit
   reconnect, so a narrow gated diagnostic allowlist addition or reviewed native
   menu action is needed. Do not invent this as existing test capability.
5. Inspect the actual loaded in-game class menu. Activate its enabled custom1
   compiled `scriptmenuresponse` action. Native `Script_ScriptMenuResponse`
   resolves the real `CS_SCRIPT_MENUS` index and queues serverId-bound `cmd mr`;
   preserve that path. If the compiled custom action is absent, report the gap
   rather than directly injecting `GiveWeapon` or treating automatic assault
   selection as a custom-class test.
6. Require an actual live custom1 spawn. Stock `menuClass` may only apply a changed
   class immediately during grace; otherwise it is pending until respawn. A
   controlled native kill is permitted only through an existing validated native
   command path, with recorded old/dead/new health and increasing native spawn
   time. If neither natural nor validated respawn occurs within the budget,
   report missing spawn proof. Never fake a spawn timestamp.
7. Compare server inventory names, model/camo variant, native perk bits and
   positive ammo/clip values with the received class/table choices. Separately
   retain client accepted/predicted equipment and fresh authoritative snapshots.
   A selected current weapon alone cannot prove sidearm/perk/grenade/camo
   ownership. Controlled firing/reload/projectile/damage outcomes remain false.
8. Leave the exact host normally. Restore the original native backup from the
   observed disconnected main menu using `restoreunlocks`; require payload
   equality and locked status. Normally Quit/relaunch the same isolated profile
   to prove persistence of the restored baseline before any setters, then close
   only created clients. Preserve all backups and failure evidence in results,
   restore the prior benchmark home, and require unchanged interactive hashes.

A subsequent unlocked persistence relaunch can prove saved unlocks independently
before restoration. It must occur before any unlock setter and must retain its
same-profile directory. The fixture should stay bounded (approximately 60–90s
after startup with at most 30s per recovery), stop dependent checks on native
error, and keep failed initial artifacts separate from corrected fixture reruns.
No live launch is authorized by this proposal itself.
