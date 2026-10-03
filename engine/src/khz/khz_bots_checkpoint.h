#pragma once

// Register the actual source-owned Bots provider, on main before the first
// coordinated capture. This is not a standalone world/VM checkpoint.
void KHZ_BotRegisterServerCheckpoint();

// Every script fs_* entry checks this BEFORE opening, reading, writing,
// closing or deleting native files while a replay checkpoint is pinned.
// A rejected call marks the branch ineligible; no file side effects occur.
bool KHZ_BotCheckpointScriptFileAccessAllowed(const char* operation);
