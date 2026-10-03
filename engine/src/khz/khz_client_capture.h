#pragma once
#include <replay_capture.hpp>

// Begin/end run only inside an acknowledged owner control barrier. Normal
// telemetry can drain published complete records while the worker continues.
// Restore flags attest independently verified harness operations; a successful
// capture never manufactures a "full-world replay passed" conclusion.
bool KHZ_ClientCaptureBegin(const khz::capture::Request& request);
bool KHZ_ClientCaptureEnd();
bool KHZ_ClientCapturePop(khz::capture::Record* record);
const char* KHZ_ClientCaptureStatusJSON();
// Called after a complete native tick, on the gameplay owner, before immutable
// presentation publication. The two message numbers come from the gameplay
// owner's exact current/next snapshot cursors, never presentation history.
void KHZ_ClientCaptureTick(int currentMessage, int nextMessage);
// Call before/after owner lifecycle teardown while preserving any queued proof.
void KHZ_ClientCaptureInvalidateLifecycle();
