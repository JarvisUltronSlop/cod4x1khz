# Bot target visibility optimization

The first optimization changed only the player visibility section of
`target_loop()` in the Bot Warfare overlay. The script-target branch, eligibility checks, target object
updates, remembered-target aging, selection, notifications, random calls,
reaction settings, spawn phase, and 50 ms target schedule are unchanged.

The original player predicate is:

```
(sight at any bone)
&& (bullet passes OR hits glass at any bone)
&& (smoke passes OR player is close)
&& (inside FOV OR existing target has nonzero trace_time)
```

The FOV/remembered-trace condition now runs before bone lookup and tracing.
Failing this condition sets `canTargetPlayer` false and follows the original
aging/remembered-selection path. It does not skip the candidate's state update.
The original `target_this_frame` override still runs afterward, consumes only a
matching eligible player, and can force acceptance even when visibility fails.

For candidates passing the cheap condition, sight checks use the original
head, left ankle, right ankle order and stop on the first success. Only a
successful sight result starts the independent bullet OR, which uses that same
bone order and stops on passage or glass. Bone positions are fetched once,
only when a sight or bullet check needs them. A head sight and left-ankle glass
hit can still qualify together; the two ORs are not merged into one per-bone
predicate. Smoke and close-distance evaluation retain their original order.

`scripts/bot_target_semantics.py` strictly parses the actual pinned upstream
and overlay GSC functions. A small GSC-subset interpreter executes both target
loops and the unchanged target-creation, trace/no-trace and aim-offset helper
statements with controlled native answers. It compares full resulting target
state, current-target selection, frame-override state, random call arguments
and results, and `new_enemy` notifications. It also checks trace order,
short-circuit boundaries, no duplicate tag lookup, and preservation of the
untouched helpers and `target()` cadence against the upstream source.

The 11,148 cases exhaust sight and bullet/glass truth assignments across FOV,
smoke/close, new/previously traced/lost targets and frame overrides. Additional
cases cover remember-time and reaction boundaries, all four difficulty
reaction times, ADS narrowing, invalid candidates, existing and tied best
targets, script targets, and multiple complete 50 ms decisions. The result in
`results/bot-target-semantics.json` includes exact source/tool hashes.

This proves source-level branch/state equivalence for controlled answers. It
does not execute the native VM compiler, prove collision geometry, validate
custom models with missing tags, or measure match timing. The native trace
call order changes intentionally and temporary VM allocations/tag-cache
contents can differ. The game owner must compile/package the overlay and
repeat short stock-map bot correctness and performance checks before making
native performance or full replay claims. No native trace helper is added.

The later per-decision forward/origin cache and its independent source-level
checks are documented in [bot-target-pose-cache.md](bot-target-pose-cache.md).

Run the extended portable regression without overwriting the historical result:

```
python scripts/bot_target_semantics.py --output results/bot-target-semantics70.json
```
