#pragma once

// Called before the engine initializes its cached installation path or filesystem.
bool KHZ_BootstrapPaths(const char* commandLine);
const char* KHZ_RuntimeRoot();
const char* KHZ_ProjectRoot();
const char* KHZ_HomeRoot();
bool KHZ_IsBenchmarkHome();
