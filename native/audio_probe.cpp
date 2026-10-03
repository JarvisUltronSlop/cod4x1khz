#include <Windows.h>
#include <mss.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>
#include <khz/khz_archive_seek.h>
#include <cstring>

namespace {
std::vector<char> memoryFile;
size_t memoryPosition = 0;
unsigned opens = 0, reads = 0;
std::string archiveFilename;
unzFile archiveFile = nullptr;
unsigned long archiveDirectory = 0, archiveLength = 0;
U32 __stdcall archiveOpen(const MSS_FILE* name, UINTa* handle) {
    std::printf("archive_open=%s\n", name);
    auto file = unzOpen(archiveFilename.c_str());
    if (!file) return 0;
    for (int found = unzGoToFirstFile(file); found == UNZ_OK; found = unzGoToNextFile(file)) {
        char filename[256]; unz_file_info info{};
        if (unzGetCurrentFileInfo(file, &info, filename, sizeof(filename), nullptr, 0, nullptr, 0) == UNZ_OK &&
            !_stricmp(filename, name)) {
            unzGetCurrentFileInfoPosition(file, &archiveDirectory); archiveLength = info.uncompressed_size;
            if (unzOpenCurrentFile(file) == UNZ_OK) { archiveFile = file; *handle = reinterpret_cast<UINTa>(file); return 1; }
            break;
        }
    }
    unzClose(file); return 0;
}
void __stdcall archiveClose(UINTa handle) { unzClose(reinterpret_cast<unzFile>(handle)); archiveFile = nullptr; }
S32 __stdcall archiveSeek(UINTa handle, S32 offset, U32 origin) {
    auto file = reinterpret_cast<unzFile>(handle);
    if (KHZ_ArchiveSeek(file, archiveDirectory, archiveLength, offset, origin == 0 ? 2 : origin == 1 ? 0 : 1) != 0) return -1;
    return unztell(file);
}
U32 __stdcall archiveRead(UINTa handle, void* output, U32 length) {
    const auto read = unzReadCurrentFile(reinterpret_cast<unzFile>(handle), output, length); ++reads;
    return read < 0 ? 0 : static_cast<U32>(read);
}
U32 __stdcall memoryOpen(const MSS_FILE* filename, UINTa* handle) {
    std::printf("callback_open=%s\n", filename);
    *handle = 1; memoryPosition = 0; ++opens;
    return std::strstr(filename, ".mp3") ? 1 : 0;
}
void __stdcall memoryClose(UINTa) {}
S32 __stdcall memorySeek(UINTa, S32 offset, U32 origin) {
    const auto base = origin == AIL_FILE_SEEK_BEGIN ? 0 : origin == AIL_FILE_SEEK_CURRENT ? memoryPosition : memoryFile.size();
    const auto next = static_cast<std::int64_t>(base) + offset;
    if (next < 0 || next > static_cast<std::int64_t>(memoryFile.size())) return -1;
    memoryPosition = static_cast<size_t>(next); return static_cast<S32>(next);
}
U32 __stdcall memoryRead(UINTa, void* buffer, U32 count) {
    ++reads;
    const auto amount = (std::min)(static_cast<size_t>(count), memoryFile.size() - memoryPosition);
    std::memcpy(buffer, memoryFile.data() + memoryPosition, amount); memoryPosition += amount;
    return static_cast<U32>(amount);
}
}

// Decoder/provider diagnostic, separate from gameplay and silent throughout.
int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) { std::fprintf(stderr, "usage: khz_audio_probe <runtime> <mp3> [memory-first]\n"); return 2; }
    const auto runtime = std::filesystem::absolute(argv[1]);
    const auto mp3 = std::filesystem::absolute(argv[2]);
    SetCurrentDirectoryA(runtime.string().c_str());
    const auto codecs = (runtime / "miles").string();
    const bool fromArchive = argc == 4 && !std::strcmp(argv[3], "archive-first");
    if (fromArchive) {
        archiveFilename = (runtime / "main" / "iw_11.iwd").string();
        AIL_set_file_callbacks(archiveOpen, archiveClose, archiveSeek, archiveRead);
    } else if (argc == 4) {
        std::ifstream input(mp3, std::ios::binary);
        memoryFile.assign(std::istreambuf_iterator<char>(input), {});
        AIL_set_file_callbacks(memoryOpen, memoryClose, memorySeek, memoryRead);
    }
    AIL_set_redist_directory(codecs.c_str());
    if (!AIL_startup()) { std::printf("startup=false error=%s\n", AIL_last_error()); return 1; }
    std::ifstream input(mp3, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
    if (bytes.empty()) { AIL_shutdown(); return 2; }
    const int format = AIL_file_type(bytes.data(), static_cast<U32>(bytes.size()));
    std::printf("bytes=%u file_type=%d\n", static_cast<unsigned>(bytes.size()), format);
    auto driver = AIL_open_digital_driver(44100, 16, 2, 0);
    if (!driver) { std::printf("driver=false error=%s\n", AIL_last_error()); AIL_shutdown(); return 1; }
    AIL_set_digital_master_volume_level(driver, 0.0f);
    auto stream = AIL_open_stream(driver, argc == 4 ? "sound/music/HGW_Gameshell_v10.mp3" : mp3.string().c_str(), 0);
    std::printf("stream=%s error=%s\n", stream ? "true" : "false", AIL_last_error());
    if (stream) {
        S32 length = 0, position = 0;
        AIL_stream_ms_position(stream, &length, &position);
        std::printf("length_ms=%d position_ms=%d reads=%u\n", length, position, reads);
        AIL_close_stream(stream);
    }
    AIL_close_digital_driver(driver);
    AIL_shutdown();
    if (argc == 4) return stream ? 0 : 1;
    memoryFile = bytes;
    AIL_set_file_callbacks(memoryOpen, memoryClose, memorySeek, memoryRead);
    AIL_set_redist_directory(codecs.c_str());
    AIL_startup();
    driver = AIL_open_digital_driver(44100, 16, 2, 0);
    if (!driver) { AIL_shutdown(); return 1; }
    AIL_set_digital_master_volume_level(driver, 0.0f);
    stream = AIL_open_stream(driver, "sound/music/HGW_Gameshell_v10.mp3", 0);
    std::printf("callbacks_before_startup stream=%s opens=%u reads=%u error=%s\n", stream ? "true" : "false", opens, reads, AIL_last_error());
    if (stream) AIL_close_stream(stream);
    AIL_close_digital_driver(driver); AIL_shutdown();
    AIL_startup();
    driver = AIL_open_digital_driver(44100, 16, 2, 0);
    if (!driver) { AIL_shutdown(); return 1; }
    AIL_set_digital_master_volume_level(driver, 0.0f);
    opens = reads = 0;
    stream = AIL_open_stream(driver, "sound/music/HGW_Gameshell_v10.mp3", 0);
    std::printf("callbacks_after_warmup_shutdown stream=%s opens=%u reads=%u error=%s\n", stream ? "true" : "false", opens, reads, AIL_last_error());
    if (stream) AIL_close_stream(stream);
    AIL_close_digital_driver(driver); AIL_shutdown();
    return 0;
}
