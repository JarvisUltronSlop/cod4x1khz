#pragma once

void KHZ_ServerAdminRegisterCheckpoint(); // source-owned END khz_admin_server.cpp
// Integration must call this BEFORE policy/log/admission I/O while a benchmark
// branch is pinned. It records an ineligible branch and prevents the operation.
bool KHZ_ServerAdminAllowExternalEffect(const char* reason);
