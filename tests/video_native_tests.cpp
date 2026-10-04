#include <Windows.h>
#include <binklib/bink.h>
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

#define CHECK(v) do { if (!(v)) { std::fprintf(stderr, "%s:%d: %s [%s]\n", __FILE__, __LINE__, #v, BinkGetError()); std::abort(); } } while (0)
struct Planes {
    BINKFRAMEBUFFERS buffers{};
    std::array<std::array<std::vector<unsigned char>, 4>, 2> storage;
    explicit Planes(HBINK b) {
        BinkGetFrameBuffersInfo(b, &buffers);
        CHECK(buffers.TotalFrames == 2);
        for (int f = 0; f != 2; ++f) {
            BINKPLANE* planes[] = {&buffers.Frames[f].YPlane, &buffers.Frames[f].cBPlane, &buffers.Frames[f].cRPlane, &buffers.Frames[f].APlane};
            for (int p = 0; p != 4; ++p) {
                if (!planes[p]->Allocate) continue;
                const auto width = (p == 1 || p == 2) ? buffers.cRcBBufferWidth : buffers.YABufferWidth;
                const auto height = (p == 1 || p == 2) ? buffers.cRcBBufferHeight : buffers.YABufferHeight;
                planes[p]->BufferPitch = width + 7;
                auto& data = storage[f][p]; data.assign(size_t(width + 7) * height + 64, 0xcd);
                planes[p]->Buffer = data.data() + 32;
            }
        }
        BinkRegisterFrameBuffers(b, &buffers);
    }
    std::vector<unsigned char> raw(HBINK b) const {
        std::vector<unsigned char> result;
        const auto& frame = buffers.Frames[buffers.FrameNum];
        const BINKPLANE* planes[] = {&frame.YPlane, &frame.cBPlane, &frame.cRPlane, &frame.APlane};
        for (int p = 0; p != 4; ++p) {
            if (!planes[p]->Allocate) continue;
            const auto width = (p == 1 || p == 2) ? (b->Width + 1) / 2 : b->Width;
            const auto height = (p == 1 || p == 2) ? (b->Height + 1) / 2 : b->Height;
            const auto* data = static_cast<const unsigned char*>(planes[p]->Buffer);
            for (unsigned y = 0; y != height; ++y) result.insert(result.end(), data + size_t(y) * planes[p]->BufferPitch, data + size_t(y) * planes[p]->BufferPitch + width);
        }
        return result;
    }
    void checkCanaries(HBINK b) const {
        for (int f = 0; f != 2; ++f) for (int p = 0; p != 4; ++p) {
            const auto& data = storage[f][p];
            if (data.empty()) continue;
            for (int i = 0; i != 32; ++i) { CHECK(data[i] == 0xcd); CHECK(data[data.size() - 1 - i] == 0xcd); }
            const auto width = (p == 1 || p == 2) ? (b->Width + 1) / 2 : b->Width;
            const auto height = (p == 1 || p == 2) ? (b->Height + 1) / 2 : b->Height;
            for (unsigned y = 0; y != height; ++y) for (unsigned x = width; x != width + 7; ++x) CHECK(data[32 + size_t(y) * (width + 7) + x] == 0xcd);
        }
    }
};
std::vector<char> read(const char* path) {
    std::ifstream input(path, std::ios::binary); CHECK(input.good());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
int main(int argc, char** argv) {
    CHECK(argc >= 3);
    BinkSetMemory(nullptr, nullptr);
    CHECK(!BinkOpen("missing-native-video-fixture.bik", BINKNOFRAMEBUFFERS)); CHECK(*BinkGetError());
    const auto file = read(argv[1]), reference = read(argv[2]);
    auto* memory = static_cast<char*>(VirtualAlloc(reinterpret_cast<void*>(0x400000000ULL), file.size(), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    CHECK(memory && reinterpret_cast<uintptr_t>(memory) > UINT32_MAX);
    std::memcpy(memory, file.data(), file.size());
    HBINK disk = BinkOpen(argv[1], BINKNOFRAMEBUFFERS | BINKALPHA);
    HBINK resident = BinkOpen(memory, BINKNOFRAMEBUFFERS | BINKALPHA | BINKFROMMEMORY);
    CHECK(disk && resident && !*BinkGetError());
    CHECK(disk->Frames == resident->Frames && disk->FrameRate == 60 && disk->FrameRateDiv == 1);
    Planes diskPlanes(disk), memoryPlanes(resident);
    std::vector<unsigned char> first;
    size_t referenceOffset = 0;
    for (unsigned i = 0; i != disk->Frames; ++i) {
        CHECK(!BinkDoFrame(disk) && !BinkDoFrame(resident));
        CHECK(disk->FrameNum == i + 1 && disk->LastFrameNum == i + 1 && diskPlanes.buffers.FrameNum == i % 2);
        auto frame = diskPlanes.raw(disk);
        CHECK(frame == memoryPlanes.raw(resident));
        if (i < 3) {
            CHECK(referenceOffset + frame.size() <= reference.size());
            CHECK(!std::memcmp(frame.data(), reference.data() + referenceOffset, frame.size()));
            referenceOffset += frame.size();
        }
        if (!i) first = frame;
        diskPlanes.checkCanaries(disk); memoryPlanes.checkCanaries(resident);
        BinkNextFrame(disk); BinkNextFrame(resident);
    }
    CHECK(referenceOffset == reference.size());
    CHECK(disk->FrameNum == 1 && resident->FrameNum == 1);
    CHECK(!BinkDoFrame(disk) && !BinkDoFrame(resident)); CHECK(first == diskPlanes.raw(disk) && first == memoryPlanes.raw(resident));
    BinkNextFrame(disk);
    CHECK(BinkPause(disk, 1) == 0); CHECK(BinkWait(disk));
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    CHECK(BinkPause(disk, 0) == 1); CHECK(BinkWait(disk));
    std::this_thread::sleep_for(std::chrono::milliseconds(20)); CHECK(!BinkWait(disk));
    CHECK(BinkGetRects(disk, BINKSURFACEFAST) == 1 && disk->FrameRects[0].Width == disk->Width);
    BINKREALTIME realtime{}; BinkGetRealtime(disk, &realtime, 0); CHECK(realtime.FrameNum == disk->FrameNum && realtime.FrameRate == disk->FrameRate);
    std::printf("Native cinematic: %u frames, file/memory parity above 4 GiB, independent Y/U/V reference, pitched plane canaries, looping and pause clock passed\n", disk->Frames);
    BinkClose(disk); BinkClose(resident); VirtualFree(memory, 0, MEM_RELEASE);
    if (argc > 3) {
        ALCdevice* device = alcOpenDevice(nullptr); CHECK(device);
        ALCcontext* context = alcCreateContext(device, nullptr); CHECK(context);
        const auto setContext = reinterpret_cast<PFNALCSETTHREADCONTEXTPROC>(alcGetProcAddress(device, "alcSetThreadContext"));
        const auto getContext = reinterpret_cast<PFNALCGETTHREADCONTEXTPROC>(alcGetProcAddress(device, "alcGetThreadContext"));
        CHECK(setContext && getContext && setContext(context));
        U32 tracks[] = {0, 1, 2, 3, 4}; BinkSetSoundTrack(5, tracks);
        HBINK audio = BinkOpen(argv[3], BINKNOFRAMEBUFFERS | BINKSNDTRACK); CHECK(audio && audio->NumTracks > 0);
        Planes audioPlanes(audio);
        S32 volumes[] = {0, 0, 0, 0, 0, 0, 0, 0}; BinkSetMixBinVolumes(audio, 0, nullptr, volumes, 8);
        for (unsigned i = 0; i != 12; ++i) { CHECK(!BinkDoFrame(audio)); audioPlanes.checkCanaries(audio); CHECK(getContext() == context); BinkNextFrame(audio); }
        BinkPause(audio, 1); BinkPause(audio, 0); CHECK(getContext() == context);
        BinkClose(audio); CHECK(getContext() == context); setContext(nullptr); alcDestroyContext(context); alcCloseDevice(device);
        std::puts("Native Bink audio decode/queue, volume, pause and OpenAL context isolation passed");
    }
}
