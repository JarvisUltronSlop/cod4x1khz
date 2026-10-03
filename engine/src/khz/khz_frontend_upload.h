#pragma once
#include <cstdint>
struct IDirect3DVertexBuffer9;
struct IDirect3DIndexBuffer9;
struct GfxBackEndData;
#if defined(COD4X1KHZ) && !defined(KISAK_DEDICATED)
void KHZ_FrontendUploadInit();
void KHZ_FrontendUploadShutdown();
bool KHZ_FrontendVertexLock(IDirect3DVertexBuffer9*, int offset, int bytes, int flags, void** result);
bool KHZ_FrontendVertexUnlock(IDirect3DVertexBuffer9*);
bool KHZ_FrontendIndexLock(IDirect3DIndexBuffer9*, int offset, int bytes, int flags, void** result);
bool KHZ_FrontendIndexUnlock(IDirect3DIndexBuffer9*);
void KHZ_FrontendUpload(const GfxBackEndData*);
const char* KHZ_FrontendUploadStatus();
#else
inline void KHZ_FrontendUploadInit() {}
inline void KHZ_FrontendUploadShutdown() {}
inline bool KHZ_FrontendVertexLock(IDirect3DVertexBuffer9*, int, int, int, void**) { return false; }
inline bool KHZ_FrontendVertexUnlock(IDirect3DVertexBuffer9*) { return false; }
inline bool KHZ_FrontendIndexLock(IDirect3DIndexBuffer9*, int, int, int, void**) { return false; }
inline bool KHZ_FrontendIndexUnlock(IDirect3DIndexBuffer9*) { return false; }
inline void KHZ_FrontendUpload(const GfxBackEndData*) {}
#endif
