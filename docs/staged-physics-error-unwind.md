# Opt-in native Physics error-unwind proposal

The authoritative checkpoint opt-in remains disabled. This proposal under
`staged/checkpoint-unwind` is prepared against the actual Build 74 native
Physics entry class, source-owned fence and `Com_Error` implementation. It is
unapplied and has no loaded-engine error-recovery evidence.

Native Physics entry guards hold a lease for a complete function. The current
destructor normally balances that lease. A native `Com_Error` longjmp can skip
the destructor, leaving the packed gate's active count nonzero and preventing
later exclusive capture. Releasing admission at the start of `Com_Error` would
also be wrong: several script/notice/map-load branches return normally and must
retain their surrounding function leases.

The proposed hook runs only immediately before the actual `ERR_JMP` longjmp,
after its native error code and TLS jump target have been selected. It abandons
the actual main owner's nested lease count with one exact compare/exchange,
advances a thread-local generation and retains an atomic refusal outside restored
gameplay state. Late destruction from an older generation cannot decrement a
newly admitted lease. Foreign callers cannot release main admission. An idle
cleanup cannot open a closed exclusive fence.

The source-extracted contract fixture uses placement ownership to model skipped
destructors without undefined C++ jumps across automatic nontrivial objects.
It executes a real setjmp/longjmp through the proposed error hook, tests nested
leases, duplicate cleanup, stale destructors, normal-return notices, foreign
refusal and the native packed exclusive CAS. The unchanged native baseline is a
negative control: an abandoned admitted entry prevents exclusive closure.

Before applying this to an isolated opt-in server, compile the fixture and all
actual guarded owner TUs, retain source hashes, and verify the exact error hook
and guard-on/guard-off link. Then exercise the real native error path in a
bounded owned process and verify all-provider discard, fence cleanup and map
recovery. This proposal does not provide animation/DB/hunk lifetime admission,
activate any checkpoint provider, or establish authoritative renderer replay.
