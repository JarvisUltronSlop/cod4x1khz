# Staged review of 33 opaque public capability entries

This is a source review, not an implementation claim or semantic proof. It covers the 33 entries marked `resolved: false` in `docs/public-capabilities.json` during Build 73 preparation. It does not change that ledger, its totals, exclusions, evidence, or configuration. Client source is pinned to `17182c7bf04b859dea5c0db4acdb0b83c28d6dd2`; server source is pinned to `ddd65fc`.

None of these entries describes proprietary authentication or joining a legacy protocol server. The six declaration-only entries are inventory errors, not exclusions authorized by those scope choices. All remaining runtime behavior is applicable to the custom pair, even when its opaque entry should be merged into an already named capability. Resolving a source name does not prove the behavior.

The tables below identify every current entry. For dynamic IDs, the full ID is **the prefix in the table heading followed by `dynamic@`, the listed path, `:`, and the listed line number**. The four field-registry IDs are written in full. Paths are relative to the project. Assembly references are source labels, argument-stack offsets, and data-table offsets; no unverified absolute executable address is asserted.

## Client dvars: prefix `client:dvar:` (8 entries)

| Path and current line | Concrete pinned behavior | Candidate inventory resolution |
| --- | --- | --- |
| `upstream/cod4x_client/src/cvar.c:1129` | `Cvar_Register` forwards runtime `var_name`, type, flags, typed value, domain, and description to `Cvar_RegisterNew` when the name is absent; otherwise it re-registers the existing object. | New family ID `client:dvar_registry:Cvar_Register`; keep applicable and pending. |
| `upstream/cod4x_client/src/cvar.c:568` | `Cvar_SetStringByName` updates an existing named dvar, or creates a string dvar with flag `0x4000` and description `External Dvar`. | New family ID `client:dvar_registry:Cvar_SetStringByName`. |
| `upstream/cod4x_client/src/cvar.c:579` | `Cvar_SetBoolByName` performs the corresponding existing-or-create boolean operation. | New family ID `client:dvar_registry:Cvar_SetBoolByName`. |
| `upstream/cod4x_client/src/cvar.c:590` | `Cvar_SetIntByName` creates missing integer dvars over `INT_MIN..INT_MAX`, with flag `0x4000`. | New family ID `client:dvar_registry:Cvar_SetIntByName`. |
| `upstream/cod4x_client/src/cvar.c:601` | `Cvar_SetFloatByName` passes `FLT_MAX, -FLT_MAX` to the registration wrapper for a missing float dvar. That is the actual reversed pinned domain, not a reviewed correct float interval. | New family ID `client:dvar_registry:Cvar_SetFloatByName`; explicitly review a safe domain correction before claiming parity. |
| `upstream/cod4x_client/src/q_shared.h:1077` | Declaration `void Cvar_RegisterBool_f(void);`; there is no registration invocation or argument name here. | Remove this declaration false positive from the dvar inventory after census-parser review. |
| `upstream/cod4x_client/src/q_shared.h:1078` | Declaration `void Cvar_RegisterInt_f(void);`. | Same declaration correction. |
| `upstream/cod4x_client/src/q_shared.h:1079` | Declaration `void Cvar_RegisterFloat_f(void);`. | Same declaration correction. |

The setter definitions are at `cvar.c:560–603`; float registration forwards its supplied domain at `cvar.c:255–266`. The generic registry is at `cvar.c:1057–1132`: it has a 4,096-object limit, typed current/reset/latched values, a hash chain, a critical section, and different borrowed/copied name lifetime according to flags. These are runtime API contracts, not an enumerable list of constant dvar names. The three header declarations repeat at `q_shared.h:1146–1148`; a focused search found no definition or active call for those `_f` names in the pinned client's C/assembly sources. This review does not invent corresponding console-command names.

Needed cases include creation versus re-registration, type/domain/flags, name ownership, latched values, update behavior, bounded capacity, and safe rejection. A fixture that only checks a dvar exists would not cover that family.

## Server commands: prefix `server:command:` (6 entries)

| Path and current line | Concrete pinned behavior | Candidate inventory resolution |
| --- | --- | --- |
| `upstream/cod4x_server/src/cmd.c:371` | `Cmd_AddPCommand` forwards a dynamic name/callback and requested power to `Cmd_AddCommandGeneric`, with warning enabled and null help text. | Merge provenance into a new `server:command_registry:Cmd_AddCommandGeneric` family, retaining wrapper cases. |
| `upstream/cod4x_server/src/cmd.c:376` | `Cmd_AddCommand` uses the same registry with default power 100. | Same family; test default-power behavior independently. |
| `upstream/cod4x_server/src/cmd.c:381` | `Cmd_AddHCommand` forwards null help text despite receiving a help-text argument. This is a pinned implementation quirk. | Same family; describe any intentional help-text correction. |
| `upstream/cod4x_server/src/plugin_exports.c:141` | `Plugin_AddCommand` registers `PHandler_CmdExecute_f`, then sets power and records the plugin's callback/name. | Merge into existing `server:sdk_export:Plugin_AddCommand`; retain dynamic command ownership and unload semantics. |
| `upstream/cod4x_server/src/scr_vm_functions.c:2583` | `GScr_AddScriptCommand` registers a script callback under a runtime command name and requested default power. | Merge into existing `server:gsc_global:addscriptcommand`. |
| `upstream/cod4x_server/src/sv_cmds.c:1510` | `Cmd_AddTranslatedCommand_f` registers the newly stored alias with `Cmd_ExecuteTranslatedCommand_f`, power 100. | Merge into existing `server:command:addCommand`. |

`Cmd_AddCommandGeneric` (`cmd.c:335–366`) checks exact-name duplicates, conditionally warns, copies name/help text, stores callback/minimum power, and prepends a command-list node. `Plugin_AddCommand` is at `plugin_exports.c:126–147`; the pinned per-plugin command array is **20** entries at `plugin_handler.h:142`. The adapted registry's bounded rejection of a 21st command is a safety correction to the upstream unchecked array write, not permission to silently omit a fixture command.

`GScr_AddScriptCommand` (`scr_vm_functions.c:2566–2584`) requires two arguments and a nonempty name. Its usage text states powers 1–100, but this function does not itself validate that interval. `GScr_ScriptCommandCB` (`2546–2564`) requires a running server and passes invoker slot, command name, and a bounded 1,024-byte argument string to `Scr_ScriptCommand`; empty command arguments become an empty string. The stable registration is at `scr_vm_main.c:286`. No matching `addscriptcommand` registration was found in the current custom engine's GSC table during this review; this remains an implementation candidate, not an implemented claim.

Translated commands (`sv_cmds.c:1389–1512`) have 32 slots (`server.h:261`), bounded command/template storage, case-insensitive duplicate detection, and expansion of `$uid`, `$steamid`, `$clnum`, `$pow`, sequential `$arg`, and `$arg:default`. The executor rejects semicolon/newline in supplied argument substitutions and queues the expanded command text. `$uid` is deprecated; numeric Steam formatting can use the custom identity without claiming a service-authenticated account. No translated-command implementation was located in the current custom admin/plugin modules by the focused search. Actual substitution, missing arguments, injection rejection, permission, and removal cases are still required.

## Server dvars: prefix `server:dvar:` (10 entries)

| Path and current line | Concrete pinned behavior / assembly mapping | Candidate inventory resolution |
| --- | --- | --- |
| `upstream/cod4x_server/src/asmsource/_bg_misc.asm:2467` | `BG_RegisterShockVolumeDvars` formats `bg_shock_volume_%s` from each loaded sound-channel name. At the call: `esp=name`, `+4=0.5`, `+8=0`, `+0xc=1`, `+0x10=0x80`, `+0x14=empty description`. | New data-driven family `server:dvar_family:bg_shock_volume_{sound_channel}`; actual loaded-channel cases remain pending. |
| `upstream/cod4x_server/src/asmsource/_con_channels.asm:535` | `Con_InitChannels` registers indices 0–3 of `con_gameMsgWindow%dFilter`. `esi` is the formatted name, `[esp+4]` is the default-list table entry, `[esp+8]=0x29`, and `[esp+0xc]` is the formatted description. | Expand into `server:dvar:con_gameMsgWindow0Filter` through `server:dvar:con_gameMsgWindow3Filter`. |
| `upstream/cod4x_server/src/asmsource/_con_channels.asm:733` | `Con_InitGameMsgChannels` registers the same four names/defaults/flags using the same family, then initializes game-message channel destinations. | Merge provenance into those four entries; do not count a second family. |
| `upstream/cod4x_server/src/asmsource/_g_scr_main_mp.asm:7338` | `GScr_MakeDvarServerInfo`: saved script argument 0 is the name; `edi` is the original value string; arguments are name, value, flags `0x4100`, description. Existing dvars receive added flag `0x100`. | Merge into existing `server:gsc_global:makedvarserverinfo`. |
| `upstream/cod4x_server/src/asmsource/_stringed_hooks.asm:947` | `SEH_UpdateLanguageInfo` obtains the name through `[loc_language]` then the dvar's first pointer field `[eax]`. Arguments are that name, default 0, min 0, max 14, flags `0x21`, description `Language`. | Merge into existing `server:dvar:loc_language`. |
| `upstream/cod4x_server/src/asmsource/_stringed_hooks.asm:954` | The same function reads `[loc_forceEnglish]` then `[eax]`; bool default 0, flags `0x21`, force-English description. | Merge into existing `server:dvar:loc_forceEnglish`. |
| `upstream/cod4x_server/src/cvar.h:160` | Declaration `void Cvar_RegisterBool_f(void);`. | Remove declaration false positive after parser review. |
| `upstream/cod4x_server/src/cvar.h:161` | Declaration `void Cvar_RegisterInt_f(void);`. | Same declaration correction. |
| `upstream/cod4x_server/src/cvar.h:162` | Declaration `void Cvar_RegisterFloat_f(void);`. | Same declaration correction. |
| `upstream/cod4x_server/src/scr_vm_functions.c:2401` | `GScr_MakeCvarServerInfo` creates a runtime string dvar with flags `0x4100`, or adds `0x100` to an existing one. | Merge with the assembly call into existing `server:gsc_global:makedvarserverinfo`. |

The shellshock loop is `_bg_misc.asm:2443–2482`, with the format string at line 3063. The foundation already has `BG_RegisterShockVolumeDvars` in `engine/src/bgame/bg_misc.cpp:208–227`, but its loaded channel names and actual sound-volume response still need semantic cases. Do not turn unknown asset-dependent names into invented fixed entries.

The console loops are `_con_channels.asm:516–546` and `706–748`; defaults at line 842 are `gamenotify obituary`, `boldgame`, `subtitle`, and the empty list. Format/description data is at lines 876–877. The foundation equivalent is `engine/src/client/con_channels.cpp:123–167`, including destinations 3–6 and archive/systeminfo/latch flags. These are client/HUD semantics even though the pinned source inventory places their assembly in the server domain. Their named dvar IDs are not currently present in the ledger. A semantic fixture must check filter routing and latch/archive behavior, not merely retrieve four strings.

Language strings are identified at `_stringed_hooks.asm:1701` and `1703`; the two calls in `SEH_UpdateLanguageInfo` are re-registration through real dvar name pointers, not arbitrary names. Foundation initialization and re-registration appear at `engine/src/stringed/stringed_hooks.cpp:61–62` and `566–567`.

The C script dvar body is `scr_vm_functions.c:2366–2402`; the assembly body is `_g_scr_main_mp.asm:7279–7360`. Both build a cleaned temporary string but pass the **original** value string to registration. Do not describe pinned behavior as successfully stripping quotes. Preserve numeric flag mappings from these game-dvar calls rather than applying the unrelated Q3-style flag numbers from another header. Current custom registration exists at `engine/src/game_mp/g_scr_main_mp.cpp:216`; actual serverinfo propagation, value boundaries, and injection safety remain separate pending behavior.

## Script fields and registries (7 entries)

| Current ID / source | Concrete behavior | Candidate inventory resolution |
| --- | --- | --- |
| `server:gsc_field:dynamic@upstream/cod4x_server/src/asmsource/_g_hudelem.asm:2469` | `GScr_AddFieldsForHudElems` walks the `fields` table with stride `0x1c`, passes class 1, name pointer, and the computed 16-bit row index to `Scr_AddClassField`. | Merge provenance into the 19 existing `server:gsc_field:_g_hudelem:<name>` entries listed below. |
| `server:gsc_field:dynamic@upstream/cod4x_server/src/asmsource/_g_spawn_mp.asm:1628` | `GScr_AddFieldsForEntity` walks `ent_fields` with stride `0x10`, passes class 0/name/16-bit row index, then tail-calls client-field registration. | Merge provenance into the 10 existing `server:gsc_field:_g_spawn_mp:<name>` entries and retain the client-registration edge. |
| `server:gsc_field:dynamic@upstream/cod4x_server/src/g_client_fields.cpp:460` | `GScr_AddFieldsForClient` passes class 0/name and `(uint16_t)(f-fields) \| ENTFIELD_MASK`, where the mask is `0xc000`. | Merge into the 20 existing `server:gsc_field:<name>` client entries below. |
| `server:gsc_field_registry:GScr_AddFieldsForEntity` | `scr_vm_main.c:49` is a declaration; the actual MP initialization call is line 783. The concrete entity table/registration is `_g_spawn_mp.asm:1610–1640`. | Resolve the finite entity/client field families above; keep their individual semantics pending. |
| `server:gsc_field_registry:GScr_AddFieldsForHudElems` | Declaration at `scr_vm.h:775`, actual MP init at `scr_vm_main.c:785`, finite table at `_g_hudelem.asm:2536`. | Resolve the 19 finite HUD field names; retain individual behavior. |
| `server:gsc_field_registry:GScr_AddFieldsForRadiant` | Declaration at `scr_vm.h:776`, actual MP init at `scr_vm_main.c:786`. `_g_spawn_mp.asm:1362–1370` calls `Scr_AddFields("radiant", "txt")`. | Resolve to the data-driven runtime-key loader contract, not an editor exclusion. |
| `server:gsc_field_registry:Scr_AddFieldsForFile` | Current provenance `cscr_variable.cpp:3741` is the fastfile caller; the loader is `3643–3730`. Fastfile loads `radiant/keys.txt`; load-object path enumerates files by directory/extension. | Keep an explicit applicable data-driven registry, e.g. `server:gsc_field_registry:RadiantRuntimeKeys`, with both source-loading modes mapped. |

The finite named table mappings are:

| Family / exact name suffixes | Pinned definition and semantic distinctions |
| --- | --- |
| `server:gsc_field:_g_hudelem:` — `x`, `y`, `z`, `fontscale`, `font`, `alignx`, `aligny`, `horzalign`, `vertalign`, `color`, `alpha`, `label`, `sort`, `foreground`, `hidewhendead`, `hidewheninmenu`, `glowcolor`, `glowalpha`, `archived` | `_g_hudelem.asm:2536` table, registration `2440–2480`; type/getter/setter/bitmask behavior is supplied per row. There are 19 names, not one generic HUD-field capability. |
| `server:gsc_field:_g_spawn_mp:` — `classname`, `origin`, `model`, `spawnflags`, `target`, `targetname`, `count`, `health`, `dmg`, `angles` | `_g_spawn_mp.asm:1649` table; `classname`, `model`, `spawnflags` use read-only setters; origin/angles/health have specific native setters, with generic field access for other rows. |
| `server:gsc_field:` — `sessionteam`, `name`, `maxhealth`, `headiconteam`, `kills`, `deaths`, `assists`, `killcamentity`, `score`, `sessionstate`, `statusicon`, `spectatorclient`, `archivetime`, `psoffsettime`, `pers`, `headicon`, `groundentity`, `hasradar`, `isbot`, `localization` | `g_client_fields.cpp:422–445` table, registration `451–463`, mask at line 12. `pers`, `groundentity`, `isbot`, `localization` are read-only; the other rows include specific enum/name/icon/timebase/score setters. |

Those 49 named field IDs are already represented in the ledger. Merging opaque provenance must not replace them with one registry-exists assertion. Relevant foundation seams are `engine/src/game/g_client_fields.cpp:26` and `415`, `engine/src/game_mp/g_spawn_mp.cpp:179`, and `engine/src/game_mp/g_scr_main_mp.cpp:508`.

The runtime key loader recognizes `float`, `int`, `string`, and `vector`, lowercases/canonicalizes names, rejects unknown types, duplicate keys, and missing field names, and appends packed type/name-ID records using temporary script storage. `cscr_variable.cpp:3734–3742` covers fastfile input; `3750–3764` covers enumerated load-object input. Foundation equivalents are in `engine/src/script/scr_variable.cpp:3614`, `3710`, and `1037`. The actual stock key file and loaded key names were **not** enumerated in this review. Its multiplayer initialization call is sufficient to rule out treating it as deferred editor-only work.

## Plugin script registration (2 entries)

| Current ID | Concrete pinned behavior | Candidate inventory resolution |
| --- | --- | --- |
| `server:gsc_global:dynamic@upstream/cod4x_server/src/plugin_internal.c:502` | `PHandler_ScrAddFunction` registers a name and executable callback stub as a GSC global, with replacement/removal handling. | Merge into existing `server:sdk_export:Plugin_ScrAddFunction` and `server:sdk_export:Plugin_ScrReplaceFunction`. |
| `server:gsc_method:dynamic@upstream/cod4x_server/src/plugin_internal.c:551` | `PHandler_ScrAddMethod` performs the corresponding GSC method registration. | Merge into existing `server:sdk_export:Plugin_ScrAddMethod` and `server:sdk_export:Plugin_ScrReplaceMethod`. |

Bodies are at `plugin_internal.c:464–513` and `516–563`; stub construction is at `433–463`. There are **64 shared function/method slots per plugin** (`plugin_handler.h:179–184`), not 64 independent slots for each kind. Registration validates loaded plugin ownership, uses bounded stub names, distinguishes method/global, and sets/restores callback ownership through the stub. The current adapted registry (`native/plugin_registry.hpp:86–88`) also uses a shared 64-slot owner limit; native exports are at `engine/src/khz/khz_plugins.cpp:673–676`.

The rebuilt-SDK requirement permits replacing raw generated executable stubs with preallocated typed thunks. It does not waive duplicate/replacement behavior, shared limits, VM call results, plugin ownership, callback lifetime, or safe disable/unload. Historical native function/method fixture results exist, but this document neither rebinds them to Build 73 nor promotes a public proof.

## Review disposition and next changes

The 33 opaque records comprise six mistaken declaration matches and 27 runtime/provenance records. No user-scope exclusion is proposed. The parser should reject declaration-only call matches; reviewed duplicates should retain provenance on stable named contracts; finite dvar/field tables should expand to their actual names; genuinely data-driven registries should retain explicit family IDs and semantic requirements. Root must review the candidate IDs before changing the manifest or recomputing totals.

The server Makefile includes all `src/asmsource/*.asm` via its wildcard at line 126, maps their objects at line 134, links them at line 199, and has the assembly rule at line 230. An assembly source entry cannot be dismissed as dead code merely because its implementation is not a C definition. Conversely, presence in a build list is not runtime semantic evidence.

A practical fixture order is: (1) named language/console filter behavior and typed dynamic dvar creation; (2) script-created serverinfo and script commands, including injection and power checks; (3) translated aliases; (4) individual native field getters/setters/read-only cases and the actual stock runtime-key file; (5) plugin registration/replacement/shared limits and unload ownership. Asset-dependent shellshock channels need their own loaded-map/audio fixture. Every batch must be bound to an immutable actual build and retain honest failures and pending cases.

This staged review ran no build, game, benchmark, hash sweep, census, or proof publication. It records source interpretation only.
