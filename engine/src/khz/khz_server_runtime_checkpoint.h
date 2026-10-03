#pragma once

// Include the source-owned implementation at END of khz_runtime.cpp. It remains
// staged until the actual paused-server observer and every required domain exist.
void KHZ_ServerRuntimeRegisterCheckpoint();
// Rebase only an authenticated explicit checkpoint halt after all copy/restore
// work. Preserve logical time; ordinary scheduler stalls still retain debt.
bool KHZ_ServerRuntimeResumeCheckpointClock(char* reason, unsigned capacity);
