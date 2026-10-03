#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Private plugin I/O; no engine FS handles, dvars, search-path pointers or
// package decoder are accessible to a worker. Blocking calls are permitted
// only by the host's explicit pinned job/thread adapter, never a gameplay tick.
namespace khz::plugin_fs {
inline constexpr unsigned Owners = 16, HandlesPerOwner = 16, BuffersPerOwner = 8;
inline constexpr unsigned SearchCapacity = 512, ReferenceCapacity = 128;
inline constexpr int MaximumTransfer = 1024 * 1024;
inline constexpr std::size_t MaximumBuffer = 16 * 1024 * 1024, BufferBudget = 64 * 1024 * 1024;
using Handle = int;
enum class Root { Home, Base };
enum class SearchKind { Directory, Archive };
enum class ReadScope { Virtual, Server };
enum class WriteScope { Game, Home, Base };
struct Search {
    Root root = Root::Base;
    SearchKind kind = SearchKind::Directory;
    std::string relative;
    bool enabled = true, looseAllowed = true, archiveAllowed = true;
    bool referenceLoose = false;
    std::uint32_t checksum = 0;
};
struct Catalog {
    std::uint64_t epoch = 0; // Assigned by Publish; caller value is ignored.
    std::filesystem::path project, home, base;
    std::string game;
    std::vector<Search> search; // Native order, copied only at an FS barrier.
};
struct Reference {
    std::uint64_t epoch = 0;
    Root root = Root::Base;
    bool loose = false;
    std::uint32_t checksum = 0;
    char relative[241]{};
};
struct Statistics {
    std::uint64_t epoch = 0, opened = 0, rejected = 0, bytesRead = 0, bytesWritten = 0;
    std::uint64_t referenceDropped = 0, cancelled = 0;
    unsigned streams = 0, buffers = 0;
    std::size_t bufferBytes = 0;
};
bool Init();
// Service/lifecycle owner only; requires the old native FS to be parked.
// Publication invalidates prior stream tokens. Returned ReadWhole buffers
// remain owned until FreeBuffer or final Discard, including after Cancel.
bool Publish(const Catalog& catalog);
void Invalidate();
std::uint64_t Epoch();
long OpenRead(unsigned owner, std::uint64_t generation, const char* qpath,
    Handle* result, ReadScope scope = ReadScope::Virtual);
Handle OpenWrite(unsigned owner, std::uint64_t generation, const char* qpath,
    WriteScope scope = WriteScope::Game, bool append = false);
int Read(unsigned owner, std::uint64_t generation, void* destination, int bytes, Handle handle);
int Write(unsigned owner, std::uint64_t generation, const void* source, int bytes, Handle handle);
// Pinned fgets semantics (1 line, 0 EOF, -1 error); SV loose reads only.
int ReadLine(unsigned owner, std::uint64_t generation, void* destination, int capacity, Handle handle);
bool Close(unsigned owner, std::uint64_t generation, Handle handle);
int ReadWhole(unsigned owner, std::uint64_t generation, const char* qpath, void** result);
bool FreeBuffer(unsigned owner, std::uint64_t generation, void* buffer);
int WriteWhole(unsigned owner, std::uint64_t generation, const char* qpath,
    const void* source, int bytes, WriteScope scope = WriteScope::Game);
bool CopyHome(unsigned owner, std::uint64_t generation, const char* source, const char* destination);
// Opaque, owner-validated cookie, not a decoder pointer. The adapted SDK
// exposes it only for its private ZIP stream password setter.
void* ZipCookie(unsigned owner, std::uint64_t generation, Handle handle);
int SetZipPassword(unsigned owner, std::uint64_t generation, void* cookie, const char* password);
bool PollReference(Reference* result);
void Cancel(unsigned owner, std::uint64_t generation);
void Discard(unsigned owner, std::uint64_t generation); // Only after DLL/job pins are gone.
void Shutdown(); // Only after all pinned plugin I/O callers have finished.
Statistics Stats();
}

// Native control-owner seams. Root wires capture/invalidate at FS lifecycle
// barriers and reference service outside ticks; these files stay independent
// of the fixed gameplay owner.
bool KHZ_PluginFsCaptureCatalog();
void KHZ_PluginFsServiceReferences();
