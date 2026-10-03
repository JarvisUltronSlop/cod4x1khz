#pragma once

// Diagnostic included at END khz_bots.cpp for the dedicated custom build.
// Call on the completed dedicated server owner
// outside Com/SV gameplay ticks, with benchmark + test_controls both enabled.
// The returned bounded JSON is owner-local scratch and must be consumed before
// the next call. No script execution, field evaluation, allocation or new refs.
const char* KHZ_BotInspectionJSON();
