# Next native bot and loadout fixtures

This is a read-only coverage audit and fixture plan. No listed follow-up was
launched during the SDK batch or the 60-second timing matrix. Current bot
evidence is the [inspected Build 70 Regular DM case](../results/native-bots-20261003-113412-364076/regular_dm/result.json),
described in [bot-acceptance-fixture.md](bot-acceptance-fixture.md).

## What the ten-case matrix establishes

`scripts/native_bot_probe.py:502` defines Recruit, Regular, Hardened and Veteran
on Killhouse DM; Regular on TDM (`war`), S&D (`sd`), Sabotage (`sab`), Domination
(`dom`) and Headquarters (`koth`); and zero-bot DM. It does not cover all 24
difficulty/mode combinations or mode objectives. The first case can exercise
Bot Count 0/15 and all four difficulty ordinals in seven settings screens,
including the otherwise unproven CTF screen.

`--require-inspection` is essential for actual VM difficulty/team evidence.
Without it the matrix checks configuration, admission and consumed commands.
With it, the native idle-VM inspector checks real skill bases 1/3/5/7,
aim/init-reaction/reaction values, actual native team membership and the same
admission identities in a later 1,000-tick tape. These are settings and consumed
commands, not measured AI reaction or combat/objective outcomes. The script's
top-level gap list still describes the old lack of an inspector; successful
required-inspection cases remove those gaps in their individual reports.

Current persistence is archived readback within the same process. Each fixture
starts with a fresh benchmark home and later restores its prior home; it does
not restart the executable using the selected profile. The `khz_ui host`
adapter validates real typed entries and calls native StartServer, but also
sets the requested bot values. It cannot independently prove remembered values
or the user's stock mode-selection widget path.

## First serial batch: missing inspected cases

Run one case at a time on a frozen deployment, stopping on native error or failed
owned cleanup. Each case retains one actual 1,000-tick tape and has a 60-second
diagnostic limit; this is not a performance benchmark.

```powershell
python scripts/native_bot_probe.py --case recruit_dm --require-inspection --captures 1 --case-timeout 60 --client-port 28992
python scripts/native_bot_probe.py --case hardened_dm --require-inspection --captures 1 --case-timeout 60 --no-menu-matrix --client-port 28992
python scripts/native_bot_probe.py --case veteran_dm --require-inspection --captures 1 --case-timeout 60 --no-menu-matrix --client-port 28992
```

Then use the same flags with `regular_war`, `regular_sd`, `regular_sab`,
`regular_dom`, `regular_koth` and `empty_dm`. For S&D, normal dead players and
intermission must be distinguished from a disconnected owner. Do not weaken
the existing native session-state/team checks to pass a stalled match. A mode
that cannot settle within the short budget remains a failed/incomplete case.

Before claiming all-six catalog presence, retain `khz_ui list` and validate its
actual `game_types_valid`/bounded, unique `game_types` entries. The staged
[mode inventory recipe](bot-native-mode-inventory.md) explains the current
formatter-completeness limit. A missing entry cannot prove CTF is unavailable.
The actual StartServer result must retain the requested loaded mode.

## Build 71 admission diagnostic

The new [admission harness](../scripts/native_bot_admission_probe.py) records its
own hash separately from the frozen build archive. Its first
[failed run](../results/native-bot-admission-20261003-120529-279470/result.json)
exposed a validator assumption: all 16 players were required to have script
life state playing/dead simultaneously. The native inspector already requires
actual active, connected admissions. Stock death cameras can briefly use state
2 while health is nonpositive and a prior spawn exists; live or never-spawned
spectators remain rejected. Observed life states/spawn changes are retained
without claiming complete respawn behavior.

The corrected [second run](../results/native-bot-admission-20261003-120802-340524/result.json)
verified a real second human reclaiming slot 15, unchanged surviving bot
admissions, two humans/fourteen bots and positive authoritative acknowledgements.
Its [joined command tape](../results/native-bot-admission-20261003-120802-340524/joined-two-human-commands.jsonl)
retains and validates all 1,000 ticks and 16,000 actual native slot records.
Both client processes exited normally and the same owned server was stopped
through host Leave; frozen provenance and interactive preferences were unchanged.

Bot refill failed: after the second client returned to its disconnected main
menu, independent server samples still reported its active human admission for
over eleven seconds. Its accepted command count stopped while snapshots
continued, so the server retained two humans/fourteen bots. Source review found
that native `CL_Disconnect` clears the accepted custom session before writing
its final reliable disconnect packets; the server therefore discards them as
stale-session packets. A corrected engine and a fresh complete reclaim/refill
run were required. That historical artifact remains a partial admission proof
and a retained failure, not successful refill acceptance.

The [Build 72 corrected run](../results/native-bot-admission-20261003-121433-590591/result.json)
passes reclaim and refill in 54.36 seconds with the same owned server. Slot 15
changes from native bot generation 3 to human generation 5 and back to native
bot generation 11; generation comparisons stay within each kind's namespace.
Every surviving admission is retained. Its
[two-human tape](../results/native-bot-admission-20261003-121433-590591/joined-two-human-commands.jsonl)
and [refilled tape](../results/native-bot-admission-20261003-121433-590591/refilled-one-human-commands.jsonl)
each retain and validate all 1,000 ticks and 16,000 native slot records, with
actual held-control observations for every admitted bot. The earlier
[Build 72 fixture failure](../results/native-bot-admission-20261003-121214-478577/result.json)
is also retained: it already verified native refill but omitted retirement of
the first tape before beginning another. The corrected harness drains only
after actual file contents are retained and validated; drain counters alone do
not attest serialized contents. The executed harness supplement has its own
hash alongside the unchanged deployed binary/asset/helper archive.

Both clients exited normally, the exact server exited through host Leave, and
interactive preferences were unchanged. The serial
[Build 72 strict lifecycle](../results/native-lifecycle-20261003-121548-351189-2016/result.json)
also passes host, Crash map change, fast restart, map restart, graphics restart,
sound restart and explicit Leave/Quit in 48.96 seconds. Each recovered match
has actual accepted input, snapshot, prediction, pose and GPU progress with the
same owned child. These are short functional checks, not timing acceptance,
complete bot combat/respawn coverage or full-world replay proof.

## Persistence: real same-profile process restart

A new harness must retain one fixture-owned benchmark home between two normally
exited client processes. Select Bot Count 0 and Veteran through the real stock
keys; record changed values and the archived config. Quit from the native main
menu, relaunch the root executable with that same home, and read `khz_ui status`
**before** any menu setter or `khz_ui host` call. Require actual count 0 and
difficulty 3. Repeat with count 15 and Recruit, including a normal Leave/Quit.

Do not reuse the current fresh-home constructor unchanged: it relocates the
very profile needed for this proof. Preserve interactive preferences and retain
every created process handle. A separate persistence case can use real stock
Start Server activation after dynamically inspecting its compiled action; a
host adapter that writes the expected values is insufficient evidence.

## Human slot reclaim and bot refill

The native seam is real: `sv_client_mp.cpp:868` drops the highest eligible native
bot when a human arrives at a full server, respecting the private-slot start
index. `khz_bots.cpp:133` maintains
`min(requestedBots, capacity - max(connectedHumans, 1))`; its refresh runs every
100 ticks. Bot Warfare's actual manager polls on a 1.5-second cadence and adds
with a 250 ms spacing, so refill needs a bounded multi-second observation.

Use a new two-client harness; current validators deliberately require one human
and cannot be reused as two-human acceptance without adapting their schema.

1. Start one rendered host with `khz_ui host mp_killhouse dm 15 1`. Require
   one active human, fifteen native bots, capacity 16 and actual accepted input.
   Read `khz_bot_inspect` through paced RCON (marker `KHZ_BOT_INSPECTION`) and
   retain every slot's bot/human kind, generation, connect/spawn times and team.
2. Launch one separately owned `runtime/cod4x1khz_bench.exe` with a distinct
   port/qport and isolated benchmark home, `+connect 127.0.0.1:<actual-child-port>`.
   Use the observed child port, not an assumed fixed port. Require a real second
   peer's unique commands/snapshots, active native human slot and positive
   accepted acknowledgements; total slots must remain 16, with 2 humans/14 bots.
3. Require the reclaimed slot to have changed from the observed highest eligible
   bot to this human. Surviving bot admission generations must stay unchanged.
   Bot and human generations have different namespaces: never compare their
   numerical magnitudes as one lifetime counter.
4. Disconnect only the second client through its native control channel. Keep
   the first client and exact server alive. Within ten seconds require 1 human/
   15 bots, a refilled bot in the vacant slot with a newer **bot** generation than
   its original admission, and real VM skill/selected-command progress. No
   stale human commands may be applied to that new bot generation.
5. Repeat one reclaim/refill cycle if the first completes within the case budget.
   End with normal host Leave/Quit and exact second-client cleanup.

Record transient connecting/zombie states rather than treating configured counts
as admissions. Capture separate tapes before/after settled membership; the
existing stable-mask tape correctly rejects a membership change during capture.
There is no direct-connect injection, dvar-only success or manually added bot
substitute in this test. A private/reserved-slot boundary variant is a later
separate case after the normal full-slot path passes.

## Connected unlocks and actual server loadout usability

The existing menu proofs cover 22 stock primaries and supported attachments/
camos, five sidearms, grenades and perks, plus backup/stat preservation. They do
not prove server equipment. `khz_ui unlockall` queues the real command;
`khz_profile_client.cpp` commits an active private-pair transaction and queues
native reconnect while preserving the owned server. Stock class loading obtains
choices from received server stats 201..249 (`GScr_GetStat` calls
`SV_GetClientStat`); the entitlement sidecar alone is not network equipment proof.

A new isolated harness should retain the locked baseline, native backup and
`KHZ_UNLOCK_TRANSACTION` before/after preserved-stat hashes. Invoke connected
`khz_ui unlockall`, require the same owned child, a new actual human admission,
received stats, accepted commands/snapshots and active prediction/GPU recovery.
Verify idempotence and that no combat/challenge counters were fabricated by the
transaction; ordinary combat progression after it is a separate observation.

Drive Create-a-Class using actual inspected enabled actions, select a distinctive
primary/attachment/camo, sidearm and perk/grenade combination, and retain
`khz_ui status`'s actual class IDs/references. Activate the real in-game custom1
class action via its compiled `scriptmenuresponse`, then observe a native live
spawn. Do not use the benchmark's automatic `assault_mp` selection as custom-class
proof. The stock class loader can normalize invalid component/perk combinations;
expected equipment must follow the actual asset tables and allowed combinations.

Existing `khz_bot_inspect` exposes an actual human's health, weapon index/state
and spawn time, while `khz_state` exposes client weapon index. These can prove a
basic synchronized selected weapon. Complete attachment/camo/secondary/perk/
grenade usability needs a reviewed, idle-owner read-only equipment seam: actual
loaded weapon names, owned inventory, offhand, perk bits, weapon models and
ammo/clip values, tied to the same slot/session/generation. If the loaded frontend
does not expose the actual in-game custom-class action, a bounded validated
native menu-response queue seam is also needed. Neither endpoint is invented
as current capability. These gaps must remain pending until integrated and tested.

All follow-ups preserve `complete_bot_acceptance=false` and
`full_world_replay_verified=false` until their broader gates are independently
proved. Reload/projectile/damage/reaction behavior, objective progress/completion,
all difficulty-mode combinations and native class/respawn outcomes require their
own observed results.
