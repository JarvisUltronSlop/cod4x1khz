#ifdef KISAK_OPENAL
// OpenAL implementation of the snd_driver/snd_mss interface declared in snd_local.h.
// Mirrors snd_mss.cpp function-for-function; only one of the two files is compiled for a
// given build (see scripts/common_files.cmake), selected by KISAK_SOUND. See snd_local.h
// for why these keep their historical MSS_ prefix despite backing onto OpenAL here.
#include <universal/q_shared.h>
#include "snd_local.h"
#include "snd_public.h"
#include <universal/com_files.h>
#include <qcommon/qcommon.h>
#include <universal/com_memory.h>
#include <math.h>

// alGlob is declared extern in snd_local.h; its one definition lives in snd_driver.cpp
// (KISAK_SOUND branch), matching where milesGlob's Miles-side definition already lives.

void MSS_InitFailed()
{
    if (Dvar_GetInt("r_vc_compile") != 2)
        Com_Printf(CON_CHANNEL_SOUND, "OpenAL sound system initialization failed\n");
}

// Mirrors MSS_Init + MSS_open_digital_driver in snd_mss.cpp. Unlike Miles (which opens a
// throwaway probe driver first to detect multichannel capability, then reopens for real -
// see MSS_open_digital_driver's comments), MSS_Startup already does an equivalent
// open-then-close sanity probe, so this just opens the real device/context once.


// Unlike Miles (which splits 40 pre-allocated 2D/3D "sample" handles from lazily-opened
// stream handles), OpenAL sources are uniform - allocate all 53 up front.


// Mirrors MSS_InitEq. Only the shared EQ *data* (alGlob.eq[][]) gets initialized here -
// see MSS_ApplyEqFilter below for why alGlob.eqFilter is deliberately left at 0/unallocated
// rather than an EFX filter object (Phase 7: no working EQ application on this side).


// Mirrors MSS_Startup (AIL_startup). Miles' AIL_startup is a global one-time SDK init with
// no device involved yet; OpenAL has no equivalent 2-stage init, so do an equivalent
// open-then-close sanity probe here instead - the real device open happens in MSS_Init.


// Mirrors MSS_ShutdownCleanup. Also frees the sources/filters MSS_InitChannels allocated
// and the reverb objects MSS_Init allocated - a gap left over from Phase 2/6 that had gone
// unnoticed since nothing previously exercised a full init-then-shutdown cycle.


float MSS_GetDryLevel()
{
    return 1.0f;
    //return g_snd.effect->drylevel;
}

float MSS_GetWetLevel(const snd_alias_t *pAlias)
{
    iassert(g_snd.effect->wetlevel >= 0 && g_snd.effect->wetlevel <= 1);

    if (!pAlias)
        return g_snd.effect->wetlevel;

    if (!snd_enableReverb->current.enabled || (pAlias->flags & 0x10) != 0)
        return 0.0f;
    else
        return g_snd.effect->wetlevel;
}

// Mirrors MSS_ApplyEqFilter. Left as a genuine no-op: Miles does true 3-band parametric EQ
// per entchannel (up to 2 EQ slots x 3 bands each, arbitrary type/freq/gain/Q per band -
// see the Miles branch in snd_mss.cpp), and there's no 1:1 equivalent in OpenAL-Soft's EFX,
// which only exposes single-band lowpass/highpass/bandpass filters. Real parity would need
// custom biquad DSP via the AL_SOFT_callback_buffer extension (alBufferCallbackSOFT) -
// genuine DSP work, budgeted separately if ever needed (see WORK.md Phase 7). Not a
// regression from what players get today either way: snd_enableEq already defaults off in
// the existing Miles code due to a documented crash bug (snd.cpp, SND_Init).
//
// Sketch of a crude single-band approximation, if this is ever revisited - uses only band 0
// of EQ slot 0 (ignoring slot 1 entirely, which only matters for the Xbox-only crossfade
// path anyway, and bands 1/2), and only handles the two band types EFX has a filter for.
// LOWSHELF/HIGHSHELF/BELL have no EFX equivalent at all. This is illustrative only - not
// wired up, not tested, and alGlob.eqFilter is deliberately left unallocated (0) above in
// MSS_InitEq since nothing here uses it while this stays commented out.
/*
void __cdecl MSS_ApplyEqFilter(ALuint source, int entchannel)
{
    if (!snd_enableEq->current.enabled)
    {
        alSourcei(source, AL_DIRECT_FILTER, AL_FILTER_NULL);
        return;
    }

    SndEqParams *params = &alGlob.eq[0].params[0][entchannel];
    if (!params->enabled)
    {
        alSourcei(source, AL_DIRECT_FILTER, AL_FILTER_NULL);
        return;
    }

    switch (params->type)
    {
    case SND_EQTYPE_LOWPASS:
        alFilteri(alGlob.eqFilter, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
        alFilterf(alGlob.eqFilter, AL_LOWPASS_GAIN, params->gain);
        alFilterf(alGlob.eqFilter, AL_LOWPASS_GAINHF, 1.0f - (params->freq / 20000.0f));
        alSourcei(source, AL_DIRECT_FILTER, alGlob.eqFilter);
        break;
    case SND_EQTYPE_HIGHPASS:
        alFilteri(alGlob.eqFilter, AL_FILTER_TYPE, AL_FILTER_HIGHPASS);
        alFilterf(alGlob.eqFilter, AL_HIGHPASS_GAIN, params->gain);
        alFilterf(alGlob.eqFilter, AL_HIGHPASS_GAINLF, params->freq / 20000.0f);
        alSourcei(source, AL_DIRECT_FILTER, alGlob.eqFilter);
        break;
    default:
        // SND_EQTYPE_LOWSHELF/HIGHSHELF/BELL: no EFX filter type covers these.
        alSourcei(source, AL_DIRECT_FILTER, AL_FILTER_NULL);
        break;
    }
}
*/
void __cdecl MSS_ApplyEqFilter(ALuint source, int entchannel)
{
}

// Mirrors MSS_ResumeSample.
void __cdecl MSS_ResumeSample(int i, int frametime)
{
    if (g_snd.chaninfo[i].startDelay)
    {
        int remaining = g_snd.chaninfo[i].startDelay - frametime;
        g_snd.chaninfo[i].startDelay = (remaining > 0) ? remaining : 0;
        if (!g_snd.chaninfo[i].startDelay)
        {
            alSourcePlay(alGlob.source[i]);
        }
    }
}

int __cdecl MSS_DigitalFormatType(int waveFormat, int bits, int channels)
{
    int digitalFormat;

    if (waveFormat != 1 && waveFormat != 17)
        Com_Error(ERR_FATAL, "unknown wave format %i", waveFormat);
    if (channels != 1 && channels != 2)
        Com_Error(ERR_FATAL, "Sound has %i channels; only 1 or 2 channels are supported.\n", channels);
    if (bits != 8 && bits != 16)
        Com_Error(ERR_FATAL, "Sound uses %i bits per channel; only 8 or 16 bit channels are supported.\n", bits);
    digitalFormat = 0;
    if (waveFormat == 17)
        digitalFormat = 4;
    if (bits == 16)
        digitalFormat |= 1u;
    if (channels == 2)
        return digitalFormat | 2;
    return digitalFormat;
}







#endif