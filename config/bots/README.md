This directory contains the project overlays for Bot Warfare v2.2.0 at
`7b52b462e40fa68287b9e54602e52da83b17766c`. Keep the original checkout and its
notices under `upstream/bot_warfare`; deploy its mod assets first, then overlay
this directory's `maps/` and `scripts/` trees in `runtime/mods/mp_bots`.

`scripts/khz_bots.gsc` initializes the native adapter before bot, chat, menu,
and waypoint-editor scripts. Enable it with `khz_bots 1` before loading the map.
The base configuration keeps `khz_bot_count 0`; the managed host sets the desired
count from 0 through 15. Humans and native bots share at most 16 match slots,
and a human can reclaim a bot slot when the match is full.

`khz_bot_difficulty` uses 0 Recruit, 1 Regular, 2 Hardened, and 3 Veteran. These
select upstream skill tiers 1, 3, 5, and 7. Bot Warfare retains its 50 ms decisions,
reaction delays, target selection, and accuracy settings. The internal overlay
publishes float aim goals to the native millisecond owner and adds a single
0–49 ms phase offset when decision threads start. Native movement and held
actions run every millisecond independently of rendering.

The `fs_*` adapter uses bounded handles and returns undefined at end of file.
Editor writes stay in `runtime/scriptdata`, independent of `fs_game`; reads use
game filesystem lookup. Handles close on map shutdown, including aborted loads.
This stage requires actual script compilation and match/lifecycle tests before
Bot Warfare compatibility or 1 kHz performance with sixteen players is claimed.
