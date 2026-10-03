# Bot target decision pose cache

The Build 70 candidate caches `self.origin` and
`anglestoforward(myAngles)` once at the start of `target_loop()`. The angle
getter was already cached by Bot Warfare. Both script-target and player
distance calculations use that same origin. Each eligible player's cone
calculation remains:

```
vectordot( vectornormalize( player.origin - myOrigin ), myForward )
```

This preserves the actual upstream `getConeDot(to, from, dir)` arithmetic:
subtract target minus observer, normalize that vector, then dot it with
`anglestoforward(dir)`. It does not substitute a squared-angle test, remove
normalization, move the candidate's eligibility checks, or share target
positions/bones/visibility between decisions.

The decision and its reviewed helpers do not wait, spawn threads or write
the observer's origin. `target()` still waits exactly 0.05 seconds before
each decision. Target creation, random calls, reaction/remembered timers,
the frame override, best-target selection and the prior independent lazy
sight/bullet/glass predicates are unchanged. The final `new_enemy`
notification retains its position at the end of the decision.

`scripts/package-bots.ps1` reads the pinned
`upstream/bot_warfare` checkout and overrides matching files from
`config/bots`. The four reviewed substitutions have been applied to that
overlay for the next build. Their exact patch is retained at
`staged/bots/target-loop-forward-origin.patch`; the package script does
not read `staged`.

`scripts/bot_target_semantics.py` applies that patch only in memory. When
the overlay already contains the hoist, it reverses the patch strictly to
reconstruct the pre-hoist overlay, reapplies it, and requires exact equality
with the current file. Hunk positions/content, the four substitutions,
unchanged surrounding source, the actual upstream cone helper, helper
synchrony and the original target cadence are checked.

The independent interpreter executes the actual parsed upstream cone helper
for the baseline and the actual candidate expression for the hoist. It
compares complete target/remembered/override state, RNG/events, distance and
LOS call order, and normalized vector/dot operands and results. Its original
visibility truth tables retain controlled cone answers. Additional cases
model zero vectors, translated origins, pitch/yaw/roll, adjacent FOV
boundaries, invalid candidates, script-only decisions and populations up to
15 candidates. Twenty successive 50 ms decisions change the observer's
position and angles to verify the cache is refreshed each time. Four
negative mutations must fail both the source contract and runtime checks.

The run recorded in `results/bot-target-semantics70.json` passed the
11,148 existing visibility cases and 12,440 hoist comparisons, including
1,304 modeled-vector cases. The historical
`results/bot-target-semantics.json` is preserved. The new result contains
the actual source/patch/tool hashes.

With 15 eligible player candidates, the modeled decision uses one forward
conversion instead of 15, and one direct observer-origin field read
instead of 30 inside the parsed decision/helpers. Empty, entirely invalid,
or script-only decisions now incur one unconditional forward conversion
that the prior player-cone path did not perform. These are call counts,
not a timing result.

This fixture does not run the native GSC compiler or reproduce native
floating-point instructions, world collision or gameplay. Python vector
math supplies the same finite inputs and operations to both paths; native
geometry/FP parity and match performance remain separate validation gates
for the rebuilt package.

Run after timing fixtures are closed:

```
python scripts/bot_target_semantics.py --output results/bot-target-semantics70.json
```
