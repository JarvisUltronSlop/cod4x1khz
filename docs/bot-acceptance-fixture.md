# Short native bot acceptance evidence

`scripts/native_bot_probe.py` has passed one inspected Build 70 case. Its default is one
Regular, Killhouse, DM match with one actual rendered human and fifteen native
bots. Each retained command tape contains exactly 1,000 authoritative ticks;
there is no ten-minute interval or performance claim.

```powershell
python scripts/native_bot_probe.py --case regular_dm
python scripts/native_bot_probe.py --matrix
```

The matrix has ten separate isolated cases: Recruit/Regular/Hardened/Veteran DM,
Regular TDM/S&D/Sabotage/Domination/Headquarters, and DM with zero bots. The
default also exercises Bot Count 0/15 and all four difficulty ordinals through
each of the seven existing stock settings screens. CTF has a settings screen,
but the fixture does not assume the actual loaded game-type catalog contains
CTF. `--no-menu-matrix` narrows menu coverage to the selected mode.

Every key must return `stock key dispatched` and produce its exact actual
`ui.bot_count` or `ui.bot_difficulty` transition, including wraparound. Even an
already-selected default gets a reversible right/left outcome. The final value
must appear in the actual archived benchmark profile. There is no invented
`handled=true` requirement: native `Menu_HandleKey` returns void. A dispatch
message or compiled action alone is not a semantic selection proof.

Map/mode creation currently uses the existing `khz_ui host` adapter, which
validates actual typed map and game-type entries and invokes native StartServer.
It sets the *same* bot settings already established through native menu keys.
This is not evidence that a user selected the game type through a stock
game-type widget. An actual typed catalog/UI action seam remains necessary for
that narrower product-flow proof.

The fixture opens an exact owned dedicated-process handle, verifies actual
native CA_ACTIVE/cgame/private-owner state, admitted human commands and
snapshots, bot control generations, and advancing rendering/prediction/pose.
It retains the immutable native JSONL tape with the export's exact byte count
and digest, and validates every slot in every tick. Observations include actual
movement, command weapon/offhand values, button runs/rising edges, and native
`last_spawn_time` changes. These prove selected commands and observed spawn-time
changes; they do not prove firing, damage, reload completion, reaction latency,
or objective completion. Absence of a particular action in one second is
recorded rather than fabricated into a successful gameplay test.

One to three separate tapes can be requested with `--captures 1..3`. Native
RCON is paced at 1.1 seconds, readiness is bounded to thirty seconds, and each
case has a ninety-second overall limit by default. Native Leave must reach the
stock frontend and stop the exact child before Quit. Cleanup preserves the
previous benchmark profile; any forced termination fails the case. Another
case is never started after failed cleanup or an engine error.

## Actual VM/team inspection: Build 70 integration

`engine/src/khz/khz_bot_inspection.h/.inl` exports
`const char* KHZ_BotInspectionJSON()`. The fragment is included at the end of
`khz_bots.cpp` under the dedicated custom build guard. Build 70 adds the real
dedicated runtime dispatch and explicit benchmark/test-controls inheritance.
The deployed Build 70 fixture requires the exact `KHZ_BOT_INSPECTION` endpoint
when explicitly selected:

```powershell
python scripts/native_bot_probe.py --case regular_dm --captures 1 --require-inspection --no-menu-matrix --client-port 28992 --case-timeout 90
```

The endpoint must be dispatched on the completed dedicated owner outside a
server tick with both `khz_benchmark` and `khz_test_controls` enabled. The owned
child must explicitly inherit both controls from the selected private fixture;
ordinary managed matches do not pass this diagnostic guard.

The fragment checks actual VM stack/frame/parameter/error/initialization state,
native client/entity pointers, and active admission generations. It validates
the entity class-table object separately from the persistent array:
`client_t.scriptId == gclient.sess.scriptPersId`. The stock native `pers` field
is an `F_OBJECT` at that session offset in `game/g_client_fields.cpp`; it is not
an ordinary entity `FindVariable` field. It follows the real
`pers["bots"]["skill"]` arrays using typed, nonallocating native lookups and
reads `base`, `aim_time`, `init_react_time`, `reaction_time`, `remember_time`,
and `fov`. Missing/invalid fields refuse the complete frame. It never evaluates
a script field, executes the VM, allocates a string/object, or adds a reference.

Native `SL_FindString` may reorder its string hash cache on a hit while holding
the native script-string critical section. This does not change string identity
or reference counts, but inspection is not claimed to be byte-pure. Do not use
it as a restoration attestation or pretend that a digest makes that cache
mutation disappear.

The optional fixture verifies real skill bases 1/3/5/7 and the source-defined
aim/init-reaction/reaction values (.6/1500/1000, .4/750/500, .25/500/300,
.1/100/50). The native Bot Warfare difficulty thread refreshes every five
seconds, so the actual inspection has an eight-second settling limit. It also
records real team counts and requires native free-for-all/team membership.
These are actual VM settings and native memberships, not elapsed AI behavior.
Inspection and subsequent tape must retain the same slot/generation identities.

## Actual Build 70 Regular evidence

The exclusive run retained at
`results/native-bots-20261003-113412-364076/regular_dm/result.json`
passed in 19.69 seconds including cleanup. It used the archived Build 70 client
and server, one rendered human and fifteen Regular native bots on Killhouse DM.
Actual stock keys changed Bot Count `15 -> 0 -> 15` and difficulty ordinal
`1 -> 2 -> 1`; both final settings were read back from the isolated archived
profile before hosting. This covers reversible selection of the chosen
settings, not the full four-difficulty menu matrix.

The real idle-VM inspector completed at authoritative tick 8022. Every bot had
skill base `3`, aim time `0.400000006`, initial reaction `750`, reaction `500`,
remember time `2000` and FOV `0.600000024`. All sixteen actual native clients
had free-for-all team `0`. The inspection and command tape retained identical
admission slot/generation identities.

The retained one-second tape covers ticks 10224 through 11223: 1,000 complete
ticks and 16,000 slot records. Its 7,982,578 bytes have SHA-256
`4d6b62f2814e9b402fbde77ad785351966b6bb26fba122945d1ce0edd71b42ff`.
Nine bots selected movement. Attack was selected for 1,500 accumulated bot
slot-ticks with a longest held interval of 765 ms; ADS for 4,090 slot-ticks
with a longest held interval of 1,000 ms; sprint for 100 slot-ticks and melee
for 50 slot-ticks. Four native `last_spawn_time` changes were observed.
Reload, frag and smoke selections were absent in this short window. These
counts attest actual consumed commands and field changes, not attack damage,
projectile emission, completed reloads or full respawn behavior.

Native Leave stopped the exact owned dedicated child. The created client
exited with code 0; exact child exit, profile isolation cleanup and empty error
lists are retained in the result. No game was kept running afterward. The
result explicitly keeps `complete_bot_acceptance` and
`full_world_replay_verified` false.

## Remaining separate acceptance gates

`complete_bot_acceptance` remains false even when the scoped fixture passes.
Required follow-up evidence includes real target/reaction/fire timing, weapon
and damage outcomes, respawn behavior, objective execution, typed catalog/menu
selection coverage, and full-slot yielding to a second actual human. Bot and
human admission-generation namespaces must not be compared as if they were one
monotonic counter. A human reclaim test should retain the real peer slot and
before/after `(native identity kind, generation, last_connect_time)` records,
then require the refilled bot's own generation to advance in its own namespace.

Recruit, Hardened and Veteran still require actual inspected matches; the
all-four difficulty/menu matrix has not passed yet. TDM, S&D, Sabotage,
Domination and Headquarters still require actual mode/skill/team/tape checks;
the six-mode gate is therefore incomplete. Team membership and balance are
separate outcomes, as are actual second-human admission and bot yielding,
slot-generation reuse, combat and objectives. The loaded-mode inventory
adaptation is staged in [bot-native-mode-inventory.md](bot-native-mode-inventory.md).

The script's portable self-test verifies menu-transition rejection, native slot
identity/record completeness, exact held windows and spawn observations, and
corruption rejection. It supplies no game evidence. The complete self-test,
including the optional VM-inspection validator, passed after timing fixtures
closed and before Build 70. The one actual inspected Regular run above supplies
the separate native evidence; additional exclusive cases await coordination.
