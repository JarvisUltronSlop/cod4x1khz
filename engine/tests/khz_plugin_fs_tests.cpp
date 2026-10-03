#include <khz/khz_plugin_fs.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace {
namespace io = khz::plugin_fs;
namespace fs = std::filesystem;
unsigned cases = 0, failures = 0;
void check(bool condition, const char* name) { ++cases; if (!condition) { ++failures; std::printf("FAIL: %s\n", name); } }
bool readMatches(const char* path, const char* expected, unsigned owner = 0, std::uint64_t generation = 1) {
    void* bytes = nullptr; const int length = io::ReadWhole(owner, generation, path, &bytes);
    const bool okay = length == int(std::strlen(expected)) && bytes && !std::memcmp(bytes, expected, std::strlen(expected)) && !static_cast<char*>(bytes)[length];
    if (bytes) io::FreeBuffer(owner, generation, bytes); return okay;
}
bool drained(unsigned streams, unsigned buffers) {
    const auto deadline = GetTickCount64() + 1000;
    while (GetTickCount64() < deadline) { const auto stats = io::Stats(); if (stats.streams == streams && stats.buffers == buffers) return true; Sleep(2); }
    return false;
}
io::Catalog makeCatalog(const fs::path& root) {
    io::Catalog value; value.project = root; value.home = root / "home"; value.base = root / "base"; value.game = "main";
    io::Search archive; archive.kind = io::SearchKind::Archive; archive.root = io::Root::Home; archive.relative = "main/ordered.iwd"; archive.checksum = 0x12345678;
    value.search.push_back(archive);
    io::Search home; home.kind = io::SearchKind::Directory; home.root = io::Root::Home; home.relative = "main"; home.referenceLoose = true;
    value.search.push_back(home);
    io::Search base = home; base.root = io::Root::Base; value.search.push_back(base);
    io::Search missing = home; missing.relative = "not-created"; value.search.push_back(missing);
    return value;
}
}
int main(int argc, char** argv) {
    if (argc != 2) { std::printf("usage: khz_plugin_fs_tests <owned fixture root>\n"); return 2; }
    const auto root = fs::absolute(fs::path(argv[1])).lexically_normal(); auto catalog = makeCatalog(root);
    check(io::Init() && io::Init(), "idempotent private FS startup");
    check(io::Publish(catalog) && io::Epoch() > 0, "copy ordered catalog with harmless missing directory");
    const auto initialEpoch = io::Epoch();
    check(readMatches("order.dat", "archive-wins"), "native search order preserves IWD over loose paths");
    check(readMatches("CASE.dat", "last-case-entry"), "case-insensitive duplicate last-entry rule");
    check(readMatches("deflated.dat", "compressed-worker-data"), "actual private deflate decoder");
    check(readMatches("empty.dat", ""), "empty archive entry has real buffer and NUL terminator");
    check(readMatches("home.txt", "home-game"), "normal home game directory");
    check(readMatches("base.txt", "base-game"), "normal base game fallback");
    check(readMatches("dir\\nested.txt", "normalized-slash"), "public backslash qpath normalization");
    io::Handle stream = 0; char text[128]{};
    check(io::OpenRead(0, 1, "sv.txt", &stream, io::ReadScope::Server) == 10 && stream > 0, "SV read prefers home root");
    check(io::Read(1, 1, text, 128, stream) == -1 && io::Read(0, 2, text, 128, stream) == -1, "stream owner and plugin generation fence");
    check(io::ReadLine(0, 1, text, 4, stream) == 1 && !std::strcmp(text, "alp"), "ReadLine fgets truncation");
    check(io::ReadLine(0, 1, text, 128, stream) == 1 && !std::strcmp(text, "ha\n"), "ReadLine continuation includes newline");
    check(io::ReadLine(0, 1, text, 128, stream) == 1 && !std::strcmp(text, "beta"), "ReadLine final unterminated line");
    check(io::ReadLine(0, 1, text, 128, stream) == 0 && !*text, "ReadLine EOF is zero");
    check(io::Close(0, 1, stream) && !io::Close(0, 1, stream), "close real stream and reject duplicate close");
    check(io::OpenRead(0, 1, "sv-base.txt", &stream, io::ReadScope::Server) == 7 && io::Read(0, 1, text, 128, stream) == 7 && !std::memcmp(text, "sv-base", 7), "SV base root fallback");
    io::Close(0, 1, stream);
    check(io::OpenRead(0, 1, "deflated.dat", &stream) == 22 && io::ReadLine(0, 1, text, 128, stream) == -1, "ReadLine excludes ZIP and non-SV streams");
    check(io::ZipCookie(0, 1, stream) && !io::ZipCookie(1, 1, stream), "ZIP cookie is private to owning module");
    check(io::SetZipPassword(0, 1, reinterpret_cast<void*>(~std::uintptr_t(0)), "x") == -102, "foreign ZIP pointer rejected without dereference");
    check(io::Read(0, 1, text, -1, stream) == -1 && io::Read(0, 1, text, io::MaximumTransfer + 1, stream) == -1, "stream transfer bounds before copying");
    io::Close(0, 1, stream);
    void* bytes = reinterpret_cast<void*>(1);
    check(io::ReadWhole(0, 1, "corrupt.dat", &bytes) == -1 && !bytes, "actual ZIP CRC failure never publishes a buffer");
    for (const auto* path : {"../outside.txt", "/outside.txt", "C:/outside.txt", "dir/../outside.txt", "NUL.txt", "dir/COM1", "trailing.", "colon:stream", "dir//double"}) {
        check(io::OpenRead(0, 1, path, &stream) == -1 && !stream && !io::OpenWrite(0, 1, path), "escape/device/ADS/noncanonical path rejected");
    }
    check(io::OpenRead(16, 1, "home.txt", &stream) == -1 && io::OpenRead(0, 0, "home.txt", &stream) == -1, "owner capacity and zero generation rejected");
    check(io::ReadWhole(0, 1, "home.txt", nullptr) == 9, "null ReadFile destination probes length without allocation");
    check(io::WriteWhole(0, 1, "written/binary.dat", "A\0B", 3) == 3, "normal binary write below home game");
    stream = io::OpenWrite(0, 1, "written/binary.dat", io::WriteScope::Game, true);
    check(stream && io::Write(0, 1, "C", 1, stream) == 1 && io::Close(0, 1, stream), "normal append writes actual bytes");
    check(io::ReadWhole(0, 1, "written/binary.dat", &bytes) == 4 && bytes && !std::memcmp(bytes, "A\0BC", 4), "binary full read preserves embedded NUL");
    check(!io::FreeBuffer(1, 1, bytes) && !io::FreeBuffer(0, 2, bytes) && io::FreeBuffer(0, 1, bytes), "buffer free checks owner and exact generation");
    check(!io::FreeBuffer(0, 1, bytes), "duplicate buffer free rejected without pointer dereference");
    check(io::WriteWhole(0, 1, "home-written.dat", "home", 4, io::WriteScope::Home) == 4 && fs::is_regular_file(root / "home/home-written.dat"), "SV home write destination");
    check(io::WriteWhole(0, 1, "base-written.dat", "base", 4, io::WriteScope::Base) == 4 && fs::is_regular_file(root / "base/base-written.dat"), "SV base write destination");
    stream = io::OpenWrite(0, 1, "home-written.dat", io::WriteScope::Home, true);
    check(stream && io::Write(0, 1, "+", 1, stream) == 1 && io::Close(0, 1, stream), "SV root append destination");
    check(io::CopyHome(0, 1, "home-written.dat", "copies/home.dat") && fs::file_size(root / "home/copies/home.dat") == 5, "home-only copy performs actual confined IO");
    check(!io::CopyHome(0, 1, "sv-base.txt", "copies/forbidden.dat") && !io::CopyHome(0, 1, "home-written.dat", "HOME-WRITTEN.dat"), "copy refuses base fallback and same-file truncation");
    if (CreateHardLinkW((root / "home/hard-alias.dat").c_str(), (root / "home/home-written.dat").c_str(), nullptr))
        check(!io::CopyHome(0, 1, "home-written.dat", "hard-alias.dat") && fs::file_size(root / "home/home-written.dat") == 5, "native file-ID check refuses hardlink self-copy before truncation");
    else std::printf("SKIP: fixture filesystem cannot create a hardlink\n");
    std::array<io::Handle, io::HandlesPerOwner> handles{};
    bool allOpened = true; for (auto& handle : handles) allOpened &= io::OpenRead(0, 1, "home.txt", &handle) == 9;
    check(allOpened && io::OpenRead(0, 1, "home.txt", &stream) == -1, "fixed per-module handle capacity");
    const auto retiredToken = handles[0]; for (auto handle : handles) io::Close(0, 1, handle);
    check(io::OpenRead(0, 1, "home.txt", &stream) == 9 && stream != retiredToken && io::Read(0, 1, text, 128, retiredToken) == -1, "slot reuse cannot revive stale token");
    io::Close(0, 1, stream);
    io::Reference reference; unsigned archives = 0, loose = 0;
    while (io::PollReference(&reference)) { if (reference.loose) ++loose; else if (reference.checksum == 0x12345678 && reference.epoch == initialEpoch) ++archives; }
    check(archives > 0 && loose > 0, "copied epoch/package reference journal");
    auto filtered = catalog; filtered.search[0].enabled = false;
    check(io::Publish(filtered) && readMatches("order.dat", "home-loose"), "disabled localized-style descriptor skipped");
    filtered = catalog; filtered.search[0].archiveAllowed = false;
    check(io::Publish(filtered) && readMatches("order.dat", "home-loose"), "copied pure policy excludes impure archive");
    filtered.search[1].looseAllowed = filtered.search[2].looseAllowed = false;
    check(io::Publish(filtered) && io::OpenRead(0, 1, "home.txt", &stream) == -1 && readMatches("allowed.cfg", "cfg-exception"), "pure loose policy and cfg exception");
    check(io::Publish(catalog), "restore valid catalog");
    check(io::OpenRead(0, 1, "encrypted.dat", &stream) == 22, "classic PKZIP encrypted entry opens a private stream");
    void* zip = io::ZipCookie(0, 1, stream);
    check(zip && io::Read(0, 1, text, 128, stream) == -1 && io::SetZipPassword(0, 1, zip, "classic-secret") == 0, "real pinned password setter opens encrypted data");
    check(io::Read(0, 1, text, 128, stream) == 22 && !std::memcmp(text, "encrypted-worker-bytes", 22) && io::Close(0, 1, stream), "actual encrypted deflate read and CRC verification");
    check(io::SetZipPassword(0, 1, zip, "classic-secret") == -102, "closed ZIP cookie cannot access a reused decoder");
    io::OpenRead(0, 1, "home.txt", &stream); const auto oldEpoch = io::Epoch();
    check(io::ReadWhole(0, 1, "home.txt", &bytes) == 9 && io::Publish(catalog) && io::Epoch() > oldEpoch && io::Read(0, 1, text, 128, stream) == -1, "map FS epoch invalidates old stream");
    check(bytes && !std::memcmp(bytes, "home-game", 9) && io::FreeBuffer(0, 1, bytes), "immutable returned bytes survive catalog publication");
    io::Close(0, 1, stream);
    std::array<void*, io::BuffersPerOwner> held{}; bool allBuffers = true;
    for (auto& buffer : held) allBuffers &= io::ReadWhole(1, 7, "empty.dat", &buffer) == 0 && buffer;
    check(allBuffers && io::ReadWhole(1, 7, "empty.dat", &bytes) == -1 && !bytes, "fixed retained-buffer capacity");
    io::OpenRead(1, 7, "home.txt", &stream); io::Cancel(1, 7);
    check(io::Read(1, 7, text, 128, stream) == -1 && io::OpenRead(1, 7, "home.txt", &stream) == -1, "cancel tombstone prevents old IO and admissions");
    bool retained = true; for (auto* buffer : held) retained &= buffer && !*static_cast<char*>(buffer);
    check(retained && io::Stats().buffers == io::BuffersPerOwner, "Cancel retains published buffer bytes until explicit free");
    for (auto* buffer : held) io::FreeBuffer(1, 7, buffer);
    check(drained(0, 0), "cancelled stream retirement and explicit buffer frees drain");
    check(io::ReadWhole(1, 8, "home.txt", &bytes) == 9, "higher reload generation may perform real IO");
    io::Discard(1, 8); check(drained(0, 0), "final unload Discard reclaims retained buffers after pins");
    check(io::ReadWhole(2, 1, "oversized.bin", &bytes) == -1 && !bytes, "full-file maximum checked before allocation");
    std::array<void*, 4> large{};
    bool budget = true; for (unsigned i = 0; i < 3; ++i) budget &= io::ReadWhole(2, 1, "maximum.bin", &large[i]) == int(io::MaximumBuffer);
    check(budget && io::ReadWhole(3, 1, "maximum.bin", &large[3]) == -1 && !large[3] && io::Stats().bufferBytes <= io::BufferBudget, "global buffer budget includes NUL terminators");
    for (unsigned i = 0; i < 3; ++i) if (large[i]) io::FreeBuffer(2, 1, large[i]);
    if (fs::exists(root / "home/main/escape")) {
        check(io::OpenRead(0, 1, "escape/outside.txt", &stream) == -1 && !io::OpenWrite(0, 1, "escape/overwrite.txt"), "actual reparse ancestor blocks reads and writes");
    } else std::printf("SKIP: fixture has no reparse junction (host privilege unavailable)\n");
    auto invalid = catalog; invalid.home = root.parent_path();
    check(!io::Publish(invalid) && io::Epoch() > 0, "catalog rejects root outside its project");
    io::Invalidate(); check(io::OpenRead(0, 1, "home.txt", &stream) == -1, "FS shutdown barrier clears immutable catalog");
    io::Shutdown(); check(!io::Stats().streams && !io::Stats().buffers && !io::OpenWrite(0, 1, "after.dat"), "worker lifecycle shutdown releases private resources");
    std::printf("KHZ_PLUGIN_FS_TESTS cases=%u failures=%u\n", cases, failures); return failures ? 1 : 0;
}
