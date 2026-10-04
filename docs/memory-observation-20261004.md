# Memory observation, October 4

With every game/server/test process exited, Windows still had roughly 22 GiB of
physical memory in use and 35.7 GiB committed. Steam's main `steamwebhelper`
browser process held 11.53 GiB resident. A read-only working-set/mapping audit
identified about 11.49 GiB as shared resident pages, largely anonymous 25 MiB
sections; its private resident pages were only about 48 MiB. This establishes
where the memory was held, but one observation cannot identify the accumulation's
cause or prove a particular leak.

The owner closed Steam. Available RAM immediately rose to 22,398 MiB and commit
fell to about 12.12 GiB. No Steam/game processes remained. After subsequent native
GPU tests, available RAM was **21.92 GiB** and commit **12.02 GiB**. Codex/ChatGPT
processes held about 2.16 GiB resident and 2.55 GiB private commit. Process resident
sets include shared pages and must not be summed as unique physical usage.

Private results are retained in `results/memory-after-steam-exit-20261004.json`
and `results/memory-after-native-gpu-20261004.json`. The read-only command
`python scripts/memory_snapshot.py --output results/memory-before-game.json`
records system physical/commit/kernel counters and individual process memory.
Use distinct before/after filenames around future launches, alongside the existing
CPU/GPU/game memory observer. It never starts, closes or trims any process.

The inherited engine had a `Steam_Shutdown` function but no caller. The experimental
client now invokes it after stopping simulation and its owned server. It cancels
the ticket, unregisters callbacks before `SteamAPI_Shutdown`, clears API state and
also shuts down a successfully initialized API if login fails. This matches the
[Steam API shutdown contract](https://partner.steamgames.com/doc/api/steam_api#SteamAPI_Shutdown).
It compiles, but **live launch/quit testing is pending**. This correction is not
evidence that the game caused Steam's shared-memory buildup or that the issue
cannot recur. The accepted DX9 root/runtime executable was not replaced.
