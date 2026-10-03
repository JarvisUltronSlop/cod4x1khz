#include <universal/q_shared.h>
#include "khz_frontend_upload.h"
#include <gfx_d3d/r_buffers.h>
#include <gfx_d3d/r_init.h>
#include <gfx_d3d/r_utils.h>
#include <qcommon/threads.h>
#include <universal/com_memory.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace {
// CPU storage follows the existing two backend-data slots. The native worker
// completion barrier and renderer event transfer each slot to one owner at a
// time. No additional frame queue, complete-world copy, or gameplay owner.
struct Buffer {
    unsigned char* memory = nullptr;
    unsigned capacity = 0, bytes = 0;
    bool locked = false, dirty = false;
    IDirect3DVertexBuffer9* vertex = nullptr;
    IDirect3DIndexBuffer9* index = nullptr;
};
struct Frame {
    std::array<Buffer, 4> buffers; // Skin, FX code mesh, marks, pre-tess indices.
    unsigned frame = 0;
    bool selected = false, enabled = false;
};
Frame slots[2];
const dvar_t* staging = nullptr;
bool initialized = false;
std::atomic<std::uint64_t> stagedLocks{0}, uploadFrames{0}, uploadBuffers{0}, uploadBytes{0};
std::atomic<unsigned> failures{0};

[[noreturn]] void reject(const char* reason) {
    failures.fetch_add(1, std::memory_order_relaxed);
    Com_Error(ERR_FATAL, "Frontend buffer ownership: %s", reason);
    std::abort();
}
int slotFor(const GfxBackEndData* data) {
    for (int i = 0; i != 2; ++i) if (data == &s_backEndData[i]) return i;
    return -1; // Loading/remote/debug views keep their native synchronized path.
}
Frame* current() {
    if (!initialized || !Sys_IsMainThread()) return nullptr;
    const int index = slotFor(frontEndDataOut);
    if (index < 0) return nullptr;
    auto& slot = slots[index];
    if (!slot.selected || slot.frame != frontEndDataOut->frameCount) {
        // The previous use of this slot has completed or was discarded by the
        // native lost-device/abort path before R_ToggleSmpFrame reuses it.
        for (auto& buffer : slot.buffers) {
            buffer.locked = buffer.dirty = false;
            buffer.bytes = 0; buffer.vertex = nullptr; buffer.index = nullptr;
        }
        slot.frame = frontEndDataOut->frameCount;
        slot.selected = true;
        slot.enabled = staging->current.enabled;
    }
    return &slot;
}
bool begin(Buffer& buffer, int offset, int bytes, int flags, void** result) {
    if (offset != 0 || bytes < 0 || unsigned(bytes) > buffer.capacity ||
        (flags != D3DLOCK_DISCARD && flags != D3DLOCK_NOOVERWRITE))
        reject("unexpected staged range or lock flags");
    if (buffer.locked) reject("overlapping frontend locks");
    buffer.locked = buffer.dirty = true;
    *result = buffer.memory;
    stagedLocks.fetch_add(1, std::memory_order_relaxed);
    return true;
}
void checkBytes(Buffer& buffer, unsigned bytes) {
    if (bytes > buffer.capacity) reject("upload exceeds preallocated capacity");
    buffer.bytes = bytes;
}
}

void KHZ_FrontendUploadInit() {
    if (initialized) reject("initialization without renderer teardown");
    staging = Dvar_RegisterBool("r_frontendBufferStaging", true, DVAR_NOFLAG,
        "Prepare per-frame geometry in CPU storage; upload on the render owner");
    if (!s_backEndData[0].codeMesh.vb.buffer) return; // Native minimal-resource startup.
    for (unsigned i = 0; i != 2; ++i) {
        const int sizes[] = {gfxBuf.skinnedCacheVbPool[i].total, s_backEndData[i].codeMesh.vb.total,
            s_backEndData[i].markMesh.vb.total, gfxBuf.preTessIndexBufferPool[i].total};
        for (unsigned b = 0; b != 4; ++b) {
            auto& buffer = slots[i].buffers[b];
            if (sizes[b] <= 0 || sizes[b] > (b == 3 ? 4 : 8) * 1024 * 1024) reject("invalid native buffer capacity");
            buffer.capacity = unsigned(sizes[b]) * (b == 3 ? 2 : 1);
            buffer.memory = reinterpret_cast<unsigned char*>(Z_VirtualAlloc(buffer.capacity, "Frontend geometry staging", 18));
            if (!buffer.memory || (reinterpret_cast<std::uintptr_t>(buffer.memory) & 15)) reject("unaligned CPU storage");
        }
    }
    initialized = true;
}

void KHZ_FrontendUploadShutdown() {
    // R_DestroyDynamicBuffers is called only after the native renderer barrier,
    // including reset and vid_restart. Resource addresses never outlive it.
    for (auto& slot : slots) {
        for (auto& buffer : slot.buffers) if (buffer.memory) Z_VirtualFree(buffer.memory);
        slot = Frame{};
    }
    initialized = false;
}

bool KHZ_FrontendVertexLock(IDirect3DVertexBuffer9* handle, int offset, int bytes, int flags, void** result) {
    auto* slot = current();
    if (!slot || !slot->enabled) return false;
    const auto* data = frontEndDataOut;
    const IDirect3DVertexBuffer9* handles[] = {data->skinnedCacheVb->buffer, data->codeMesh.vb.buffer, data->markMesh.vb.buffer};
    for (unsigned i = 0; i != 3; ++i) if (handle == handles[i]) {
        auto& buffer = slot->buffers[i]; buffer.vertex = handle;
        return begin(buffer, offset, bytes, flags, result);
    }
    return false;
}
bool KHZ_FrontendVertexUnlock(IDirect3DVertexBuffer9* handle) {
    if (!initialized || !Sys_IsMainThread()) return false;
    const int index = slotFor(frontEndDataOut);
    if (index < 0) return false;
    for (auto& buffer : slots[index].buffers) if (buffer.vertex == handle && buffer.locked) {
        buffer.locked = false; return true;
    }
    return false;
}
bool KHZ_FrontendIndexLock(IDirect3DIndexBuffer9* handle, int offset, int bytes, int flags, void** result) {
    auto* slot = current();
    if (!slot || !slot->enabled || handle != frontEndDataOut->preTessIb) return false;
    auto& buffer = slot->buffers[3]; buffer.index = handle;
    return begin(buffer, offset, bytes, flags, result);
}
bool KHZ_FrontendIndexUnlock(IDirect3DIndexBuffer9* handle) {
    if (!initialized || !Sys_IsMainThread()) return false;
    const int index = slotFor(frontEndDataOut);
    if (index < 0) return false;
    auto& buffer = slots[index].buffers[3];
    if (buffer.index != handle || !buffer.locked) return false;
    if (gfxBuf.preTessIndexBuffer->buffer != handle) reject("index pool changed during a frontend lock");
    if (gfxBuf.preTessIndexBuffer->used < 0 || gfxBuf.preTessIndexBuffer->used > gfxBuf.preTessIndexBuffer->total)
        reject("invalid native index count");
    checkBytes(buffer, 2 * gfxBuf.preTessIndexBuffer->used);
    buffer.locked = false; return true;
}

void KHZ_FrontendUpload(const GfxBackEndData* data) {
    if (!Sys_IsMainThread() && !Sys_IsRenderThread()) reject("upload called by a non-render owner");
    const int index = slotFor(data);
    if (!initialized || index < 0) return;
    auto& slot = slots[index];
    if (!slot.selected || slot.frame != data->frameCount || !slot.enabled) return;
    // Both SMP and synchronous rendering call this after frontend workers finish.
    const unsigned sizes[] = {data->skinnedCacheVb->used, data->codeMesh.vb.used, data->markMesh.vb.used};
    bool uploaded = false;
    for (unsigned i = 0; i != 4; ++i) {
        auto& buffer = slot.buffers[i];
        if (!buffer.dirty) continue;
        if (buffer.locked) reject("render owner received a locked CPU buffer");
        if (i < 3) checkBytes(buffer, sizes[i]);
        if (buffer.bytes) {
            void* destination = nullptr;
            const auto hr = i == 3
                ? buffer.index->Lock(0, buffer.bytes, &destination, D3DLOCK_DISCARD)
                : buffer.vertex->Lock(0, buffer.bytes, &destination, D3DLOCK_DISCARD);
            if (FAILED(hr)) R_FatalLockError(hr);
            std::memcpy(destination, buffer.memory, buffer.bytes);
            const auto unlocked = i == 3 ? buffer.index->Unlock() : buffer.vertex->Unlock();
            if (FAILED(unlocked)) R_FatalLockError(unlocked);
            uploadBuffers.fetch_add(1, std::memory_order_relaxed);
            uploadBytes.fetch_add(buffer.bytes, std::memory_order_relaxed);
            uploaded = true;
        }
        buffer.dirty = false;
    }
    if (uploaded) uploadFrames.fetch_add(1, std::memory_order_relaxed);
}

const char* KHZ_FrontendUploadStatus() {
    static char text[384];
    std::snprintf(text, sizeof(text), "{\"enabled\":%s,\"initialized\":%s,\"staged_locks\":%llu,\"upload_frames\":%llu,\"upload_buffers\":%llu,\"upload_bytes\":%llu,\"failures\":%u}",
        staging && staging->current.enabled ? "true" : "false", initialized ? "true" : "false",
        stagedLocks.load(), uploadFrames.load(), uploadBuffers.load(), uploadBytes.load(), failures.load());
    return text;
}
