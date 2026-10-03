#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "khz_plugin_fs.h"
#include "khz_content.h"
#include "khz_plugin_zip.h"
#include <plugin_io_policy.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace khz::plugin_fs {
namespace {
namespace fs = std::filesystem;
constexpr unsigned SlotCount = Owners * HandlesPerOwner, BufferCount = Owners * BuffersPerOwner;
constexpr unsigned SerialLimit = 0x3fffff, IndexMask = 0x1ff;
constexpr std::uint64_t MaximumStreamBytes = 0x7fffffff;
struct Stream {
    HANDLE file = INVALID_HANDLE_VALUE;
    unzFile zip = nullptr;
    bool writing = false, lineAllowed = false, zipOpen = false, encrypted = false;
    std::uint64_t length = 0;
    // HANDLE callbacks point at this object, which must remain stationary.
    bool ioError = false;
};
struct Slot {
    std::mutex mutex;
    Stream stream;
    unsigned serial = 0;
    std::uint64_t generation = 0, epoch = 0;
    bool occupied = false;
};
struct Buffer {
    std::unique_ptr<unsigned char[]> data;
    std::size_t bytes = 0;
    unsigned owner = 0;
    std::uint64_t generation = 0;
    bool published = false;
};
// Retain host storage through process cleanup if an uncooperative DLL still
// owns a pinned job. No global std::thread/mutex/catalog destructor can race it.
struct State {
    std::array<Slot, SlotCount> slots{};
    std::array<Buffer, BufferCount> buffers{};
    std::array<std::atomic<std::uint64_t>, Owners> cancelled{}, discarded{};
    std::shared_ptr<const Catalog> catalog;
    std::atomic<std::uint64_t> currentEpoch{0}, nextEpoch{0};
    std::atomic<bool> running{false}, stopping{false};
    std::thread retirement;
    std::mutex wakeMutex, bufferMutex, referenceMutex, publicationMutex;
    std::condition_variable wake;
    std::array<Reference, ReferenceCapacity> references{};
    unsigned referenceRead = 0, referenceCount = 0;
    std::size_t bufferBytes = 0;
    std::atomic<std::uint64_t> opened{0}, rejected{0}, bytesRead{0}, bytesWritten{0}, referenceDropped{0}, cancelCount{0};
    std::atomic<unsigned> streamCount{0}, allocatedBuffers{0};
    std::atomic<std::size_t> allocatedBytes{0};
};
State& storage() { static auto* value = new State; return *value; }
auto& slots = storage().slots;
auto& buffers = storage().buffers;
auto& cancelled = storage().cancelled;
auto& discarded = storage().discarded;
auto& catalog = storage().catalog;
auto& currentEpoch = storage().currentEpoch;
auto& nextEpoch = storage().nextEpoch;
auto& running = storage().running;
auto& stopping = storage().stopping;
auto& retirement = storage().retirement;
auto& wakeMutex = storage().wakeMutex;
auto& bufferMutex = storage().bufferMutex;
auto& referenceMutex = storage().referenceMutex;
auto& publicationMutex = storage().publicationMutex;
auto& wake = storage().wake;
auto& references = storage().references;
auto& referenceRead = storage().referenceRead;
auto& referenceCount = storage().referenceCount;
auto& bufferBytes = storage().bufferBytes;
auto& opened = storage().opened;
auto& rejected = storage().rejected;
auto& bytesRead = storage().bytesRead;
auto& bytesWritten = storage().bytesWritten;
auto& referenceDropped = storage().referenceDropped;
auto& cancelCount = storage().cancelCount;
auto& streamCount = storage().streamCount;
auto& allocatedBuffers = storage().allocatedBuffers;
auto& allocatedBytes = storage().allocatedBytes;
bool admitted(unsigned owner, std::uint64_t generation) {
    return running.load(std::memory_order_acquire) && !stopping.load(std::memory_order_acquire) &&
        owner < Owners && generation && generation > cancelled[owner].load(std::memory_order_acquire);
}
void raise(std::atomic<std::uint64_t>& destination, std::uint64_t value) {
    auto previous = destination.load();
    while (previous < value && !destination.compare_exchange_weak(previous, value)) {}
}
bool normalize(const char* input, std::string* result) {
    if (!input || !result) return false;
    char text[241]{};
    unsigned used = 0;
    for (; used < sizeof(text); ++used) {
        const char c = input[used];
        if (!c) break;
        text[used] = c == '\\' ? '/' : c;
    }
    if (!used || used == sizeof(text) || !plugins::io::RelativePath(std::string_view(text, used))) return false;
    result->assign(text, used); return true;
}
bool pathEqual(const fs::path& a, const fs::path& b) {
    const auto x = a.lexically_normal().wstring(), y = b.lexically_normal().wstring();
    return CompareStringOrdinal(x.c_str(), int(x.size()), y.c_str(), int(y.size()), TRUE) == CSTR_EQUAL;
}
bool within(const fs::path& root, const fs::path& candidate) {
    auto a = root.lexically_normal(), b = candidate.lexically_normal();
    auto i = a.begin(), j = b.begin();
    for (; i != a.end(); ++i, ++j) if (j == b.end() || !pathEqual(*i, *j)) return false;
    return true;
}
bool ordinaryDirectory(const fs::path& path) {
    if (!path.is_absolute()) return false;
    auto walk = path.root_path();
    for (const auto& part : path.relative_path()) {
        walk /= part;
        const auto attributes = GetFileAttributesW(walk.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    }
    return true;
}
const fs::path& rootPath(const Catalog& snapshot, Root root) { return root == Root::Home ? snapshot.home : snapshot.base; }
bool verifiedHandle(HANDLE file, const fs::path& expected) {
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(file, &information) || information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    std::array<wchar_t, 32768> physical{};
    const auto length = GetFinalPathNameByHandleW(file, physical.data(), DWORD(physical.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!length || length >= physical.size()) return false;
    std::wstring name(physical.data(), length);
    if (name.compare(0, 4, L"\\\\?\\") == 0) name.erase(0, 4);
    return pathEqual(fs::path(name), expected);
}
HANDLE openFile(const Catalog& snapshot, Root root, const std::string& relative, bool writing, bool append, bool truncate = true) {
    fs::path path;
    if (!content::ConfinedPath(rootPath(snapshot, root), relative, &path, writing)) return INVALID_HANDLE_VALUE;
    // Never truncate before validating the opened file's actual final path.
    const auto file = CreateFileW(path.c_str(), writing ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ,
        writing ? FILE_SHARE_READ : FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, writing ? OPEN_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return file;
    if (!verifiedHandle(file, path)) { CloseHandle(file); return INVALID_HANDLE_VALUE; }
    if (writing) {
        LARGE_INTEGER position{};
        if (!SetFilePointerEx(file, position, nullptr, append ? FILE_END : FILE_BEGIN) || (!append && truncate && !SetEndOfFile(file))) {
            CloseHandle(file); return INVALID_HANDLE_VALUE;
        }
    }
    return file;
}
bool closeStream(Stream& stream) {
    bool okay = true;
    if (stream.zip) {
        if (stream.zipOpen && unzCloseCurrentFile(stream.zip) != UNZ_OK) okay = false;
        if (unzClose(stream.zip) != UNZ_OK) okay = false;
        // The private decoder's close callback closes this same file handle.
    } else if (stream.file != INVALID_HANDLE_VALUE && !CloseHandle(stream.file)) okay = false;
    stream = {}; return okay;
}
voidpf ZCALLBACK zipOpen(voidpf opaque, const char*, int mode) {
    auto* stream = static_cast<Stream*>(opaque);
    if (!stream || stream->file == INVALID_HANDLE_VALUE || (mode & ZLIB_FILEFUNC_MODE_READWRITEFILTER) != ZLIB_FILEFUNC_MODE_READ) return nullptr;
    return stream;
}
uLong ZCALLBACK zipRead(voidpf, voidpf file, void* destination, uLong length) {
    auto* stream = static_cast<Stream*>(file); DWORD count = 0;
    if (!::ReadFile(stream->file, destination, length, &count, nullptr)) { stream->ioError = true; return 0; }
    return count;
}
uLong ZCALLBACK zipWrite(voidpf, voidpf, const void*, uLong) { return 0; }
long ZCALLBACK zipTell(voidpf, voidpf file) {
    auto* stream = static_cast<Stream*>(file); LARGE_INTEGER zero{}, position{};
    if (!SetFilePointerEx(stream->file, zero, &position, FILE_CURRENT) || position.QuadPart > INT32_MAX) { stream->ioError = true; return -1; }
    return long(position.QuadPart);
}
long ZCALLBACK zipSeek(voidpf, voidpf file, uLong offset, int origin) {
    auto* stream = static_cast<Stream*>(file); LARGE_INTEGER position{}; position.QuadPart = offset;
    const DWORD mode = origin == ZLIB_FILEFUNC_SEEK_SET ? FILE_BEGIN : origin == ZLIB_FILEFUNC_SEEK_CUR ? FILE_CURRENT : FILE_END;
    if (!SetFilePointerEx(stream->file, position, nullptr, mode)) { stream->ioError = true; return -1; }
    return 0;
}
int ZCALLBACK zipClose(voidpf, voidpf file) {
    auto* stream = static_cast<Stream*>(file);
    if (stream->file == INVALID_HANDLE_VALUE) return 0;
    const bool okay = CloseHandle(stream->file) != FALSE; stream->file = INVALID_HANDLE_VALUE; return okay ? 0 : -1;
}
int ZCALLBACK zipError(voidpf, voidpf file) { return static_cast<Stream*>(file)->ioError ? 1 : 0; }
bool sameQpath(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (unsigned i = 0; i < a.size(); ++i) {
        auto x = a[i], y = b[i];
        if (x == '\\' || x == ':') x = '/'; if (y == '\\' || y == ':') y = '/';
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A'; if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return false;
    }
    return true;
}
bool openArchive(Stream& stream, const Catalog& snapshot, const Search& search, const std::string& qpath) {
    stream.file = openFile(snapshot, search.root, search.relative, false, false);
    if (stream.file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(stream.file, &size) || size.QuadPart < 22 || std::uint64_t(size.QuadPart) > MaximumStreamBytes) { closeStream(stream); return false; }
    zlib_filefunc_def functions{};
    functions.zopen_file = zipOpen; functions.zread_file = zipRead; functions.zwrite_file = zipWrite;
    functions.ztell_file = zipTell; functions.zseek_file = zipSeek; functions.zclose_file = zipClose;
    functions.zerror_file = zipError; functions.opaque = &stream;
    stream.zip = unzOpen2("private-handle", &functions);
    if (!stream.zip) { closeStream(stream); return false; }
    unz_global_info global{};
    if (unzGetGlobalInfo(stream.zip, &global) != UNZ_OK || global.number_entry > 65536) { closeStream(stream); return false; }
    unz_file_pos selected{}; unz_file_info selectedInfo{}; bool found = false;
    int position = unzGoToFirstFile(stream.zip);
    bool directoryValid = true;
    // The native hash prepends entries: the last case-insensitive duplicate
    // wins. Copy that rule instead of minizip's first-match LocateFile rule.
    for (uLong i = 0; position == UNZ_OK && i < global.number_entry; ++i) {
        char filename[256]{}; unz_file_info information{};
        if (unzGetCurrentFileInfo(stream.zip, &information, filename, sizeof(filename), nullptr, 0, nullptr, 0) != UNZ_OK) { directoryValid = false; break; }
        if (information.size_filename < sizeof(filename) && sameQpath(std::string_view(filename, information.size_filename), qpath)) {
            if (unzGetFilePos(stream.zip, &selected) != UNZ_OK) { directoryValid = false; break; }
            selectedInfo = information; found = true;
        }
        position = unzGoToNextFile(stream.zip);
        if (position != UNZ_OK && (i + 1 != global.number_entry || position != UNZ_END_OF_LIST_OF_FILE)) { directoryValid = false; break; }
    }
    if (!directoryValid || !found || selectedInfo.uncompressed_size > MaximumStreamBytes || unzGoToFilePos(stream.zip, &selected) != UNZ_OK) { closeStream(stream); return false; }
    stream.length = selectedInfo.uncompressed_size; stream.encrypted = (selectedInfo.flag & 1) != 0;
    // Permit obtaining the private ZIP cookie before supplying the password.
    if (!stream.encrypted) {
        stream.zipOpen = unzOpenCurrentFile(stream.zip) == UNZ_OK;
        if (!stream.zipOpen) { closeStream(stream); return false; }
    }
    return true;
}
bool openLoose(Stream& stream, const Catalog& snapshot, Root root, const std::string& relative, bool writing, bool append) {
    stream.file = openFile(snapshot, root, relative, writing, append);
    if (stream.file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(stream.file, &length) || length.QuadPart < 0 || std::uint64_t(length.QuadPart) > MaximumStreamBytes) { closeStream(stream); return false; }
    stream.length = std::uint64_t(length.QuadPart); stream.writing = writing; return true;
}
bool globallyLoaded(std::string_view path) {
    for (const char* suffix : {".hlsl", ".txt", ".cfg", ".levelshots", ".menu", ".arena", ".str"}) {
        const std::string_view ending(suffix);
        if (path.size() >= ending.size() && plugins::io::Equal(path.substr(path.size() - ending.size()), ending)) return true;
    }
    return false;
}
bool pureIgnored(std::string_view path) {
    const auto dot = path.find_last_of('.');
    // Preserve the foundation's actual cfg exception. Its second comparison
    // includes a leading dot after stripping it, so ordinary demos do not hit.
    return dot != path.npos && plugins::io::Equal(path.substr(dot + 1), "cfg");
}
bool queueReference(std::uint64_t epoch, const Search& search, bool loose) {
    Reference reference{}; reference.epoch = epoch; reference.root = search.root;
    reference.loose = loose; reference.checksum = search.checksum;
    if (search.relative.size() > sizeof(reference.relative) - 1) { ++referenceDropped; return false; }
    std::memcpy(reference.relative, search.relative.data(), search.relative.size());
    std::lock_guard<std::mutex> guard(referenceMutex);
    for (unsigned i = 0; i < referenceCount; ++i) {
        const auto& pending = references[(referenceRead + i) % ReferenceCapacity];
        if (pending.epoch == reference.epoch && pending.root == reference.root && pending.loose == reference.loose &&
            pending.checksum == reference.checksum && !std::strcmp(pending.relative, reference.relative)) return true;
    }
    if (referenceCount == ReferenceCapacity) { ++referenceDropped; return false; }
    references[(referenceRead + referenceCount++) % ReferenceCapacity] = reference;
    return true;
}
bool valid(Slot& slot, unsigned owner, std::uint64_t generation, Handle handle) {
    return admitted(owner, generation) && slot.occupied && slot.generation == generation &&
        slot.epoch == currentEpoch.load(std::memory_order_acquire) &&
        unsigned(handle) >> 9 == slot.serial;
}
Slot* lookup(unsigned owner, Handle handle) {
    if (owner >= Owners || handle <= 0) return nullptr;
    const unsigned encoded = unsigned(handle) & IndexMask;
    if (!encoded || encoded > SlotCount) return nullptr;
    const unsigned index = encoded - 1;
    return index / HandlesPerOwner == owner ? &slots[index] : nullptr;
}
void retireLoop() {
    while (!stopping.load()) {
        for (unsigned i = 0; i < slots.size(); ++i) {
            auto& slot = slots[i]; std::unique_lock<std::mutex> guard(slot.mutex, std::try_to_lock);
            if (guard && slot.occupied && (slot.epoch != currentEpoch.load() || slot.generation <= cancelled[i / HandlesPerOwner].load())) {
                closeStream(slot.stream); slot.occupied = false;
                --streamCount;
            }
        }
        {
            std::lock_guard<std::mutex> guard(bufferMutex);
            for (auto& buffer : buffers) if (buffer.data && buffer.generation <= discarded[buffer.owner].load()) {
                bufferBytes -= buffer.bytes; buffer = {};
                --allocatedBuffers; allocatedBytes.store(bufferBytes);
            }
        }
        std::unique_lock<std::mutex> guard(wakeMutex); wake.wait_for(guard, std::chrono::milliseconds(20));
    }
}
template<class Op> auto withStream(unsigned owner, std::uint64_t generation, Handle handle, Op operation, decltype(operation(std::declval<Stream&>())) failure) {
    auto* slot = lookup(owner, handle);
    if (!slot) { ++rejected; return failure; }
    std::lock_guard<std::mutex> guard(slot->mutex);
    if (!valid(*slot, owner, generation, handle)) { ++rejected; return failure; }
    auto result = operation(slot->stream);
    if (!valid(*slot, owner, generation, handle)) { ++rejected; return failure; }
    return result;
}
}
bool Init() {
    std::lock_guard<std::mutex> guard(publicationMutex);
    if (running.load()) return !stopping.load();
    for (auto& value : cancelled) value.store(0); for (auto& value : discarded) value.store(0);
    stopping.store(false); running.store(true);
    try { retirement = std::thread(retireLoop); }
    catch (...) { running.store(false); return false; }
    return true;
}
bool Publish(const Catalog& input) {
    if (!running.load() || stopping.load() || input.search.size() > SearchCapacity) return false;
    std::lock_guard<std::mutex> guard(publicationMutex);
    try {
        auto candidate = std::make_shared<Catalog>(input);
        candidate->project = fs::absolute(candidate->project).lexically_normal();
        candidate->home = fs::absolute(candidate->home).lexically_normal(); candidate->base = fs::absolute(candidate->base).lexically_normal();
        if (!ordinaryDirectory(candidate->project) || !ordinaryDirectory(candidate->home) || !ordinaryDirectory(candidate->base) ||
            !within(candidate->project, candidate->home) || !within(candidate->project, candidate->base) ||
            !plugins::io::RelativePath(candidate->game)) return false;
        for (const auto& search : candidate->search) {
            if (search.root != Root::Home && search.root != Root::Base) return false;
            if (!plugins::io::RelativePath(search.relative)) return false;
            if (search.kind != SearchKind::Directory && search.kind != SearchKind::Archive) return false;
        }
        candidate->epoch = ++nextEpoch;
        // Invalidate old streams before exposing the new immutable catalog.
        // A reader in the publication gap rejects the mismatched old pointer.
        currentEpoch.store(candidate->epoch, std::memory_order_release);
        std::atomic_store_explicit(&catalog, std::shared_ptr<const Catalog>(candidate), std::memory_order_release);
        wake.notify_one(); return true;
    } catch (...) { return false; }
}
void Invalidate() {
    std::lock_guard<std::mutex> guard(publicationMutex);
    currentEpoch.store(++nextEpoch, std::memory_order_release);
    std::atomic_store_explicit(&catalog, std::shared_ptr<const Catalog>{}, std::memory_order_release); wake.notify_one();
}
std::uint64_t Epoch() { return currentEpoch.load(std::memory_order_acquire); }
long OpenRead(unsigned owner, std::uint64_t generation, const char* input, Handle* result, ReadScope scope) {
    if (result) *result = 0;
    if (!result || !admitted(owner, generation)) { ++rejected; return -1; }
    std::string path;
    if (!normalize(input, &path)) { ++rejected; return -1; }
    const auto epoch = Epoch(); auto snapshot = std::atomic_load_explicit(&catalog, std::memory_order_acquire);
    if (!snapshot || snapshot->epoch != epoch || epoch != Epoch()) { ++rejected; return -1; }
    for (unsigned i = owner * HandlesPerOwner; i != (owner + 1) * HandlesPerOwner; ++i) {
        auto& slot = slots[i]; std::unique_lock<std::mutex> guard(slot.mutex, std::try_to_lock);
        if (!guard || slot.occupied || slot.serial == SerialLimit) continue;
        bool found = false; const Search* usedSearch = nullptr; bool looseReference = false;
        if (scope == ReadScope::Server) {
            found = openLoose(slot.stream, *snapshot, Root::Home, path, false, false) || openLoose(slot.stream, *snapshot, Root::Base, path, false, false);
            slot.stream.lineAllowed = found;
        } else if (scope == ReadScope::Virtual) {
            for (const auto& search : snapshot->search) {
                if (!search.enabled) continue;
                if (search.kind == SearchKind::Directory) {
                    if (!search.looseAllowed && !pureIgnored(path)) continue;
                    found = openLoose(slot.stream, *snapshot, search.root, search.relative + '/' + path, false, false);
                    looseReference = found && search.referenceLoose && !pureIgnored(path);
                } else if (search.archiveAllowed) found = openArchive(slot.stream, *snapshot, search, path);
                if (found) { usedSearch = &search; break; }
            }
        }
        if (!found || epoch != Epoch() || !admitted(owner, generation)) { closeStream(slot.stream); ++rejected; return -1; }
        if (usedSearch && (looseReference || (slot.stream.zip && !globallyLoaded(path))) && !queueReference(epoch, *usedSearch, looseReference)) {
            // Do not serve a package/loose pure-sensitive read whose copied
            // reference cannot reach the native FS owner.
            closeStream(slot.stream); ++rejected; return -1;
        }
        slot.epoch = epoch; slot.generation = generation; slot.occupied = true;
        ++streamCount;
        *result = Handle(++slot.serial << 9 | (i + 1)); ++opened;
        return long(slot.stream.length);
    }
    ++rejected; return -1;
}
Handle OpenWrite(unsigned owner, std::uint64_t generation, const char* input, WriteScope scope, bool append) {
    if (!admitted(owner, generation)) { ++rejected; return 0; }
    std::string path;
    if (!normalize(input, &path)) { ++rejected; return 0; }
    const auto epoch = Epoch(); auto snapshot = std::atomic_load_explicit(&catalog, std::memory_order_acquire);
    if (!snapshot || snapshot->epoch != epoch || epoch != Epoch()) { ++rejected; return 0; }
    Root root = Root::Home;
    if (scope == WriteScope::Game) path = snapshot->game + '/' + path;
    else if (scope == WriteScope::Base) root = Root::Base;
    else if (scope != WriteScope::Home) { ++rejected; return 0; }
    if (!plugins::io::RelativePath(path)) { ++rejected; return 0; }
    for (unsigned i = owner * HandlesPerOwner; i != (owner + 1) * HandlesPerOwner; ++i) {
        auto& slot = slots[i]; std::unique_lock<std::mutex> guard(slot.mutex, std::try_to_lock);
        if (!guard || slot.occupied || slot.serial == SerialLimit) continue;
        if (!openLoose(slot.stream, *snapshot, root, path, true, append) || epoch != Epoch() || !admitted(owner, generation)) {
            closeStream(slot.stream); ++rejected; return 0;
        }
        slot.epoch = epoch; slot.generation = generation; slot.occupied = true; ++opened;
        ++streamCount;
        return Handle(++slot.serial << 9 | (i + 1));
    }
    ++rejected; return 0;
}
int Read(unsigned owner, std::uint64_t generation, void* destination, int length, Handle handle) {
    if (length < 0 || length > MaximumTransfer || (!destination && length)) { ++rejected; return -1; }
    return withStream(owner, generation, handle, [&](Stream& stream) {
        if (stream.writing || (stream.zip && !stream.zipOpen)) return -1;
        int count = 0;
        if (stream.zip) count = unzReadCurrentFile(stream.zip, destination, unsigned(length));
        else { DWORD actual = 0; if (!::ReadFile(stream.file, destination, DWORD(length), &actual, nullptr)) return -1; count = int(actual); }
        if (count > 0) bytesRead += std::uint64_t(count); return count;
    }, -1);
}
int Write(unsigned owner, std::uint64_t generation, const void* source, int length, Handle handle) {
    if (length < 0 || length > MaximumTransfer || (!source && length)) { ++rejected; return -1; }
    return withStream(owner, generation, handle, [&](Stream& stream) {
        if (!stream.writing || stream.zip) return -1;
        LARGE_INTEGER zero{}, position{}; DWORD actual = 0;
        if (!SetFilePointerEx(stream.file, zero, &position, FILE_CURRENT) || position.QuadPart < 0 ||
            std::uint64_t(position.QuadPart) + unsigned(length) > MaximumStreamBytes ||
            !::WriteFile(stream.file, source, DWORD(length), &actual, nullptr)) return -1;
        bytesWritten += actual; stream.length = std::max(stream.length, std::uint64_t(position.QuadPart) + actual); return int(actual);
    }, -1);
}
int ReadLine(unsigned owner, std::uint64_t generation, void* destination, int capacity, Handle handle) {
    if (!destination || capacity < 2 || capacity > MaximumTransfer) { ++rejected; return -1; }
    auto* output = static_cast<char*>(destination); output[0] = 0;
    return withStream(owner, generation, handle, [&](Stream& stream) {
        if (!stream.lineAllowed || stream.writing || stream.zip) return -1;
        unsigned used = 0;
        for (; used + 1 < unsigned(capacity); ++used) {
            DWORD actual = 0; char value = 0;
            if (!::ReadFile(stream.file, &value, 1, &actual, nullptr)) { output[used] = 0; return -1; }
            if (!actual) break;
            output[used] = value; ++bytesRead;
            if (value == '\n') { ++used; break; }
        }
        output[used] = 0; return used ? 1 : 0;
    }, -1);
}
bool Close(unsigned owner, std::uint64_t generation, Handle handle) {
    auto* slot = lookup(owner, handle);
    if (!slot) { ++rejected; return false; }
    std::lock_guard<std::mutex> guard(slot->mutex);
    // A cancelled/stale owner may still close its own exact token safely.
    if (!slot->occupied || slot->generation != generation || unsigned(handle) >> 9 != slot->serial) { ++rejected; return false; }
    const bool okay = closeStream(slot->stream); slot->occupied = false; --streamCount; return okay;
}
int ReadWhole(unsigned owner, std::uint64_t generation, const char* path, void** output) {
    if (output) *output = nullptr;
    Handle handle = 0; const auto length = OpenRead(owner, generation, path, &handle);
    if (length < 0) return -1;
    if (!output) { return Close(owner, generation, handle) ? int(length) : -1; }
    if (std::size_t(length) > MaximumBuffer) { Close(owner, generation, handle); ++rejected; return -1; }
    const auto bytes = std::size_t(length) + 1;
    std::unique_lock<std::mutex> guard(bufferMutex);
    Buffer* selected = nullptr;
    for (unsigned i = owner * BuffersPerOwner; i != (owner + 1) * BuffersPerOwner; ++i) if (!buffers[i].data) { selected = &buffers[i]; break; }
    if (!selected || bytes > BufferBudget - bufferBytes) { guard.unlock(); Close(owner, generation, handle); ++rejected; return -1; }
    try { selected->data.reset(new unsigned char[bytes]); }
    catch (...) { guard.unlock(); Close(owner, generation, handle); ++rejected; return -1; }
    selected->bytes = bytes; selected->owner = owner; selected->generation = generation; bufferBytes += bytes;
    ++allocatedBuffers; allocatedBytes.store(bufferBytes);
    auto* destination = selected->data.get(); bool okay = true;
    // Release registration before blocking reads so service statistics/free
    // on another module never wait for disk. Cancel retains the registration;
    // final Discard requires the host to finish all pinned callers first.
    guard.unlock();
    for (int offset = 0; offset < length;) {
        const auto wanted = std::min(MaximumTransfer, int(length) - offset);
        const auto count = Read(owner, generation, destination + offset, wanted, handle);
        if (count <= 0) { okay = false; break; } offset += count;
    }
    okay = Close(owner, generation, handle) && okay && admitted(owner, generation);
    guard.lock();
    if (!okay) { bufferBytes -= selected->bytes; *selected = {}; --allocatedBuffers; allocatedBytes.store(bufferBytes); return -1; }
    destination[length] = 0; selected->published = true; *output = destination; return int(length);
}
bool FreeBuffer(unsigned owner, std::uint64_t generation, void* pointer) {
    if (!pointer || owner >= Owners) { ++rejected; return false; }
    std::lock_guard<std::mutex> guard(bufferMutex);
    for (auto& buffer : buffers) if (buffer.published && buffer.data.get() == pointer && buffer.owner == owner && buffer.generation == generation) {
        bufferBytes -= buffer.bytes; buffer = {}; --allocatedBuffers; allocatedBytes.store(bufferBytes); return true;
    }
    ++rejected; return false;
}
int WriteWhole(unsigned owner, std::uint64_t generation, const char* path, const void* source, int length, WriteScope scope) {
    if (length < 0 || std::size_t(length) > MaximumBuffer || (!source && length)) { ++rejected; return -1; }
    const auto handle = OpenWrite(owner, generation, path, scope);
    if (!handle) return -1;
    int offset = 0; bool okay = true;
    while (offset < length) {
        const int wanted = std::min(MaximumTransfer, length - offset);
        const int count = Write(owner, generation, static_cast<const unsigned char*>(source) + offset, wanted, handle);
        if (count <= 0) { okay = false; break; } offset += count;
    }
    return Close(owner, generation, handle) && okay ? length : -1;
}
bool CopyHome(unsigned owner, std::uint64_t generation, const char* from, const char* to) {
    // Both endpoints are confined home paths; SV OpenRead's base fallback is
    // deliberately not used for this pinned home-only copy operation.
    std::string source, destination;
    if (!admitted(owner, generation) || !normalize(from, &source) || !normalize(to, &destination) || plugins::io::Equal(source, destination)) { ++rejected; return false; }
    const auto epoch = Epoch(); const auto snapshot = std::atomic_load_explicit(&catalog, std::memory_order_acquire);
    if (!snapshot || snapshot->epoch != epoch || epoch != Epoch()) return false;
    Stream input, output;
    if (!openLoose(input, *snapshot, Root::Home, source, false, false) || input.length > MaximumBuffer) { closeStream(input); return false; }
    output.file = openFile(*snapshot, Root::Home, destination, true, false, false);
    BY_HANDLE_FILE_INFORMATION a{}, b{};
    LARGE_INTEGER zero{};
    if (output.file == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(input.file, &a) || !GetFileInformationByHandle(output.file, &b) ||
        (a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow) ||
        !SetFilePointerEx(output.file, zero, nullptr, FILE_BEGIN) || !SetEndOfFile(output.file)) {
        closeStream(input); closeStream(output); return false;
    }
    std::array<unsigned char, 65536> bytes{}; bool okay = true;
    for (;;) {
        if (!admitted(owner, generation) || epoch != Epoch()) { okay = false; break; }
        DWORD read = 0, wrote = 0;
        if (!::ReadFile(input.file, bytes.data(), DWORD(bytes.size()), &read, nullptr)) { okay = false; break; }
        if (!read) break;
        if (!::WriteFile(output.file, bytes.data(), read, &wrote, nullptr) || wrote != read) { okay = false; break; }
        bytesRead += read; bytesWritten += wrote;
    }
    const bool inputClosed = closeStream(input), outputClosed = closeStream(output);
    return inputClosed && outputClosed && okay;
}
void* ZipCookie(unsigned owner, std::uint64_t generation, Handle handle) {
    return withStream(owner, generation, handle, [&](Stream& stream) -> void* {
        return stream.zip ? reinterpret_cast<void*>(std::uintptr_t(unsigned(handle))) : nullptr;
    }, static_cast<void*>(nullptr));
}
int SetZipPassword(unsigned owner, std::uint64_t generation, void* cookie, const char* password) {
    const auto encoded = reinterpret_cast<std::uintptr_t>(cookie);
    if (!password || encoded > INT32_MAX) { ++rejected; return UNZ_PARAMERROR; }
    unsigned length = 0; for (; length < 64 && password[length]; ++length) {}
    if (length == 64) { ++rejected; return UNZ_PARAMERROR; }
    return withStream(owner, generation, int(encoded), [&](Stream& stream) {
        if (!stream.zip) return UNZ_PARAMERROR;
        if (!stream.encrypted) return unzSetPassword(stream.zip, password);
        if (stream.zipOpen) { unzCloseCurrentFile(stream.zip); stream.zipOpen = false; }
        const auto status = unzSetPassword(stream.zip, password);
        if (status != UNZ_OK) return status;
        // Empty is a real PKZIP password; nullptr is a different operation.
        stream.zipOpen = unzOpenCurrentFilePassword(stream.zip, password) == UNZ_OK;
        return stream.zipOpen ? UNZ_OK : UNZ_BADZIPFILE;
    }, UNZ_PARAMERROR);
}
bool PollReference(Reference* result) {
    if (!result) return false;
    std::lock_guard<std::mutex> guard(referenceMutex);
    if (!referenceCount) return false;
    *result = references[referenceRead++ % ReferenceCapacity]; --referenceCount; return true;
}
void Cancel(unsigned owner, std::uint64_t generation) {
    if (owner >= Owners || !generation) return;
    raise(cancelled[owner], generation); ++cancelCount; wake.notify_one();
}
void Discard(unsigned owner, std::uint64_t generation) {
    if (owner >= Owners || !generation) return;
    Cancel(owner, generation); raise(discarded[owner], generation); wake.notify_one();
}
void Shutdown() {
    std::lock_guard<std::mutex> guard(publicationMutex);
    if (!running.exchange(false)) return;
    stopping.store(true); wake.notify_one(); if (retirement.joinable()) retirement.join();
    for (auto& slot : slots) { std::lock_guard<std::mutex> lock(slot.mutex); closeStream(slot.stream); slot.occupied = false; }
    { std::lock_guard<std::mutex> lock(bufferMutex); for (auto& buffer : buffers) buffer = {}; bufferBytes = 0; }
    { std::lock_guard<std::mutex> lock(referenceMutex); referenceRead = referenceCount = 0; }
    std::atomic_store_explicit(&catalog, std::shared_ptr<const Catalog>{}, std::memory_order_release);
    currentEpoch.store(++nextEpoch);
    streamCount.store(0); allocatedBuffers.store(0); allocatedBytes.store(0);
}
Statistics Stats() {
    Statistics result{}; result.epoch = Epoch(); result.opened = opened.load(); result.rejected = rejected.load();
    result.bytesRead = bytesRead.load(); result.bytesWritten = bytesWritten.load(); result.referenceDropped = referenceDropped.load(); result.cancelled = cancelCount.load();
    result.streams = streamCount.load(); result.buffers = allocatedBuffers.load(); result.bufferBytes = allocatedBytes.load();
    return result;
}
}
