#ifdef KISAK_OPENAL
#include <universal/q_shared.h>
#include "snd_local.h"
#include <universal/com_memory.h>
#include <qcommon/qcommon.h>
#include <dr_libs/dr_wav.h>
#include <vector>
#include <limits>

char __cdecl MSS_Init()
{
    int integer = snd_khz->current.integer;
    int hertz;
    if (integer == 11)
    {
        hertz = 11025;
    }
    else if (integer == 44)
    {
        hertz = 44100;
    }
    else
    {
        if (integer != 22)
            Com_Printf(CON_CHANNEL_SOUND, "invalid value %i for snd_khz, using 22 khz instead\n", snd_khz->current.integer);
        hertz = 22050;
    }

    Com_Printf(
        CON_CHANNEL_SOUND,
        "Attempting %i kHz %i bit [%s] sound\n",
        hertz / 1000,
        16,
        snd_outputConfigurationStrings[snd_outputConfiguration->current.integer]);

    alGlob.device = alcOpenDevice(NULL);
    if (!alGlob.device)
    {
        Com_PrintError(CON_CHANNEL_SOUND, "ERROR: Couldn't open OpenAL device\n");
        return 0;
    }

    ALCint attrs[] = { ALC_FREQUENCY, hertz, 0 };
    alGlob.context = alcCreateContext(alGlob.device, attrs);
    if (!alGlob.context || !alcMakeContextCurrent(alGlob.context))
    {
        Com_PrintError(CON_CHANNEL_SOUND, "ERROR: Couldn't create OpenAL context\n");
        if (alGlob.context)
            alcDestroyContext(alGlob.context);
        alcCloseDevice(alGlob.device);
        alGlob.device = NULL;
        alGlob.context = NULL;
        return 0;
    }

    // Distance falloff is computed entirely by SND_Attenuate's curve and baked directly
    // into AL_GAIN (see the Phase 4 playback functions); disable OpenAL's own automatic
    // distance attenuation so it isn't applied a second time. Mirrors Miles'
    // AIL_set_3D_rolloff_factor(driver, 0.0f) in the original MSS_Init.
    alDistanceModel(AL_NONE);

    // Miles' listener never moves either - SND_Set3DPosition (snd_driver.cpp) always
    // pre-transforms each source's world position into listener-space before handing it
    // to the driver. Fix the AL listener at the origin with identity orientation once,
    // and never touch it again; this is also why split-screen's 2 listeners already work
    // today despite only one physical AL listener existing.
    alListener3f(AL_POSITION, 0.0f, 0.0f, 0.0f);
    alListener3f(AL_VELOCITY, 0.0f, 0.0f, 0.0f);
    ALfloat orientation[6] = { 0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f };
    alListenerfv(AL_ORIENTATION, orientation);

    // One global reverb bus, matching Miles' own model (a single active room type + a
    // per-sample dry/wet send level - see SND_SetRoomtype/SND_ApplyReverbSend in
    // snd_driver.cpp, Phase 6). The effect's actual EAXREVERB properties get filled in by
    // SND_SetRoomtype whenever the room changes; just allocate the objects here.
    alGenAuxiliaryEffectSlots(1, &alGlob.auxSlot);
    alGenEffects(1, &alGlob.reverbEffect);
    alEffecti(alGlob.reverbEffect, AL_EFFECT_TYPE, AL_EFFECT_EAXREVERB);

    g_snd.Initialized2d = 1;
    g_snd.Initialized3d = 1;
    g_snd.max_2D_channels = 8;
    g_snd.max_3D_channels = 32;
    g_snd.max_stream_channels = 13;
    g_snd.playback_rate = hertz + hertz / 2;
    if (g_snd.playback_rate >= 0xAC44)
        g_snd.playback_rate = 0x7FFFFFFF;
    // KISAK_SOUND TODO (Phase 4): derive from snd_outputConfiguration once speaker-config
    // mapping is ported; Miles derives this from its opened mss_spec[] channel count.
    g_snd.playback_channels = 2;
    g_snd.timescale = 1.0;
    return 1;
}

void MSS_InitChannels()
{
    int totalChannels = g_snd.max_2D_channels + g_snd.max_3D_channels + g_snd.max_stream_channels;
    alGenSources(totalChannels, alGlob.source);
    for (int i = 0; i < totalChannels; ++i)
    {
        if (!alGlob.source[i])
            Com_Error(ERR_DROP, "OpenAL source allocation failed on channel %i", i + 1);
    }

    // Per-channel wet-send gain carrier (see AlLocal::sendFilter's comment in snd_local.h).
    // Allocated once here, params updated in place per-channel by SND_ApplyReverbSend.
    alGenFilters(totalChannels, alGlob.sendFilter);
    for (int i = 0; i < totalChannels; ++i)
        alFilteri(alGlob.sendFilter[i], AL_FILTER_TYPE, AL_FILTER_LOWPASS);

    g_snd.ambient_track = SND_TRACK_AMBIENT_PRIMARY_0;
}

void MSS_InitEq()
{
    alGlob.eqFilter = 0;
#ifndef KISAK_XBOX
    alGlob.eqLerp = 1.0f;
#endif

    for (int eqIndex = 0; eqIndex < 2; ++eqIndex)
    {
        for (int band = 0; band < 3; ++band)
        {
            for (int channelIndex = 0; channelIndex < 64; ++channelIndex)
            {
                SndEqParams *params = &alGlob.eq[eqIndex].params[band][channelIndex];
                params->enabled = 0;
                params->freq = 20000.0f;
                params->gain = 1.0f;
                params->q = 1.0f;
                params->type = SND_EQTYPE_FIRST;
            }
        }
    }
}

bool __cdecl MSS_Startup()
{
    ALCdevice *probe = alcOpenDevice(NULL);
    if (!probe)
        return false;
    alcCloseDevice(probe);
    return true;
}

void MSS_ShutdownCleanup()
{
    if (alGlob.context)
    {
        int totalChannels = g_snd.max_2D_channels + g_snd.max_3D_channels + g_snd.max_stream_channels;
        if (totalChannels > 0)
        {
            alDeleteSources(totalChannels, alGlob.source);
            alDeleteFilters(totalChannels, alGlob.sendFilter);
        }
        alDeleteAuxiliaryEffectSlots(1, &alGlob.auxSlot);
        alDeleteEffects(1, &alGlob.reverbEffect);

        alcMakeContextCurrent(NULL);
        alcDestroyContext(alGlob.context);
    }
    if (alGlob.device)
        alcCloseDevice(alGlob.device);
    memset(&alGlob, 0, sizeof(alGlob));
}

uint8_t *__cdecl MSS_Alloc(uint32_t bytes, uint32_t rate)
{
    if (IsFastFileLoad())
        return reinterpret_cast<uint8_t *>(MSS_Alloc_FastFile(bytes));
    else
        return MSS_Alloc_LoadObj(bytes, rate);
}

uint8_t *__cdecl MSS_Alloc_LoadObj(uint32_t bytes, uint32_t rate)
{
    int min_Spec_bytes = bytes;
    while (rate > 0x4099)
    {
        rate >>= 1;
        min_Spec_bytes /= 2;
    }
    return Hunk_Alloc(bytes, "MSS_Alloc", 15);
}

uint32_t *__cdecl MSS_Alloc_FastFile(int bytes)
{
    return (uint32_t *)Z_Malloc(bytes, "MSS_Alloc", 15);
}

float __cdecl SND_Get2DChannelVolume(int index)
{
    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);

    // Unlike Miles' separate left/right levels, AL_GAIN is a single scalar regardless of
    // channel count - no srcChannelCount branching needed on this side.
    ALfloat gain;
    alGetSourcef(alGlob.source[index], AL_GAIN, &gain);
    return gain;
}

void __cdecl SND_Set2DChannelVolume(int index, float volume)
{
    iassert(index >= 0 && index < 0 + g_snd.max_2D_channels);

    alSourcef(alGlob.source[index], AL_GAIN, volume);
}

float __cdecl SND_Get3DChannelVolume(int index)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    ALfloat gain;
    alGetSourcef(alGlob.source[index], AL_GAIN, &gain);
    return gain;
}

void __cdecl SND_Set3DChannelVolume(int index, float volume)
{
    iassert(index >= (0 + 8) && index < (0 + 8) + g_snd.max_3D_channels);

    alSourcef(alGlob.source[index], AL_GAIN, volume);
}

float __cdecl SND_GetStreamChannelVolume(int index)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    ALfloat gain;
    alGetSourcef(alGlob.source[index], AL_GAIN, &gain);
    return gain;
}

void __cdecl SND_SetStreamChannelVolume(int index, float volume)
{
    iassert(index >= SND_FIRST_STREAM_CHANNEL && index < SND_FIRST_STREAM_CHANNEL + g_snd.max_stream_channels);

    alSourcef(alGlob.source[index], AL_GAIN, volume);
}

void __cdecl SND_SetData(MssSoundCOD4 *mssSound, void *srcData)
{
    // Loose WAVs arrive as PCM16, but retail fastfiles also contain PCM8 and
    // IMA-ADPCM payloads. Decode those once during asset loading so playback
    // uses the same signed PCM16 path for both asset sources.
    std::vector<int16_t> decoded;
    if (mssSound->info.format != 1 || mssSound->info.bits != 16)
    {
        const auto &info = mssSound->info;
        if (!srcData || (info.channels != 1 && info.channels != 2) || !info.rate ||
            (info.format != 1 && info.format != 17) ||
            (info.format == 1 && info.bits != 8 && info.bits != 24 && info.bits != 32) ||
            (info.format == 17 && info.block_size <= uint32_t(info.channels * 4)) ||
            info.data_len > uint32_t(std::numeric_limits<int>::max() - 64))
            Com_Error(ERR_DROP, "Invalid retail sound payload format %i/%i", info.format, info.bits);
        const bool adpcm = info.format == 17;
        const uint32_t headerBytes = adpcm ? 60 : 44;
        const uint32_t blockSize = adpcm ? info.block_size : uint32_t(info.channels * info.bits / 8);
        const uint32_t blockFrames = adpcm ? (blockSize - 4 * info.channels) * 2 / info.channels + 1 : 1;
        if (blockSize > UINT16_MAX || blockFrames > UINT16_MAX)
            Com_Error(ERR_DROP, "Retail sound block is too large");
        std::vector<uint8_t> wav(headerBytes + info.data_len + (info.data_len & 1));
        auto put = [&](uint32_t at, uint32_t value, uint32_t bytes) {
            for (uint32_t i = 0; i != bytes; ++i) wav[at + i] = uint8_t(value >> (8 * i));
        };
        memcpy(wav.data(), "RIFF", 4); put(4, uint32_t(wav.size() - 8), 4);
        memcpy(wav.data() + 8, "WAVEfmt ", 8); put(16, adpcm ? 20 : 16, 4);
        put(20, info.format, 2); put(22, info.channels, 2); put(24, info.rate, 4);
        put(28, uint32_t(uint64_t(info.rate) * blockSize / blockFrames), 4);
        put(32, blockSize, 2); put(34, adpcm ? 4 : info.bits, 2);
        if (adpcm) {
            put(36, 2, 2); put(38, blockFrames, 2);
            memcpy(wav.data() + 40, "fact", 4); put(44, 4, 4); put(48, info.samples, 4);
        }
        memcpy(wav.data() + headerBytes - 8, "data", 4); put(headerBytes - 4, info.data_len, 4);
        memcpy(wav.data() + headerBytes, srcData, info.data_len);
        drwav reader{};
        if (!drwav_init_memory(&reader, wav.data(), wav.size(), nullptr))
            Com_Error(ERR_DROP, "Retail sound could not be decoded");
        const uint64_t frames = reader.totalPCMFrameCount;
        if (!frames || frames > uint64_t(std::numeric_limits<int>::max()) / (info.channels * 2)) {
            drwav_uninit(&reader);
            Com_Error(ERR_DROP, "Invalid decoded retail sound size");
        }
        decoded.resize(size_t(frames) * info.channels);
        const auto readFrames = drwav_read_pcm_frames_s16(&reader, frames, decoded.data());
        drwav_uninit(&reader);
        if (readFrames != frames) Com_Error(ERR_DROP, "Truncated retail sound payload");
        mssSound->info.format = 1; mssSound->info.bits = 16;
        mssSound->info.samples = uint32_t(frames); mssSound->info.block_size = info.channels * 2;
        mssSound->info.data_len = uint32_t(decoded.size() * 2);
        srcData = decoded.data();
    }
    if (mssSound->info.rate > g_snd.playback_rate)
    {
        // Resample down to g_snd.playback_rate via simple decimation (nearest-frame
        // resample), matching the halving loop in the Miles branch above. A real
        // low-pass-filtered resample would sound better, but this matches WORK.md Phase 3's
        // stated scope - revisit if downsampled loaded sounds turn out to sound too aliased.
        uint32_t srcFrameCount = mssSound->info.samples;
        uint32_t channels = mssSound->info.channels;
        uint32_t rate = mssSound->info.rate;
        uint32_t frameCount = srcFrameCount;

        while (rate > g_snd.playback_rate)
        {
            rate /= 2;
            frameCount /= 2;
        }

        uint32_t newDataLen = frameCount * channels * sizeof(int16_t);
        mssSound->data = MSS_Alloc(newDataLen, rate);

        const int16_t *src16 = (const int16_t *)srcData;
        int16_t *dst16 = (int16_t *)mssSound->data;
        for (uint32_t i = 0; i < frameCount; ++i)
        {
            uint32_t srcFrame = (uint32_t)((uint64_t)i * srcFrameCount / frameCount);
            for (uint32_t c = 0; c < channels; ++c)
                dst16[i * channels + c] = src16[srcFrame * channels + c];
        }

        mssSound->info.rate = rate;
        mssSound->info.samples = frameCount;
        mssSound->info.data_len = newDataLen;
    }
    else
    {
        mssSound->data = MSS_Alloc(mssSound->info.data_len, mssSound->info.rate);
        Com_Memcpy(mssSound->data, srcData, mssSound->info.data_len);
    }

    mssSound->info.data_ptr = mssSound->data;
    mssSound->info.initial_ptr = mssSound->data;
}
#endif
