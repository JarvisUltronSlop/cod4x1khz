#pragma once
#include "gfx_backend.h"
#include <d3d9.h>

namespace gfx::legacy {
// Engine ABI facade. Resources remain compact stock references; native GPU
// handles live exclusively in the backend's generation-checked tables.
IDirect3D9* createVulkanInterface(const gfx::Config& config);
const char* lastError();
unsigned validationErrors(IDirect3DDevice9* device);
// Atomic counters published by the submission owner; reading cannot wait for
// recording, presentation, or a device lifetime barrier.
const char* frameTelemetry();
void nameResource(IUnknown* resource,const char* name);
}
