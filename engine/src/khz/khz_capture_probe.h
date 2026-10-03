#pragma once
// Main-thread, gated localhost diagnostics. Ordinary captures never attest
// world/script/input restoration and cannot establish deterministic replay.
const char* KHZ_ClientCaptureProbe(const char* action);
