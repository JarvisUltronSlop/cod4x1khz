#include <universal/q_shared.h>
#include <sound/snd_local.h>
#include <universal/com_memory.h>
#include <Windows.h>
#include <array>
#include <vector>
#include <unordered_map>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#define CHECK(v) do { if (!(v)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #v); std::abort(); } } while (0)
snd_local_t g_snd{};
AlLocal alGlob{};
const dvar_t *snd_khz;
const dvar_t *snd_outputConfiguration;
static dvar_t fastFile{};
const dvar_t *useFastFile = &fastFile;
static std::unordered_map<void *, size_t> allocations;
static int errors;
static void *allocate(size_t bytes) {
    auto *p = static_cast<unsigned char *>(VirtualAlloc(nullptr, bytes + 16,
        MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE)); CHECK(p);
    if constexpr (sizeof(void *) == 8) CHECK(reinterpret_cast<uintptr_t>(p) > UINT32_MAX);
    std::memset(p + bytes, 0xc7, 16); allocations.emplace(p, bytes); return p;
}
static void release(void *p) {
    CHECK(allocations.contains(p)); const size_t bytes = allocations.at(p);
    for (size_t i = 0; i < 16; ++i) CHECK(static_cast<unsigned char *>(p)[bytes + i] == 0xc7);
    allocations.erase(p); CHECK(VirtualFree(p, 0, MEM_RELEASE));
}
uint8_t *Hunk_Alloc(uint32_t bytes, const char *name, int) {
    if (!std::strcmp(name, "SND_LoadFromBuffer")) CHECK(bytes == sizeof(LoadedSound));
    return static_cast<uint8_t *>(allocate(bytes));
}
void *Z_Malloc(int bytes, const char *, int) { CHECK(bytes > 0); return allocate(bytes); }
void Z_Free(void *p, int) { release(p); }
void Com_Memcpy(void *dest, const void *src, size_t bytes) { std::memcpy(dest, src, bytes); }
void Com_Printf(int, const char *, ...) {}
void Com_PrintError(int, const char *, ...) { ++errors; }
void Com_Error(errorParm_t, const char *, ...) { throw std::runtime_error("Sound error"); }
void MyAssertHandler(const char *, int, int, const char *, ...) { throw std::runtime_error("Sound assertion"); }
int Com_sprintf(char *, uint32_t, const char *, ...) { throw std::runtime_error("Unexpected file load"); }
int FS_ReadFile(const char *, void **) { throw std::runtime_error("Unexpected file load"); }
void FS_FreeFile(char *) { throw std::runtime_error("Unexpected file load"); }
LoadedSound *SND_LoadFromBuffer(void *, uint32_t, const char *);

static void put(std::vector<uint8_t> &bytes, size_t at, uint32_t value, size_t width) {
    for (size_t i = 0; i < width; ++i) bytes[at + i] = static_cast<uint8_t>(value >> (i * 8));
}
static std::vector<uint8_t> wav(int channels, const std::vector<int16_t> &pcm) {
    const auto dataBytes = pcm.size() * sizeof(int16_t);
    std::vector<uint8_t> bytes(44 + dataBytes);
    std::memcpy(bytes.data(), "RIFF", 4); put(bytes, 4, 36 + dataBytes, 4);
    std::memcpy(bytes.data() + 8, "WAVEfmt ", 8); put(bytes, 16, 16, 4);
    put(bytes, 20, 1, 2); put(bytes, 22, channels, 2); put(bytes, 24, 44100, 4);
    put(bytes, 28, 44100 * channels * 2, 4); put(bytes, 32, channels * 2, 2); put(bytes, 34, 16, 2);
    std::memcpy(bytes.data() + 36, "data", 4); put(bytes, 40, dataBytes, 4);
    std::memcpy(bytes.data() + 44, pcm.data(), dataBytes); return bytes;
}
int main() {
    const std::vector<int16_t> pcm{-32768, -1000, -1, 0, 1, 1000, 16384, 32767};
    g_snd.playback_rate = INT_MAX;
    for (bool fast : {false, true}) {
        fastFile.current.enabled = fast;
        for (int channels : {1, 2}) {
            auto input = wav(channels, pcm);
            auto *sound = SND_LoadFromBuffer(input.data(), static_cast<uint32_t>(input.size()), "fixture.wav");
            CHECK(sound && sound->sound.info.samples == pcm.size() / channels);
            CHECK(sound->sound.info.channels == channels && sound->sound.info.rate == 44100);
            CHECK(sound->sound.data == sound->sound.info.data_ptr && sound->sound.data == sound->sound.info.initial_ptr);
            CHECK(std::memcmp(sound->sound.data, pcm.data(), pcm.size() * 2) == 0);
            release(sound->sound.data); release(sound); CHECK(allocations.empty());
        }
    }
    // Exercise the fastfile payload path, which bypasses SND_LoadFromBuffer.
    const std::vector<uint8_t> pcm8{0, 64, 128, 192, 255, 128};
    const std::vector<int16_t> expected8{-32768, -16384, 0, 16384, 32512, 0};
    for (int channels : {1, 2}) {
        MssSoundCOD4 sound{};
        sound.info.format = 1; sound.info.bits = 8; sound.info.channels = channels;
        sound.info.rate = 44100; sound.info.samples = uint32_t(pcm8.size() / channels);
        sound.info.data_len = uint32_t(pcm8.size());
        SND_SetData(&sound, const_cast<uint8_t *>(pcm8.data()));
        CHECK(sound.info.format == 1 && sound.info.bits == 16 && sound.info.data_len == expected8.size() * 2);
        CHECK(!memcmp(sound.data, expected8.data(), expected8.size() * 2)); release(sound.data);

        std::vector<uint8_t> adpcm(channels == 1 ? 8 : 16);
        put(adpcm, 0, 100, 2);
        if (channels == 2) put(adpcm, 4, uint16_t(-100), 2);
        sound = {}; sound.info.format = 17; sound.info.bits = 4; sound.info.channels = channels;
        sound.info.rate = 22050; sound.info.samples = 9; sound.info.block_size = uint32_t(adpcm.size());
        sound.info.data_len = uint32_t(adpcm.size());
        SND_SetData(&sound, adpcm.data());
        CHECK(sound.info.samples == 9 && sound.info.format == 1 && sound.info.bits == 16);
        for (int i = 0; i != 9 * channels; ++i) CHECK(reinterpret_cast<int16_t *>(sound.data)[i] == (channels == 2 && (i & 1) ? -100 : 100));
        release(sound.data); CHECK(allocations.empty());
    }
    auto input = wav(1, pcm); input[0] = 'X';
    CHECK(!SND_LoadFromBuffer(input.data(), static_cast<uint32_t>(input.size()), "invalid.wav"));
    CHECK(errors == 1 && allocations.empty());

    // Exercise the real OpenAL device, source/filter allocation, channel access and cleanup.
    dvar_t rate{}, configuration{}; rate.current.integer = 44; configuration.current.integer = 0;
    snd_khz = &rate; snd_outputConfiguration = &configuration;
    CHECK(MSS_Startup());
    for (int cycle = 0; cycle < 2; ++cycle) {
        CHECK(MSS_Init()); MSS_InitChannels(); MSS_InitEq();
        CHECK(g_snd.max_2D_channels == 8 && g_snd.max_3D_channels == 32 && g_snd.max_stream_channels == 13);
        CHECK(alGetError() == AL_NO_ERROR);
        SND_Set2DChannelVolume(0, 0.25f); SND_Set3DChannelVolume(8, 0.5f); SND_SetStreamChannelVolume(40, 0.75f);
        CHECK(SND_Get2DChannelVolume(0) == 0.25f && SND_Get3DChannelVolume(8) == 0.5f && SND_GetStreamChannelVolume(40) == 0.75f);
        CHECK(alGetError() == AL_NO_ERROR);
        MSS_ShutdownCleanup(); CHECK(!alGlob.context && !alGlob.device && !alGlob.source[0]);
    }
    std::printf("Native OpenAL: full-pointer mono/stereo PCM loading, both allocators, 53 channels and two device lifecycles passed\n");
}
