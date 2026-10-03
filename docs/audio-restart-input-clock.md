# Audio restart input admission

Build63's final isolated lifecycle diagnostic is recorded in
`results/native-lifecycle-20261003-100029-388434-26592/result.json`.
Hosting, map change, fast/full restart and graphics restart passed. Audio restart
failed. Exact-owned client/server processes were cleaned up after capture.

Two native PDB inspections separated by about3.1seconds read the real client
command ring and runtime fields. Input timestamp advanced28068→28983 while the
accepted player-state command time advanced30316→33411. Input acknowledgement
stayed7812; the retained prediction span grew82→997 commands. Prediction performed
494083 additional one-millisecond Pmove steps across915 gameplay ticks. The main
thread waited on the gameplay dvar scope; the private owner ran movement traces.
Independent samples from the exact owned server showed received/applied commands
fixed at7791 while timestamp rejections rose170→1811→3098. Snapshots and server
ticks continued, with zero debt or queue drops. These are separate sample times,
so the server applied value is not asserted equal to the client's later7812ack.

The server admits native command timestamps only from2000ms in the past to200ms
in the future. Reload paused command production while the dedicated server
continued. The subsequent `++inputTime` clock stayed more than two seconds behind
that authoritative clock. Fresh sequence numbers therefore failed admission;
prediction repeatedly replayed unacknowledged history and delayed presentation.
Native string-table critical sections were free in the preceding diagnostic.

The repair aligns the native input timestamp to `cl.snap.ps.commandTime` only
after the complete native snapshot and current-session protocol metadata commit.
Alignment never moves it backward. New commands still increment it once per
gameplay tick. Same-session initialization preserves that monotonic timestamp;
new-session reset remains separate. Input sequences, stored commands, duplicate
checks, acknowledgement values and prediction's one-millisecond Pmove steps are
unchanged. Retained older commands still undergo normal timestamp admission.

The standalone regression reproduces the measured rejection boundary and checks
fresh admission, delayed snapshots, duplicate rejection, unchanged saved input
and button-edge behavior. Actual repaired lifecycle verification remains pending
until the coherent next game build is deployed. This evidence is not a full-world
replay or a completed release gate.
