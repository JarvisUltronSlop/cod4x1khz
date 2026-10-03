# Timestep audit

Stock speed, gravity, jump dvars and weapon-duration definitions remain the
tuning baseline. The custom path changes numerical integration and scheduling
to elapsed milliseconds; a different timestep can still expose behavior that
needs comparison against stock gameplay.

| Area | Implemented change | Verification and remaining coverage |
| --- | --- | --- |
| Player movement | Shared Pmove takes 1 ms substeps; retains fractional velocity; dead movement damping uses elapsed time; bob cycle carries fractions. | Actual shared movement and static BSP replay hashes match at three presentation-read cadences. Scenario assertions for stairs, slopes, ladders and mantling remain. |
| Weapon/view idle | Integer idle phase carries fractional ADS/idle-speed increments. | Core accumulation/reset checks; live client/server smoke tests. Detailed ADS, hold-breath, visual animation and weapon-duration comparisons remain. |
| Turrets | Rotation uses degrees/second; think and sound/fire timers use elapsed milliseconds. | Actual turret function checks: 0.2 degrees at 1 ms and a 50 ms timer decreases to 49. World firing/collision remains. |
| Missiles | Integration, acceleration, steering/attractors and Javelin turns use current frame duration. | Actual RPG trajectory and Javelin acceleration checks. Guided-target, bounce, detonation and collision scenarios remain. Some future-path forecasts retain coarser sampling and need a separate accuracy/cost audit. |
| Helicopters | Angle/velocity integration, acceleration, tilt, lookahead, sound fades and jitter use elapsed time. Jitter acceleration integrates a per-second derivative. | Actual helicopter acceleration and jitter function checks. Flight paths, target guidance, world collision and slide behavior remain. |
| Vehicles | Think intervals, position/velocity integration and inverse-step calculations use current frame duration. Acceleration smoothing retains its 50 ms response through exponential conversion. Texture phases accumulate fractions. | Core smoothing response and actual material-phase comparisons at 1/50 ms. Active vehicle world/collision scenarios remain. |
| Scripts | Seconds become 64-bit millisecond deadlines; due threads use a bounded heap; current-phase notifications preserve listener rearming. | Actual GSC timing/wrap and repeated-notification probes. Broader script workloads and cancellation patterns remain. |
| Prediction collision | Bounded immutable records derive entity transforms from accepted snapshots at a fixed gameplay time, using real BSP/brush geometry. Incomplete worlds have a counted legacy dispatch. | Both Build 43 four-player target-map runs used this handler on every tick with zero legacy dispatches; 46 diagnostic sweeps per map and nine guards pass. No dynamic-entity brush was exercised. Full movement parity, fixed DynEnt poses and concurrent map lifetime remain. |
| Local animation/input | Turn/condition calculations, command feedback, recoil and shellshock input effects advance on fixed ticks. Inspected rendering random consumers use a private stream. | Native and live tests pass. Client-info storage, DObj-derived aim targets and the complete CG world still require separate ownership; no full-world render determinism claim. |
| Presentation | Uncapped real presentations run between simulation deadlines. Three-slot player-state publication and selected field restoration provide an ownership seam. | Live cap changes preserve measured 1 kHz rates. Frontend and gameplay remain on the same thread, so long draws delay ticks. Full immutable world presentation and actual renderer replay remain. |

The legacy renderer's cosmetic ODE physics and all frame-dependent filters have
not been fully audited. Body rewind covers historical position/stance bounds,
not historical animation bones. Those distinctions remain release requirements.
