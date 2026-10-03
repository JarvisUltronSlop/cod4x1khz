# Build 75 CPU and GPU usage rerun

A 60-second Killhouse match with one rendered human and fifteen Regular bots
passes all native rate, p99, backlog, bot control, independent prediction owner,
collision, script, rendering and provenance checks. Rendering uses Direct3D 9,
640×480, an uncapped presentation rate and VSync off. The benchmark uses its
isolated profile. All exact owned test processes are closed afterward.

| Measurement | Result |
| --- | --- |
| Authoritative simulation | 1,000.005 ticks/s |
| Client prediction / commands sent | 999.890/s |
| Server commands received | 999.889/s |
| Client snapshots received | 1,000.024/s |
| Fifteen bot controls combined | 15,000.079 applications/s |
| Server simulation plus snapshot work p99 | 856 µs |
| Actual rendered FPS | 752.78 |
| Client and server CPU, combined | 2.172 logical-core equivalents; 13.58% of sixteen logical processors |
| System CPU, including other applications | 17.85% average |
| Hottest logical processor | 53.69% average; 78.28% peak |
| RX 9070 XT 3D engine | 19.29% average; 25.53% peak |
| Both RX 9070 XT Copy engines | 0% average and peak |

The renderer's WDDM adapter LUID matches the Windows DirectX adapter record for
the RX 9070 XT. No game GPU engine uses the integrated Radeon in these samples.
The server has no GPU counter activity; it executes gameplay on the CPU.

There is measured hardware headroom. Rendering remains below the separate
1,000 FPS goal in this run. Frame preparation averages 889 µs, backend work
874 µs and the Present call 468 µs. These are elapsed wall times: frontend and
backend can overlap, Present is inside backend work, and waits count toward the
measurements. Adding them would produce an invalid frame cost.

The aggregate evidence points toward frame preparation, draw submission,
scheduling or presentation waits as useful profiling targets. It does not prove
a particular function is the bottleneck, CPU throttling, GPU clock limitations
or a guaranteed benefit from another graphics API. The current gameplay and
bot application clocks already meet the 1 kHz timing requirement in this case.
Bot Warfare decisions retain their intended 50 ms cadence.

WDDM counters describe engine busy time rather than shader throughput. Separate
Copy engines need not be busy for a 3D workload; data movement can also occur on
the 3D engine. Microsoft's [GPU counter explanation](https://devblogs.microsoft.com/directx/gpus-in-the-task-manager/)
and [CPU/GPU boundedness explanation](https://devblogs.microsoft.com/directx/cpu-and-gpu-boundedness/)
describe these distinctions.

The [consolidated result](../results/usage-20261003174417-summary.json) hashes the
raw benchmark, GPU adapter mapping, CPU samples and timestamped console output.
It includes every GPU engine, each logical CPU, process/thread CPU time, native
rates, exact cleanup and limits. CPU sampling is read-only, outside the game,
with samples aligned to the measurement console markers. Those system-wide
values include other applications. The full benchmark is
[retained separately](../results/mp_killhouse-60s-uncapped-480p-20261003-124428.json).

This single current-build case does not complete the all-map matrix, controlled
gameplay tests, public-feature integration or renderer-active full-world replay.
