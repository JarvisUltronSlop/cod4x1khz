# Build 75 native archive codec

The loaded Killhouse match passes the actual native archive codec fixture with
sixteen clients and twenty-eight entities. It validates full and delta records,
the loaded-state serialization oracle, 150 logical delay cases, real message
overflow, phantom cache rejection and reused identifiers, descriptor and client
row headroom, split records across the 32 MiB byte ring and byte eviction.

The native decoder executes nine times. The fixture performs 274,042 serialized
field comparisons and 37,248 floating-point comparisons. It writes 257 records
for the byte-ring boundary. Exact codec/profile restoration completes and the
observed world diagnostic hash is unchanged. Native diagnostic owner time is
297 ms; this deliberately off-tick operation is excluded from performance claims.

The command is accepted only through the authenticated owned-child channel.
The ordinary RCON refusal passes. Explicit Leave stops the exact owned server,
and the client quits with exit code zero. Total fixture time, including cleanup,
is 20.83 seconds. The original [result](../results/native-archive-codec-20261003-132554-281924/result.json)
binds Build 75 source/binary hashes and retains the replies and lifecycle observations.

Build 74's first native run failed because its diagnostic readers did not reset
the entity reference before standalone client/entity lists. The real archive
decoder already resets those lists. Build 75 adds those two reader resets;
the archive codec and comparison rules are unchanged. Source-extracted legacy
and deployed packed-codec fixtures each pass 4,885 assertions, including the
original stale-reference negative control. The [first failure](../results/native-archive-codec-20261003-131125-599279/result.json)
and [reference contracts](../staged/archive/results/20261003-132024-942012/result.json)
remain preserved.

This proves the scoped codec diagnostic and restoration. Live spectator/killcam
behavior, long-session clock bounds, whole-world checkpoint restoration and
authoritative replay with the renderer active remain separate pending gates.
