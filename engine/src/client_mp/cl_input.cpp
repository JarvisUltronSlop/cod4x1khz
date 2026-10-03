#ifdef COD4X1KHZ
#include <khz/khz_runtime.h>
#include <khz/khz_client_owner.h>
#include <khz/khz_input_owner.h>
#include <khz/khz_client_input.h>
#include <khz/khz_client_input_checkpoint.h>
#include <khz/khz_client_checkpoint.h>
#include <input_canonical.hpp>
#include <input_replay_policy.hpp>
#include <array>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <limits>
#endif
#ifndef KISAK_MP
#error This File is MultiPlayer Only
#endif

#include <universal/q_shared.h>
#include "client_mp.h"
#include <qcommon/mem_track.h>
#include <universal/com_memory.h>
#include <client/client.h>
#include <aim_assist/aim_assist.h>
#include <win32/win_local.h>
#include <qcommon/cmd.h>
#include <qcommon/threads.h>
#include <cgame_mp/cg_local_mp.h>
#include <devgui/devgui.h>
#include <ui/keycodes.h>


const dvar_t *cl_stanceHoldTime;
const dvar_t *cl_analog_attack_threshold;
const dvar_t *cl_yawspeed;
const dvar_t *cl_upspeed;
const dvar_t *cl_sidespeed;
const dvar_t *cl_bypassMouseInput;
const dvar_t *cl_pitchspeed;
const dvar_t *cl_forwardspeed;
const dvar_t *cl_anglespeedkey;

#define KEY_LEFT 0
#define KEY_RIGHT 1
#define KEY_FORWARD 2
#define KEY_BACK 3
#define KEY_LOOKUP 4
#define KEY_LOOKDOWN 5
#define KEY_MOVELEFT 6
#define KEY_MOVERIGHT 7
#define KEY_STRAFE 8
#define KEY_SPEED 9 // in_speed
#define KEY_MOVEUP 10 // jump
#define KEY_DOWN 11 // stance-down
#define KEY_UP 12 // stance-up
#define KEY_MLOOK 13
#define KEY_ATTACK 14
#define KEY_BREATH 15
#define KEY_FRAG 16
#define KEY_SMOKE 17
#define KEY_MELEE 18
#define KEY_ACTIVATE 19
#define KEY_RELOAD 20
#define KEY_USERELOAD 21
#define KEY_LEANLEFT 22
#define KEY_LEANRIGHT 23
#define KEY_PRONE 24

#define KEY_THROW 26
#define KEY_SPRINT 27

#define KEY_TALK 29

static kbutton_t frontendPlayersKb[1][30];
#ifdef COD4X1KHZ
namespace {
kbutton_t ownerPlayersKb[1][30]{}; // Worker OR acknowledged main control scope; never concurrently.
thread_local bool ownerInputInitialized = false, ownerTypedInput = false, ownerMainPending = false;
thread_local std::uint64_t ownerInputEpoch = 0, ownerLastInputSequence = 0, ownerLastInputQpc = 0;
thread_local int ownerTypedKey = 0;
thread_local unsigned ownerTypedMilliseconds = 0;
thread_local LocSelInputState ownerLocSelInputState = LOC_SEL_INPUT_NONE;
thread_local std::array<bool, 256> ownerKeyDown{};
thread_local std::array<bool, 256> ownerAwaitRelease{};
std::atomic<std::uint64_t> frozenBindingRevision{0};
thread_local std::uint64_t ownerBindingRevision = 0;
khz::canonical::Digest frozenBindingCanonical{};
thread_local khz::canonical::Digest ownerBindingCanonical{};
struct FrozenBinding { char text[1024]{}; bool consoleKey = false; };
std::array<FrozenBinding, 256> frozenBindings{};
unsigned char frozenKeyMap[256][2]{};
KHZ_ClientInputReceipt replayReceipts[KHZ_MaxInputReplayReceipts]; // fixed BSS; worker only
std::atomic<bool> replayActive{false},replayFailed{false};
unsigned replayCount=0,replayCursor=0,replayNativeMilliseconds=0;
std::uint64_t replayQpcEpoch=0,replayFrequency=0,replaySequenceBase=0;
khz::canonical::Digest replayIdentity{};
KHZ_ClientInputCheckpoint savedInputCheckpoint;
KHZ_ClientCheckpointFrame savedInputFrame;
bool savedInputValid=false;
bool OwnerInputContext() { return KHZ_IsClientSimulationThread() || KHZ_ClientOwnerControlsActive(); }
kbutton_t (&InputButtons())[1][30] { return OwnerInputContext() ? ownerPlayersKb : frontendPlayersKb; }
unsigned InputMilliseconds() { return OwnerInputContext() ? KHZ_ClientOwnerMilliseconds() : com_frameTime; }
unsigned InputEventMilliseconds() { return ownerTypedInput ? ownerTypedMilliseconds : InputMilliseconds(); }
int InputFrameMilliseconds() { return OwnerInputContext() ? 1 : frame_msec; }
int InputCatchers(int localClientNum) { return KHZ_IsClientSimulationThread() ? KHZ_ClientOwnerKeyCatchers() : clientUIActives[localClientNum].keyCatchers; }
bool InputHasCatcher(int localClientNum, int flags) { return (InputCatchers(localClientNum) & flags) != 0; }
bool GameplayInputAllowedState(bool developerGui, int catchers) {
    return !developerGui && !(catchers & ~KEYCATCH_LOCATION_SELECTION);
}
}
bool KHZ_ClientGameplayInputAllowed() {
    iassert(Sys_IsMainThread());
    return GameplayInputAllowedState(DevGui_IsActive(),clientUIActives[0].keyCatchers);
}
#define playersKb InputButtons()
#else
#define playersKb frontendPlayersKb
static unsigned InputMilliseconds() { return com_frameTime; }
static unsigned InputEventMilliseconds() { return com_frameTime; }
static int InputFrameMilliseconds() { return frame_msec; }
static bool InputHasCatcher(int localClientNum, int flags) { return Key_IsCatcherActive(localClientNum, flags); }
#endif

void __cdecl TRACK_cl_input()
{
    track_static_alloc_internal(playersKb, 600, "playersKb", 10);
}

void __cdecl CL_ShowSystemCursor(BOOL show)
{
    IN_ShowSystemCursor(show);
}

void __cdecl Scr_MouseEvent(int x, int y)
{
    UI_Component::MouseEvent(x, y);
}

int __cdecl CL_MouseEvent(int x, int y, int dx, int dy)
{
    clientActive_t *LocalClientGlobals; // [esp+0h] [ebp-8h]

    if (DevGui_IsActive())
    {
        DevGui_MouseEvent(dx, dy);
        return 1;
    }
    else if (Key_IsCatcherActive(0, KEYCATCH_SCRIPT))
    {
        Scr_MouseEvent(x, y);
        CL_ShowSystemCursor(1);
        return 0;
    }
    else
    {
        LocalClientGlobals = CL_GetLocalClientGlobals(0);
        if ((clientUIActives[0].keyCatchers & KEYCATCH_UI) == 0
            || UI_GetActiveMenu(0) == UIMENU_SCOREBOARD
            || cl_bypassMouseInput->current.enabled)
        {
            CL_ShowSystemCursor(0);
            LocalClientGlobals->mouseDx[LocalClientGlobals->mouseIndex] += dx;
            LocalClientGlobals->mouseDy[LocalClientGlobals->mouseIndex] += dy;
            return 1;
        }
        else
        {
            UI_MouseEvent(0, x, y);
            return 0;
        }
    }
}

void __cdecl CL_SetStance(int localClientNum, StanceState stance)
{
    if (!playersKb[localClientNum][KEY_PRONE].active && !playersKb[localClientNum][KEY_DOWN].active)
        CL_GetLocalClientGlobals(localClientNum)->stance = stance;
}

void __cdecl IN_CenterView()
{
    clientActive_t *LocalClientGlobals; // edx

    if (clientUIActives[0].connectionState >= CA_CONNECTED)
    {
        LocalClientGlobals = CL_GetLocalClientGlobals(0);
        LocalClientGlobals->viewangles[0] = -LocalClientGlobals->snap.ps.delta_angles[0];
    }
}

bool __cdecl IN_IsTalkKeyHeld()
{
    return playersKb[0][KEY_TALK].active;
}

void __cdecl CL_UpdateCmdButton(int localClientNum, int *cmdButtons, int kbButton, int buttonFlag)
{
    kbutton_t *kb; // [esp+0h] [ebp-4h]

    kb = playersKb[localClientNum];
    if (kb[kbButton].active || kb[kbButton].wasPressed)
        *cmdButtons |= buttonFlag;
    kb[kbButton].wasPressed = 0;
}

void __cdecl CL_WritePacket(int localClientNum)
{
    int v1; // eax
    int v2; // eax
    uint8_t *v3; // ecx
    uint8_t *v4; // edx
    const char *v5; // eax
    int v6; // [esp-Ch] [ebp-8C0h]
    char *v7; // [esp+8h] [ebp-8ACh]
    const char *v8; // [esp+Ch] [ebp-8A8h]
    usercmd_s nullcmd; // [esp+2Ch] [ebp-888h] BYREF
    clientActive_t *LocalClientGlobals; // [esp+4Ch] [ebp-868h]
    usercmd_s *oldcmd; // [esp+58h] [ebp-85Ch]
    int compressedSize; // [esp+5Ch] [ebp-858h]
    connstate_t connstate; // [esp+60h] [ebp-854h]
    uint8_t data[2052]; // [esp+64h] [ebp-850h] BYREF
    uint8_t (*compressedBuf)[2048]; // [esp+86Ch] [ebp-48h]
    msg_t buf; // [esp+870h] [ebp-44h] BYREF
    int oldPacketNum; // [esp+898h] [ebp-1Ch]
    clientConnection_t *clc; // [esp+89Ch] [ebp-18h]
    int key; // [esp+8A0h] [ebp-14h]
    int i; // [esp+8A4h] [ebp-10h]
    usercmd_s *cmd; // [esp+8A8h] [ebp-Ch]
    int count; // [esp+8ACh] [ebp-8h]
    int packetNum; // [esp+8B0h] [ebp-4h]

    LargeLocal compressedBuf_large_local(2048);

    //compressedBuf = (uint8_t (*)[2048])LargeLocal::GetBuf(&compressedBuf_large_local);
    compressedBuf = (uint8_t (*)[2048])compressedBuf_large_local.GetBuf();
    clc = CL_GetLocalClientConnection(localClientNum);

    connstate = CL_GetLocalClientConnectionState(localClientNum);
    if (clc->demoplaying || connstate == CA_CINEMATIC || connstate == CA_LOGO || connstate == CA_SENDINGSTATS)
    {
        //LargeLocal::~LargeLocal(&compressedBuf_large_local);
        return;
    }
    else
    {
        LocalClientGlobals = CL_GetLocalClientGlobals(localClientNum);
        MSG_SetDefaultUserCmd(&LocalClientGlobals->snap.ps, &nullcmd);
        oldcmd = &nullcmd;
        cmd = &nullcmd;
        memset(&buf, 0, sizeof(buf));
        MSG_Init(&buf, data, 2048);
        MSG_WriteByte(&buf, LocalClientGlobals->serverId);
        MSG_WriteLong(&buf, clc->serverMessageSequence);
        MSG_WriteLong(&buf, clc->serverCommandSequence);
        KHZ_WriteClientHeader(&buf);
        for (i = clc->reliableAcknowledge + 1; i <= clc->reliableSequence; ++i)
        {
            MSG_WriteBits(&buf, 2, 3u);
            MSG_WriteLong(&buf, i);
            MSG_WriteString(&buf, clc->reliableCommands[i & 0x7F]);
        }
        oldPacketNum = (clc->netchan.outgoingSequence - 1 - cl_packetdup->current.integer) & (khz::SnapshotHistory - 1);
        count = LocalClientGlobals->cmdNumber - KHZ_ClientPackets()[oldPacketNum].p_cmdNumber;
        if (count > 32)
        {
            count = 32;
            Com_Printf(CON_CHANNEL_CLIENT, "MAX_PACKET_USERCMDS\n");
        }
        if (count >= 1)
        {
            if (cl_showSend->current.enabled)
                Com_Printf(CON_CHANNEL_CLIENT, "(%i)", count);
            if (!clc->demowaiting
                && !cl_nodelta->current.enabled
                && LocalClientGlobals->snap.valid
                && clc->serverMessageSequence == LocalClientGlobals->snap.messageNum)
            {
                MSG_WriteBits(&buf, 0, 3u);
            }
            else
            {
                MSG_WriteBits(&buf, 1, 3u);
            }
            MSG_WriteByte(&buf, count);
            key = clc->checksumFeed;
            key ^= clc->serverMessageSequence;
            v1 = Com_HashKey(clc->serverCommands[clc->serverCommandSequence & 0x7F], 32);
            key ^= v1;
            if (cl_debugMessageKey->current.enabled)
            {
                v7 = &clc->serverCommands[clc->serverCommandSequence & 0x7F][1];
                v8 = clc->serverCommands[clc->serverCommandSequence & 0x7F];
                v6 = clc->serverCommandSequence & 0x7F;
                v2 = Com_HashKey(v8, 32);
                Com_Printf(
                    CON_CHANNEL_CLIENT,
                    "key:%i, checksumFeed:%i, messageAcknowledge:%i, Com_HashKey:%i, servercommand(%i):'%s', len:%i\n",
                    key,
                    clc->checksumFeed,
                    clc->serverMessageSequence,
                    v2,
                    v6,
                    v8,
                    &v8[strlen(v8) + 1] - v7);
            }
            for (i = 0; i < count; ++i)
            {
                cmd = &KHZ_ClientCommands()[(LocalClientGlobals->cmdNumber - count + i + 1)
                    & (khz::InputHistory - 1)];
                MSG_WriteDeltaUsercmdKey(&buf, key, oldcmd, cmd);
                oldcmd = cmd;
            }
            MSG_WriteLong(&buf, LODWORD(LocalClientGlobals->cgameOrigin[0]));
            MSG_WriteLong(&buf, LODWORD(LocalClientGlobals->cgameOrigin[1]));
            MSG_WriteLong(&buf, LODWORD(LocalClientGlobals->cgameOrigin[2]));
            iassert(!IS_NAN((LocalClientGlobals->cgameVelocity)[0]) && !IS_NAN((LocalClientGlobals->cgameVelocity)[1]) && !IS_NAN((LocalClientGlobals->cgameVelocity)[2]));

            MSG_WriteLong(&buf, LocalClientGlobals->cgamePredictedDataServerTime);
            CL_SavePredictedOriginForServerTime(
                LocalClientGlobals,
                LocalClientGlobals->cgamePredictedDataServerTime,
                LocalClientGlobals->cgameOrigin,
                LocalClientGlobals->cgameVelocity,
                LocalClientGlobals->cgameViewangles,
                LocalClientGlobals->cgameBobCycle,
                LocalClientGlobals->cgameMovementDir);
        }
        MSG_WriteBits(&buf, 3, 3u);
        if (buf.cursize < 9)
            MyAssertHandler(".\\client_mp\\cl_input.cpp", 2063, 0, "%s", "buf.cursize >= CL_ENCODE_START");
        v3 = buf.data;

        // Copy the 9-byte uncompressed message header (CL_ENCODE_START) verbatim
        // before the compressed bit stream begins. Bytes 0-3 + 4-7 as ints, byte 8 raw.
        v4 = (uint8_t *)compressedBuf;
        *(uint32_t *)compressedBuf = *(uint32_t *)buf.data;
        *((uint32_t *)v4 + 1) = *((uint32_t *)v3 + 1);
        v4[8] = v3[8];
        if (buf.cursize > 0x800u)
            Com_Error(ERR_DROP, "Overflow compressed msg buf in CL_WritePacket()");
        compressedSize = MSG_WriteBitsCompress(
            0,
            (const uint8_t *)buf.data + 9,
            &(*compressedBuf)[9],
            buf.cursize - 9,
            sizeof(*compressedBuf) - 9)
            + 9;
        if (compressedSize < 9)
            Com_Error(ERR_DROP, "Overflow compressed msg buf in CL_WritePacket()");
        packetNum = clc->netchan.outgoingSequence & (khz::SnapshotHistory - 1);
        KHZ_ClientPackets()[packetNum].p_realtime = cls.realtime;
        KHZ_ClientPackets()[packetNum].p_serverTime = oldcmd->serverTime;
        KHZ_ClientPackets()[packetNum].p_cmdNumber = LocalClientGlobals->cmdNumber;
        clc->lastPacketSentTime = cls.realtime;
        if (cl_showSend->current.enabled)
        {
            v5 = NET_AdrToString(clc->netchan.remoteAddress);
            Com_Printf(CON_CHANNEL_CLIENT, "%i to %s\n", compressedSize, v5);
        }
        CL_Netchan_Transmit(&clc->netchan, (uint8_t *)compressedBuf, compressedSize);
        while (clc->netchan.unsentFragments)
            CL_Netchan_TransmitNextFragment(&clc->netchan);
        //LargeLocal::~LargeLocal(&compressedBuf_large_local);
    }
}

void __cdecl CL_SendCmd(int localClientNum)
{
    if (CL_ReadyToSendPacket(localClientNum))
        CL_WritePacket(localClientNum);
}

bool __cdecl Sys_IsLANAddress(netadr_t adr);
bool __cdecl CL_ReadyToSendPacket(int localClientNum)
{
    int oldPacketNum; // [esp+Ch] [ebp-8h]
    clientConnection_t *clc; // [esp+10h] [ebp-4h]

    clc = CL_GetLocalClientConnection(localClientNum);
    if (clc->demoplaying)
        return 0;
    
    connstate_t connstate = CL_GetLocalClientConnectionState(localClientNum);
    if (connstate == CA_CINEMATIC || connstate == CA_LOGO)
        return 0;
    if (cls.downloadTempName[0] && cls.realtime - clc->lastPacketSentTime < 50)
        return 0;
    if (connstate != CA_ACTIVE
        && connstate != CA_PRIMED
        && !cls.downloadTempName[0]
        && cls.realtime - clc->lastPacketSentTime < 1000)
    {
        return 0;
    }
    if (clc->netchan.remoteAddress.type == NA_LOOPBACK)
        return 1;
    if (Sys_IsLANAddress(clc->netchan.remoteAddress))
        return 1;
    oldPacketNum = (clc->netchan.outgoingSequence - 1) & (khz::SnapshotHistory - 1);
    return cls.realtime - KHZ_ClientPackets()[oldPacketNum].p_realtime >= 1000 / cl_maxpackets->current.integer;
}

void __cdecl CL_CreateCmdsDuringConnection(int localClientNum)
{
    connstate_t connstate = CL_GetLocalClientConnectionState(localClientNum);
    if (connstate >= CA_CONNECTED && connstate != CA_ACTIVE)
        CL_CreateNewCommands(localClientNum);
}

void __cdecl CL_CreateNewCommands(int localClientNum)
{
    usercmd_s result; // [esp+8h] [ebp-48h] BYREF
    _BYTE v2[32]; // [esp+28h] [ebp-28h] BYREF
    clientActive_t *LocalClientGlobals; // [esp+48h] [ebp-8h]
    int cmdNum; // [esp+4Ch] [ebp-4h]

    if (CL_GetLocalClientConnectionState(localClientNum) >= CA_PRIMED)
    {
        LocalClientGlobals = CL_GetLocalClientGlobals(localClientNum);
        cmdNum = ++LocalClientGlobals->cmdNumber & (khz::InputHistory - 1);
        memcpy(v2, CL_CreateCmd(&result, localClientNum), sizeof(v2));
        memcpy(&KHZ_ClientCommands()[cmdNum], v2, sizeof(KHZ_ClientCommands()[cmdNum]));
    }
}

usercmd_s *__cdecl CL_CreateCmd(usercmd_s *result, int localClientNum)
{
    clientActive_t *LocalClientGlobals; // [esp+Ch] [ebp-34h]
    float oldAngles; // [esp+10h] [ebp-30h]
    usercmd_s cmd; // [esp+1Ch] [ebp-24h] BYREF

    LocalClientGlobals = CL_GetLocalClientGlobals(localClientNum);
    oldAngles = LocalClientGlobals->viewangles[0];
    CL_AdjustAngles(localClientNum);
    memset(&cmd, 0, sizeof(cmd));
    if (!InputHasCatcher(localClientNum, KEYCATCH_LOCATION_SELECTION) || !CG_HandleLocationSelectionInput(localClientNum, &cmd))
    {
        CL_CmdButtons(localClientNum, &cmd);
        CL_KeyMove(localClientNum, &cmd);
        CL_MouseMove(localClientNum, &cmd);
        if (LocalClientGlobals->viewangles[0] - oldAngles <= 90.0)
        {
            if (oldAngles - LocalClientGlobals->viewangles[0] > 90.0)
                LocalClientGlobals->viewangles[0] = oldAngles - 90.0;
        }
        else
        {
            LocalClientGlobals->viewangles[0] = oldAngles + 90.0;
        }
        if (CG_VehLocalClientDriving(localClientNum))
        {
            cmd.buttons &= ~BUTTON_ATTACK;
            cmd.buttons &= ~BUTTON_ADS;
            cmd.buttons &= ~(BUTTON_FRAG | BUTTON_SMOKE);
            cmd.buttons &= ~BUTTON_SMOKE;
        }
    }
    CL_FinishMove(localClientNum, &cmd);
    KHZ_BenchmarkInput(&cmd);
    memcpy(result, &cmd, sizeof(usercmd_s));
    return result;
}

void __cdecl CL_AdjustAngles(int localClientNum)
{
    clientActive_t *client; // [esp+40h] [ebp-14h]
    float max; // [esp+44h] [ebp-10h]
    kbutton_t *kb; // [esp+48h] [ebp-Ch]
    float speed; // [esp+50h] [ebp-4h]

    kb = playersKb[localClientNum];
    client = CL_GetLocalClientGlobals(localClientNum);
    if ((client->snap.ps.pm_flags & PMF_FROZEN) != 0)
    {
        return;
    }

    if (kb[KEY_SPEED].active)
        speed = 0.001 * cls.frametime * cl_anglespeedkey->current.value;
    else
        speed = 0.001 * cls.frametime;

    if (!kb[KEY_STRAFE].active)
    {
        if (client->cgameMaxYawSpeed <= 0.0)
        {
            max = cl_yawspeed->current.value;
        }
        else
        {
            if ((client->cgameMaxYawSpeed - cl_yawspeed->current.value) < 0.0)
                max = client->cgameMaxYawSpeed;
            else
                max = cl_yawspeed->current.value;
        }

        client->viewangles[YAW] -= CL_KeyState(&kb[KEY_RIGHT]) * (speed * max);
        client->viewangles[YAW] += CL_KeyState(&kb[KEY_LEFT]) * (speed * max);
    }

    if (client->cgameMaxPitchSpeed <= 0.0)
    {
        max = cl_pitchspeed->current.value;
    }
    else
    {
        if ((client->cgameMaxPitchSpeed - cl_pitchspeed->current.value)< 0.0)
            max = client->cgameMaxPitchSpeed;
        else
            max = cl_pitchspeed->current.value;
    }

    client->viewangles[PITCH] -= CL_KeyState(&kb[KEY_LOOKUP]) * (speed * max);
    client->viewangles[PITCH] += CL_KeyState(&kb[KEY_LOOKDOWN]) * (speed * max);
}

float __cdecl CL_KeyState(kbutton_t *key)
{
    signed int msec; // [esp+Ch] [ebp-4h]

    msec = key->msec;
    key->msec = 0;
    if (key->active)
    {
        if (key->downtime)
            msec += InputMilliseconds() - key->downtime;
        else
            msec = InputMilliseconds();
        key->downtime = InputMilliseconds();
    }

    if (msec <= 0)
        return 0.0f;

    if (msec >= InputFrameMilliseconds())
        return 1.0f;

    iassert(InputFrameMilliseconds());

    return (float)((float)msec / (float)InputFrameMilliseconds());
}

void __cdecl CL_KeyMove(int localClientNum, usercmd_s *cmd)
{
    int side; // [esp+3Ch] [ebp-10h]
    kbutton_t *kb; // [esp+44h] [ebp-8h]
    int forward; // [esp+48h] [ebp-4h]

    //CL_GetLocalClientGlobals(localClientNum); // unused call
    if (playersKb[localClientNum][KEY_PRONE].active || playersKb[localClientNum][KEY_DOWN].active)
    {
        if (playersKb[localClientNum][KEY_PRONE].active)
        {
            cmd->buttons |= BUTTON_PRONE;
            cmd->buttons = cmd->buttons & ~BUTTON_CROUCH;
        }
        else
        {
            cmd->buttons |= BUTTON_CROUCH;
            cmd->buttons = cmd->buttons & ~BUTTON_PRONE;
        }
        cmd->buttons |= BUTTON_TEMP_STANCE;
    }
    else
    {
        CL_StanceButtonUpdate(localClientNum);
        CL_AddCurrentStanceToCmd(localClientNum, cmd);
    }

    kb = playersKb[localClientNum];

    if (kb[KEY_SPEED].active == !CL_GetLocalClientGlobals(localClientNum)->usingAds)
        cmd->buttons = cmd->buttons | BUTTON_ADS;
    else
        cmd->buttons = cmd->buttons & ~BUTTON_ADS;

    forward = 0;
    side = 0;
    //up = 0;

    side += (int)(CL_KeyState(&kb[KEY_MOVERIGHT]) * 127.0f);
    side -= (int)(CL_KeyState(&kb[KEY_MOVELEFT]) * 127.0f);
    forward += (int)(CL_KeyState(&kb[KEY_FORWARD]) * 127.0f);
    forward -= (int)(CL_KeyState(&kb[KEY_BACK]) * 127.0f);

    if (!kb[KEY_BACK].active)
    {
        if (kb[KEY_SPRINT].active || kb[KEY_SPRINT].wasPressed)
        {
            cmd->buttons |= BUTTON_SPRINT;
            kb[KEY_SPRINT].wasPressed = 0;
        }
        else
        {
            cmd->buttons &= ~BUTTON_SPRINT;
        }
    }

    if (kb[KEY_STRAFE].active && (cmd->buttons & BUTTON_SPRINT) == 0)
    {
        side += (int)(CL_KeyState(&kb[KEY_RIGHT]) * (double)127);
        side -= (int)(CL_KeyState(&kb[KEY_LEFT]) * (double)127);
    }
    cmd->forwardmove = ClampChar(forward);
    cmd->rightmove = ClampChar(side);
    //cmd->upmove = ClampChar(up);
}

int __cdecl IN_IsTempStanceKeyActive(int localClientNum)
{
    return playersKb[localClientNum][KEY_PRONE].active || playersKb[localClientNum][KEY_DOWN].active;
}

void __cdecl CL_StanceButtonUpdate(int localClientNum)
{
    clientActive_t *LocalClientGlobals; // [esp+4h] [ebp-4h]

    iassert(!IN_IsTempStanceKeyActive(localClientNum));

    LocalClientGlobals = CL_GetLocalClientGlobals(localClientNum);
    if (LocalClientGlobals->stanceHeld
        && InputMilliseconds() - LocalClientGlobals->stanceTime >= cl_stanceHoldTime->current.integer)
    {
        if (LocalClientGlobals->stancePosition == CL_STANCE_PRONE)
            LocalClientGlobals->stance = CL_STANCE_STAND;
        else
            LocalClientGlobals->stance = CL_STANCE_PRONE;
        LocalClientGlobals->stanceHeld = 0;
    }
}

void __cdecl CL_AddCurrentStanceToCmd(int localClientNum, usercmd_s *cmd)
{
    StanceState stance; // [esp+0h] [ebp-8h]
    clientActive_t *cl; // [esp+4h] [ebp-4h]

    cl = CL_GetLocalClientGlobals(localClientNum);
    stance = cl->stance;
    if (stance == CL_STANCE_CROUCH)
    {
        cmd->buttons |= BUTTON_CROUCH;
        cmd->buttons &= ~BUTTON_PRONE;
    }
    else if (stance == CL_STANCE_PRONE)
    {
        cmd->buttons |= BUTTON_PRONE;
        cmd->buttons &= ~BUTTON_CROUCH;
    }
    else
    {
        iassert(cl->stance == CL_STANCE_STAND);
        cmd->buttons &= ~BUTTON_PRONE;
        cmd->buttons &= ~BUTTON_CROUCH;
    }
    cmd->buttons &= ~BUTTON_TEMP_STANCE;
}

void __cdecl CL_MouseMove(int localClientNum, usercmd_s *cmd)
{
    float v2; // [esp+10h] [ebp-D4h]
    float v3; // [esp+14h] [ebp-D0h]
    float v4; // [esp+18h] [ebp-CCh]
    float v5; // [esp+24h] [ebp-C0h]
    float v6; // [esp+28h] [ebp-BCh]
    float v7; // [esp+2Ch] [ebp-B8h]
    float v8; // [esp+40h] [ebp-A4h]
    float v10; // [esp+58h] [ebp-8Ch]
    float v11; // [esp+5Ch] [ebp-88h]
    float v12; // [esp+60h] [ebp-84h]
    float v13; // [esp+64h] [ebp-80h]
    float v15; // [esp+80h] [ebp-64h]
    clientActive_t *LocalClientGlobals; // [esp+84h] [ebp-60h]
    float rate; // [esp+88h] [ebp-5Ch]
    float delta; // [esp+8Ch] [ebp-58h]
    float deltaa; // [esp+8Ch] [ebp-58h]
    AimInput aimInput; // [esp+90h] [ebp-54h] BYREF
    kbutton_t *kb; // [esp+C0h] [ebp-24h]
    float mx; // [esp+C4h] [ebp-20h] BYREF
    float my; // [esp+C8h] [ebp-1Ch] BYREF
    AimOutput aimOutput; // [esp+CCh] [ebp-18h] BYREF
    float cap; // [esp+DCh] [ebp-8h]
    float accelSensitivity; // [esp+E0h] [ebp-4h]

    LocalClientGlobals = CL_GetLocalClientGlobals(localClientNum);
    CL_GetMouseMovement(LocalClientGlobals, &mx, &my);
    if (InputFrameMilliseconds())
    {
        v15 = my * my + mx * mx;
        v8 = sqrt(v15);
        rate = v8 / (double)InputFrameMilliseconds();
        accelSensitivity = rate * cl_mouseAccel->current.value + cl_sensitivity->current.value;
        accelSensitivity = accelSensitivity * LocalClientGlobals->cgameFOVSensitivityScale;
        if (rate != 0.0 && cl_showMouseRate->current.enabled)
            Com_Printf(CON_CHANNEL_CLIENT, "%f : %f\n", rate, accelSensitivity);
        if ((LocalClientGlobals->snap.ps.pm_flags & PMF_FROZEN) == 0)
        {
            mx = mx * accelSensitivity;
            my = my * accelSensitivity;
            if (mx != 0.0 || my != 0.0)
            {
                kb = playersKb[localClientNum];
                if (kb[KEY_STRAFE].active)
                {
                    cmd->rightmove = ClampChar(SnapFloatToInt(mx * m_side->current.value) + cmd->rightmove);
                }
                else
                {
                    delta = m_yaw->current.value * mx;
                    if (LocalClientGlobals->cgameMaxYawSpeed > 0.0)
                    {
                        cap = (double)InputFrameMilliseconds() * LocalClientGlobals->cgameMaxYawSpeed * EQUAL_EPSILON;
                        v7 = delta - cap;
                        if (v7 < 0.0)
                            v13 = delta;
                        else
                            v13 = cap;
                        v12 = -cap;
                        v6 = v12 - v13;
                        if (v6 < 0.0)
                            v5 = v13;
                        else
                            v5 = -cap;
                        delta = v5;
                    }
                    LocalClientGlobals->viewangles[1] = LocalClientGlobals->viewangles[1] - delta;
                }
                if ((kb[KEY_MLOOK].active || cl_freelook->current.enabled) && !kb[KEY_STRAFE].active)
                {
                    deltaa = m_pitch->current.value * my;
                    if (LocalClientGlobals->cgameMaxPitchSpeed > 0.0)
                    {
                        cap = (double)InputFrameMilliseconds() * LocalClientGlobals->cgameMaxPitchSpeed * EQUAL_EPSILON;
                        v4 = deltaa - cap;
                        if (v4 < 0.0)
                            v11 = deltaa;
                        else
                            v11 = cap;
                        v10 = -cap;
                        v3 = v10 - v11;
                        if (v3 < 0.0)
                            v2 = v11;
                        else
                            v2 = -cap;
                        deltaa = v2;
                    }
                    LocalClientGlobals->viewangles[0] = LocalClientGlobals->viewangles[0] + deltaa;
                }
                else
                {
                    cmd->forwardmove = ClampChar(cmd->forwardmove - SnapFloatToInt(my * m_forward->current.value));
                }
            }
            aimInput.deltaTime = (double)cls.frametime * EQUAL_EPSILON;
            aimInput.pitch = LocalClientGlobals->viewangles[0];
            aimInput.pitchAxis = 0.0;
            aimInput.pitchMax = 0.0;
            aimInput.yaw = LocalClientGlobals->viewangles[1];
            aimInput.yawAxis = 0.0;
            aimInput.yawMax = 0.0;
            aimInput.forwardAxis = 0.0;
            aimInput.rightAxis = 0.0;
            aimInput.buttons = cmd->buttons;
            aimInput.localClientNum = 0;
            aimInput.ps = CG_GetPredictedPlayerState(0);
            AimAssist_UpdateMouseInput(&aimInput, &aimOutput);
            LocalClientGlobals->viewangles[0] = aimOutput.pitch;
            LocalClientGlobals->viewangles[1] = aimOutput.yaw;
            cmd->meleeChargeYaw = aimOutput.meleeChargeYaw;
            cmd->meleeChargeDist = aimOutput.meleeChargeDist;
        }
    }
}

void __cdecl CL_GetMouseMovement(clientActive_t *cl, float *mx, float *my)
{
    iassert(mx);
    iassert(my);

    if (m_filter->current.enabled)
    {
        *mx = (double)(cl->mouseDx[1] + cl->mouseDx[0]) * 0.5f;
        *my = (double)(cl->mouseDy[1] + cl->mouseDy[0]) * 0.5f;
    }
    else
    {
        *mx = (float)cl->mouseDx[cl->mouseIndex];
        *my = (double)cl->mouseDy[cl->mouseIndex];
    }

    cl->mouseIndex ^= 1u;
    cl->mouseDx[cl->mouseIndex] = 0;
    cl->mouseDy[cl->mouseIndex] = 0;
}

void __cdecl CL_CmdButtons(int localClientNum, usercmd_s *cmd)
{
    clientActive_t *LocalClientGlobals; // [esp+8h] [ebp-Ch]

    CL_UpdateCmdButton(localClientNum, &cmd->buttons, 14, BUTTON_ATTACK);
    CL_UpdateCmdButton(localClientNum, &cmd->buttons, 15, BUTTON_BREATH);
    CL_UpdateCmdButton(localClientNum, &cmd->buttons, 16, BUTTON_FRAG);
    CL_UpdateCmdButton(localClientNum, &cmd->buttons, 17, BUTTON_SMOKE);
    CL_UpdateCmdButton(localClientNum, &cmd->buttons, 18, BUTTON_MELEE);
    CL_UpdateCmdButton(localClientNum, &cmd->buttons, 19, BUTTON_USE);
    CL_UpdateCmdButton(localClientNum, &cmd->buttons, 20, BUTTON_RELOAD);
    CL_UpdateCmdButton(localClientNum, &cmd->buttons, 21, BUTTON_USE_RELOAD);
    CL_UpdateCmdButton(localClientNum, &cmd->buttons, 22, BUTTON_LEAN_LEFT);
    CL_UpdateCmdButton(localClientNum, &cmd->buttons, 23, BUTTON_LEAN_RIGHT);
    CL_UpdateCmdButton(localClientNum, &cmd->buttons, 24, BUTTON_PRONE);
    CL_UpdateCmdButton(localClientNum, &cmd->buttons, 25, BUTTON_CROUCH);
    CL_UpdateCmdButton(localClientNum, &cmd->buttons, 10, BUTTON_JUMP);
    CL_UpdateCmdButton(localClientNum, &cmd->buttons, 26, BUTTON_THROW);

    // Input catchers are immutable main-to-owner metadata during this tick.

#ifdef COD4X1KHZ
    if (KHZ_IsClientSimulationThread() ? (InputCatchers(localClientNum) & ~KEYCATCH_LOCATION_SELECTION) != 0
        : clientUIActives[0].keyCatchers && !cl_bypassMouseInput->current.enabled && UI_GetActiveMenu(localClientNum) != UIMENU_SCOREBOARD)
#else
    if (clientUIActives[0].keyCatchers && !cl_bypassMouseInput->current.enabled && UI_GetActiveMenu(localClientNum) != UIMENU_SCOREBOARD)
#endif
        cmd->buttons |= BUTTON_LOC_SELECTING;

    if (CL_GetLocalClientConnectionState(localClientNum) > CA_CONNECTED)
    {
        LocalClientGlobals = CL_GetLocalClientGlobals(localClientNum);
        if (LocalClientGlobals->snap.ps.pm_type == PM_NOCLIP
            || LocalClientGlobals->snap.ps.pm_type == PM_UFO
            || LocalClientGlobals->snap.ps.pm_type == PM_SPECTATOR)
        {
            CL_UpdateCmdButton(localClientNum, &cmd->buttons, 12, BUTTON_JUMP);
        }
    }
    iassert(cmd->buttons < (1 << BUTTON_BIT_COUNT));
}

void __cdecl CL_FinishMove(int localClientNum, usercmd_s *cmd)
{
    int serverTime; // [esp+0h] [ebp-Ch]
    clientActive_t *LocalClientGlobals; // [esp+4h] [ebp-8h]
    int i; // [esp+8h] [ebp-4h]

    LocalClientGlobals = CL_GetLocalClientGlobals(localClientNum);
    cmd->weapon = LocalClientGlobals->cgameUserCmdWeapon;
    cmd->offHandIndex = LocalClientGlobals->cgameUserCmdOffHandIndex;
    if (LocalClientGlobals->serverTime - LocalClientGlobals->snap.serverTime > 5000)
        serverTime = LocalClientGlobals->snap.serverTime + 5000;
    else
        serverTime = LocalClientGlobals->serverTime;
    cmd->serverTime = KHZ_InputTime();
    for (i = 0; i < 3; ++i)
        cmd->angles[i] = (uint16_t)(int)((LocalClientGlobals->viewangles[i] + LocalClientGlobals->cgameKickAngles[i])
            * 182.0444488525391);
    cmd->buttons |= LocalClientGlobals->cgameExtraButtons;
    LocalClientGlobals->cgameExtraButtons = 0;
    if (CG_VehLocalClientDriving(localClientNum))
    {
        cmd->angles[1] = (uint16_t)(int)(LocalClientGlobals->vehicleViewYaw * 182.0444488525391);
        cmd->angles[0] = (uint16_t)(int)(LocalClientGlobals->vehicleViewPitch * 182.0444488525391);
        cmd->angles[2] = 0;
    }
}

char __cdecl CG_HandleLocationSelectionInput(int localClientNum, usercmd_s *cmd)
{
    float v5; // [esp+28h] [ebp-30h]
    float v6; // [esp+2Ch] [ebp-2Ch]
    float v7; // [esp+30h] [ebp-28h]
    float v8; // [esp+34h] [ebp-24h]
    clientActive_t *LocalClientGlobals; // [esp+38h] [ebp-20h]
    float mapAspectRatio; // [esp+40h] [ebp-18h]
    float mx; // [esp+48h] [ebp-10h] BYREF
    float my; // [esp+4Ch] [ebp-Ch] BYREF
    float frametime; // [esp+50h] [ebp-8h]
    LocSelInputState locSelInputState; // [esp+54h] [ebp-4h]

    cg_s *cgameGlob = CG_GetLocalClientGlobals(localClientNum);

    if (cgameGlob->predictedPlayerState.locationSelectionInfo)
    {
        CL_AddCurrentStanceToCmd(localClientNum, cmd);
        cmd->buttons |= BUTTON_LOC_SELECTING;
        frametime = (double)cgameGlob->frametime * EQUAL_EPSILON;
        mapAspectRatio = cgameGlob->compassMapWorldSize[0] / cgameGlob->compassMapWorldSize[1];
        LocalClientGlobals = CL_GetLocalClientGlobals(localClientNum);
        CL_GetMouseMovement(LocalClientGlobals, &mx, &my);
        cgameGlob->selectedLocation[0] = mx * cg_mapLocationSelectionCursorSpeed->current.value * frametime * (float)0.1
            + cgameGlob->selectedLocation[0];
        cgameGlob->selectedLocation[1] = my
            * mapAspectRatio
            * cg_mapLocationSelectionCursorSpeed->current.value
            * frametime
            * (float)0.1
            + cgameGlob->selectedLocation[1];
        v8 = cgameGlob->selectedLocation[0];
        if (0.0 >= 1.0)
            MyAssertHandler("c:\\trees\\cod3\\src\\universal\\com_math.h", 533, 0, "%s", "min < max");
        if (v8 >= 0.0)
        {
            if (v8 <= 1.0)
                v7 = v8;
            else
                v7 = 1.0;
        }
        else
        {
            v7 = 0.0;
        }
        cgameGlob->selectedLocation[0] = v7;
        v6 = cgameGlob->selectedLocation[1];
        if (0.0 >= 1.0)
            MyAssertHandler("c:\\trees\\cod3\\src\\universal\\com_math.h", 533, 0, "%s", "min < max");
        if (v6 >= 0.0)
        {
            if (v6 <= 1.0)
                v5 = v6;
            else
                v5 = 1.0;
        }
        else
        {
            v5 = 0.0;
        }
        cgameGlob->selectedLocation[1] = v5;
#ifdef COD4X1KHZ
        locSelInputState = KHZ_IsClientSimulationThread() ? ownerLocSelInputState : playerKeys[localClientNum].locSelInputState;
#else
        locSelInputState = playerKeys[localClientNum].locSelInputState;
#endif
        if (locSelInputState == LOC_SEL_INPUT_CONFIRM)
        {
            cmd->buttons |= BUTTON_LOC_CONFIRM;
            cmd->selectedLocation[0] = SnapFloatToInt(cgameGlob->selectedLocation[0] * 255.0f) + 0x80;
            cmd->selectedLocation[1] = SnapFloatToInt(cgameGlob->selectedLocation[1] * 255.0f) + 0x80;
        }
        else if (locSelInputState == LOC_SEL_INPUT_CANCEL)
        {
            cmd->buttons |= BUTTON_LOC_CANCEL;
        }
        return 1;
    }
    else
    {
#ifdef COD4X1KHZ
        if (KHZ_IsClientSimulationThread()) KHZ_ClientOwnerSetLocationSelection(false);
        else
#endif
        Key_RemoveCatcher(localClientNum, ~KEYCATCH_LOCATION_SELECTION);
        return 0;
    }
}

void __cdecl CL_Input(int localClientNum)
{
    if (CL_GetLocalClientConnectionState(localClientNum) == CA_ACTIVE)
    {
#ifdef COD4X1KHZ
        if (!KHZ_IsClientSimulationThread())
#endif
        IN_Frame();
        CL_CreateNewCommands(localClientNum);
    }
}

void __cdecl IN_TalkDown()
{
    IN_KeyDown(&playersKb[0][KEY_TALK]);
}

void __cdecl IN_TalkUp()
{
    IN_KeyUp(&playersKb[0][KEY_TALK]);
}

cmd_function_s IN_CenterView_VAR;
cmd_function_s IN_UpDown_VAR;
cmd_function_s IN_UpUp_VAR;
cmd_function_s IN_DownDown_VAR;
cmd_function_s IN_DownUp_VAR;
cmd_function_s IN_LeftDown_VAR;
cmd_function_s IN_LeftUp_VAR;
cmd_function_s IN_RightDown_VAR;
cmd_function_s IN_RightUp_VAR;
cmd_function_s IN_ForwardDown_VAR;
cmd_function_s IN_ForwardUp_VAR;
cmd_function_s IN_BackDown_VAR;
cmd_function_s IN_BackUp_VAR;
cmd_function_s IN_LookupDown_VAR;
cmd_function_s IN_LookupUp_VAR;
cmd_function_s IN_LookdownDown_VAR;
cmd_function_s IN_LookdownUp_VAR;
cmd_function_s IN_StrafeDown_VAR;
cmd_function_s IN_StrafeUp_VAR;
cmd_function_s IN_MoveleftDown_VAR;
cmd_function_s IN_MoveleftUp_VAR;
cmd_function_s IN_MoverightDown_VAR;
cmd_function_s IN_MoverightUp_VAR;
cmd_function_s IN_SpeedDown_VAR;
cmd_function_s IN_SpeedUp_VAR;
cmd_function_s IN_Attack_Down_VAR;
cmd_function_s IN_Attack_Up_VAR;
cmd_function_s IN_Melee_Down_VAR;
cmd_function_s IN_Melee_Up_VAR;
cmd_function_s IN_Breath_Down_VAR;
cmd_function_s IN_Breath_Up_VAR;
cmd_function_s IN_MeleeBreath_Down_VAR;
cmd_function_s IN_MeleeBreath_Up_VAR;
cmd_function_s IN_Frag_Down_VAR;
cmd_function_s IN_Frag_Up_VAR;
cmd_function_s IN_Smoke_Down_VAR;
cmd_function_s IN_Smoke_Up_VAR;
cmd_function_s IN_BreathSprint_Down_VAR;
cmd_function_s IN_BreathSprint_Up_VAR;
cmd_function_s IN_Activate_Down_VAR;
cmd_function_s IN_Activate_Up_VAR;
cmd_function_s IN_Reload_Down_VAR;
cmd_function_s IN_Reload_Up_VAR;
cmd_function_s IN_UseReload_Down_VAR;
cmd_function_s IN_UseReload_Up_VAR;
cmd_function_s IN_LeanLeft_Down_VAR;
cmd_function_s IN_LeanLeft_Up_VAR;
cmd_function_s IN_LeanRight_Down_VAR;
cmd_function_s IN_LeanRight_Up_VAR;
cmd_function_s IN_Prone_Down_VAR;
cmd_function_s IN_Prone_Up_VAR;
cmd_function_s IN_Stance_Down_VAR;
cmd_function_s IN_Stance_Up_VAR;
cmd_function_s IN_MLookDown_VAR;
cmd_function_s IN_MLookUp_VAR;
cmd_function_s IN_ToggleADS_VAR;
cmd_function_s IN_LeaveADS_VAR;
cmd_function_s IN_Throw_Down_VAR;
cmd_function_s IN_Throw_Up_VAR;
cmd_function_s IN_Speed_Throw_Down_VAR;
cmd_function_s IN_Speed_Throw_Up_VAR;
cmd_function_s IN_ToggleADS_Throw_Down_VAR;
cmd_function_s IN_ToggleADS_Throw_Up_VAR;
cmd_function_s IN_LowerStance_VAR;
cmd_function_s IN_RaiseStance_VAR;
cmd_function_s IN_ToggleCrouch_VAR;
cmd_function_s IN_ToggleProne_VAR;
cmd_function_s IN_GoProne_VAR;
cmd_function_s IN_GoCrouch_VAR;
cmd_function_s IN_GoStandDown_VAR;
cmd_function_s IN_GoStandUp_VAR;
cmd_function_s IN_TalkDown_VAR;
cmd_function_s IN_TalkUp_VAR;
cmd_function_s IN_SprintDown_VAR;
cmd_function_s IN_SprintUp_VAR;

void __cdecl CL_InitInput()
{
    DvarLimits min; // [esp+4h] [ebp-10h]

    Cmd_AddCommandInternal("centerview", IN_CenterView, &IN_CenterView_VAR);
    Cmd_AddCommandInternal("+moveup", IN_UpDown, &IN_UpDown_VAR);
    Cmd_AddCommandInternal("-moveup", IN_UpUp, &IN_UpUp_VAR);
    Cmd_AddCommandInternal("+movedown", IN_DownDown, &IN_DownDown_VAR);
    Cmd_AddCommandInternal("-movedown", IN_DownUp, &IN_DownUp_VAR);
    Cmd_AddCommandInternal("+left", IN_LeftDown, &IN_LeftDown_VAR);
    Cmd_AddCommandInternal("-left", IN_LeftUp, &IN_LeftUp_VAR);
    Cmd_AddCommandInternal("+right", IN_RightDown, &IN_RightDown_VAR);
    Cmd_AddCommandInternal("-right", IN_RightUp, &IN_RightUp_VAR);
    Cmd_AddCommandInternal("+forward", IN_ForwardDown, &IN_ForwardDown_VAR);
    Cmd_AddCommandInternal("-forward", IN_ForwardUp, &IN_ForwardUp_VAR);
    Cmd_AddCommandInternal("+back", IN_BackDown, &IN_BackDown_VAR);
    Cmd_AddCommandInternal("-back", IN_BackUp, &IN_BackUp_VAR);
    Cmd_AddCommandInternal("+lookup", IN_LookupDown, &IN_LookupDown_VAR);
    Cmd_AddCommandInternal("-lookup", IN_LookupUp, &IN_LookupUp_VAR);
    Cmd_AddCommandInternal("+lookdown", IN_LookdownDown, &IN_LookdownDown_VAR);
    Cmd_AddCommandInternal("-lookdown", IN_LookdownUp, &IN_LookdownUp_VAR);
    Cmd_AddCommandInternal("+strafe", IN_StrafeDown, &IN_StrafeDown_VAR);
    Cmd_AddCommandInternal("-strafe", IN_StrafeUp, &IN_StrafeUp_VAR);
    Cmd_AddCommandInternal("+moveleft", IN_MoveleftDown, &IN_MoveleftDown_VAR);
    Cmd_AddCommandInternal("-moveleft", IN_MoveleftUp, &IN_MoveleftUp_VAR);
    Cmd_AddCommandInternal("+moveright", IN_MoverightDown, &IN_MoverightDown_VAR);
    Cmd_AddCommandInternal("-moveright", IN_MoverightUp, &IN_MoverightUp_VAR);
    Cmd_AddCommandInternal("+speed", IN_SpeedDown, &IN_SpeedDown_VAR);
    Cmd_AddCommandInternal("-speed", IN_SpeedUp, &IN_SpeedUp_VAR);
    Cmd_AddCommandInternal("+attack", IN_Attack_Down, &IN_Attack_Down_VAR);
    Cmd_AddCommandInternal("-attack", IN_Attack_Up, &IN_Attack_Up_VAR);
    Cmd_AddCommandInternal("+melee", IN_Melee_Down, &IN_Melee_Down_VAR);
    Cmd_AddCommandInternal("-melee", IN_Melee_Up, &IN_Melee_Up_VAR);
    Cmd_AddCommandInternal("+holdbreath", IN_Breath_Down, &IN_Breath_Down_VAR);
    Cmd_AddCommandInternal("-holdbreath", IN_Breath_Up, &IN_Breath_Up_VAR);
    Cmd_AddCommandInternal("+melee_breath", IN_MeleeBreath_Down, &IN_MeleeBreath_Down_VAR);
    Cmd_AddCommandInternal("-melee_breath", IN_MeleeBreath_Up, &IN_MeleeBreath_Up_VAR);
    Cmd_AddCommandInternal("+frag", IN_Frag_Down, &IN_Frag_Down_VAR);
    Cmd_AddCommandInternal("-frag", IN_Frag_Up, &IN_Frag_Up_VAR);
    Cmd_AddCommandInternal("+smoke", IN_Smoke_Down, &IN_Smoke_Down_VAR);
    Cmd_AddCommandInternal("-smoke", IN_Smoke_Up, &IN_Smoke_Up_VAR);
    Cmd_AddCommandInternal("+breath_sprint", IN_BreathSprint_Down, &IN_BreathSprint_Down_VAR);
    Cmd_AddCommandInternal("-breath_sprint", IN_BreathSprint_Up, &IN_BreathSprint_Up_VAR);
    Cmd_AddCommandInternal("+activate", IN_Activate_Down, &IN_Activate_Down_VAR);
    Cmd_AddCommandInternal("-activate", IN_Activate_Up, &IN_Activate_Up_VAR);
    Cmd_AddCommandInternal("+reload", IN_Reload_Down, &IN_Reload_Down_VAR);
    Cmd_AddCommandInternal("-reload", IN_Reload_Up, &IN_Reload_Up_VAR);
    Cmd_AddCommandInternal("+usereload", IN_UseReload_Down, &IN_UseReload_Down_VAR);
    Cmd_AddCommandInternal("-usereload", IN_UseReload_Up, &IN_UseReload_Up_VAR);
    Cmd_AddCommandInternal("+leanleft", IN_LeanLeft_Down, &IN_LeanLeft_Down_VAR);
    Cmd_AddCommandInternal("-leanleft", IN_LeanLeft_Up, &IN_LeanLeft_Up_VAR);
    Cmd_AddCommandInternal("+leanright", IN_LeanRight_Down, &IN_LeanRight_Down_VAR);
    Cmd_AddCommandInternal("-leanright", IN_LeanRight_Up, &IN_LeanRight_Up_VAR);
    Cmd_AddCommandInternal("+prone", IN_Prone_Down, &IN_Prone_Down_VAR);
    Cmd_AddCommandInternal("-prone", IN_Prone_Up, &IN_Prone_Up_VAR);
    Cmd_AddCommandInternal("+stance", IN_Stance_Down, &IN_Stance_Down_VAR);
    Cmd_AddCommandInternal("-stance", IN_Stance_Up, &IN_Stance_Up_VAR);
    Cmd_AddCommandInternal("+mlook", IN_MLookDown, &IN_MLookDown_VAR);
    Cmd_AddCommandInternal("-mlook", IN_MLookUp, &IN_MLookUp_VAR);
    Cmd_AddCommandInternal("toggleads", IN_ToggleADS, &IN_ToggleADS_VAR);
    Cmd_AddCommandInternal("leaveads", IN_LeaveADS, &IN_LeaveADS_VAR);
    Cmd_AddCommandInternal("+throw", IN_Throw_Down, &IN_Throw_Down_VAR);
    Cmd_AddCommandInternal("-throw", IN_Throw_Up, &IN_Throw_Up_VAR);
    Cmd_AddCommandInternal("+speed_throw", IN_Speed_Throw_Down, &IN_Speed_Throw_Down_VAR);
    Cmd_AddCommandInternal("-speed_throw", IN_Speed_Throw_Up, &IN_Speed_Throw_Up_VAR);
    Cmd_AddCommandInternal("+toggleads_throw", IN_ToggleADS_Throw_Down, &IN_ToggleADS_Throw_Down_VAR);
    Cmd_AddCommandInternal("-toggleads_throw", IN_ToggleADS_Throw_Up, &IN_ToggleADS_Throw_Up_VAR);
    Cmd_AddCommandInternal("lowerstance", IN_LowerStance, &IN_LowerStance_VAR);
    Cmd_AddCommandInternal("raisestance", IN_RaiseStance, &IN_RaiseStance_VAR);
    Cmd_AddCommandInternal("togglecrouch", IN_ToggleCrouch, &IN_ToggleCrouch_VAR);
    Cmd_AddCommandInternal("toggleprone", IN_ToggleProne, &IN_ToggleProne_VAR);
    Cmd_AddCommandInternal("goprone", IN_GoProne, &IN_GoProne_VAR);
    Cmd_AddCommandInternal("gocrouch", IN_GoCrouch, &IN_GoCrouch_VAR);
    Cmd_AddCommandInternal("+gostand", IN_GoStandDown, &IN_GoStandDown_VAR);
    Cmd_AddCommandInternal("-gostand", IN_GoStandUp, &IN_GoStandUp_VAR);
    Cmd_AddCommandInternal("+talk", IN_TalkDown, &IN_TalkDown_VAR);
    Cmd_AddCommandInternal("-talk", IN_TalkUp, &IN_TalkUp_VAR);
    Cmd_AddCommandInternal("+sprint", IN_SprintDown, &IN_SprintDown_VAR);
    Cmd_AddCommandInternal("-sprint", IN_SprintUp, &IN_SprintUp_VAR);
    min.value.max = 1.0f;
    min.value.min = 0.000099999997f;
    cl_analog_attack_threshold = Dvar_RegisterFloat(
        "cl_analog_attack_threshold",
        0.80000001f,
        min,
        DVAR_NOFLAG,
        "The threshold before firing");
    cl_stanceHoldTime = Dvar_RegisterInt(
        "cl_stanceHoldTime",
        300,
        (DvarLimits)0x3E800000000LL,
        DVAR_NOFLAG,
        "The time to hold the stance button before the player goes prone");
    cl_nodelta = Dvar_RegisterBool("cl_nodelta", false, DVAR_NOFLAG, "The server does not send snapshot deltas");
}

void __cdecl IN_MLookDown()
{
    playersKb[0][KEY_MLOOK].active = 1;
}

void __cdecl IN_MLookUp()
{
    if (clientUIActives[0].connectionState >= CA_CONNECTED)
    {
        playersKb[0][KEY_MLOOK].active = 0;
        if (!cl_freelook->current.enabled)
            IN_CenterView();
    }
}

void __cdecl IN_UpDown()
{
    StanceState stance; // [esp+0h] [ebp-10h]
    bool v1; // [esp+4h] [ebp-Ch]
    clientActive_t *LocalClientGlobals; // [esp+8h] [ebp-8h]

    IN_KeyDown(&playersKb[0][KEY_UP]);
    v1 = playersKb[0][KEY_PRONE].active || playersKb[0][KEY_DOWN].active;
    if (!v1 && clientUIActives[0].connectionState >= CA_CONNECTED)
    {
        LocalClientGlobals = CL_GetLocalClientGlobals(0);
        stance = LocalClientGlobals->stance;
        if (stance == CL_STANCE_CROUCH)
        {
            LocalClientGlobals->stance = CL_STANCE_STAND;
        }
        else if (stance == CL_STANCE_PRONE)
        {
            LocalClientGlobals->stance = CL_STANCE_CROUCH;
        }
        else
        {
            if (LocalClientGlobals->stance)
                MyAssertHandler(".\\client_mp\\cl_input.cpp", 324, 1, "%s", "cl->stance == CL_STANCE_STAND");
            IN_KeyDown(&playersKb[0][KEY_MOVEUP]);
        }
    }
}

void __cdecl IN_KeyDown(kbutton_t *b)
{
#ifdef COD4X1KHZ
    if (ownerTypedInput) {
        const int key = ownerTypedKey;
        if (key == b->down[0] || key == b->down[1]) return;
        if (!b->down[0]) b->down[0] = key;
        else if (!b->down[1]) b->down[1] = key;
        else return; // Stock supports two keys per action; avoid tick logging.
        if (!b->active) { b->downtime = ownerTypedMilliseconds; b->active = 1; b->wasPressed = 1; }
        return;
    }
#endif
    const char *c; // [esp+0h] [ebp-8h]
    const char *ca; // [esp+0h] [ebp-8h]
    int k; // [esp+4h] [ebp-4h]

    c = Cmd_Argv(1);
    if (*c)
        k = atoi(c);
    else
        k = -1;
    if (k != b->down[0] && k != b->down[1])
    {
        if (b->down[0])
        {
            if (b->down[1])
            {
                Com_Printf(CON_CHANNEL_CLIENT, "Three keys down for a button!\n");
                return;
            }
            b->down[1] = k;
        }
        else
        {
            b->down[0] = k;
        }
        if (!b->active)
        {
            ca = Cmd_Argv(2);
            b->downtime = atoi(ca);
            b->active = 1;
            b->wasPressed = 1;
        }
    }
}

void __cdecl IN_UpUp()
{
    IN_KeyUp(&playersKb[0][KEY_UP]);
    IN_KeyUp(&playersKb[0][KEY_MOVEUP]);
}

void __cdecl IN_KeyUp(kbutton_t *b)
{
#ifdef COD4X1KHZ
    if (ownerTypedInput) {
        if (b->down[0] == ownerTypedKey) b->down[0] = 0;
        else if (b->down[1] == ownerTypedKey) b->down[1] = 0;
        else return;
        if (!b->down[0] && !b->down[1]) {
            b->msec += ownerTypedMilliseconds - b->downtime;
            b->active = 0;
        }
        return;
    }
#endif
    uint32_t v1; // edx
    const char *c; // [esp+0h] [ebp-Ch]
    const char *ca; // [esp+0h] [ebp-Ch]
    uint32_t uptime; // [esp+4h] [ebp-8h]
    int k; // [esp+8h] [ebp-4h]

    c = Cmd_Argv(1);
    if (!*c)
    {
        b->down[1] = 0;
        b->down[0] = 0;
        b->active = 0;
        return;
    }
    k = atoi(c);
    if (b->down[0] == k)
    {
        b->down[0] = 0;
    }
    else
    {
        if (b->down[1] != k)
            return;
        b->down[1] = 0;
    }
    if (!b->down[0] && !b->down[1])
    {
        b->active = 0;
        ca = Cmd_Argv(2);
        uptime = atoi(ca);
        if (uptime)
            v1 = b->msec + uptime - b->downtime;
        else
            v1 = b->msec + (InputFrameMilliseconds() >> 1);
        b->msec = v1;
        b->active = 0;
    }
}

void __cdecl IN_DownDown()
{
    IN_KeyDown(&playersKb[0][KEY_DOWN]);
}

void __cdecl IN_DownUp()
{
    IN_KeyUp(&playersKb[0][KEY_DOWN]);
}

void __cdecl IN_LeftDown()
{
    IN_KeyDown(&playersKb[0][KEY_LEFT]);
}

void __cdecl IN_LeftUp()
{
    IN_KeyUp(&playersKb[0][KEY_LEFT]);
}

void __cdecl IN_RightDown()
{
    IN_KeyDown(&playersKb[0][KEY_RIGHT]);
}

void __cdecl IN_RightUp()
{
    IN_KeyUp(&playersKb[0][KEY_RIGHT]);
}

void __cdecl IN_ForwardDown()
{
    IN_KeyDown(&playersKb[0][KEY_FORWARD]);
}

void __cdecl IN_ForwardUp()
{
    IN_KeyUp(&playersKb[0][KEY_FORWARD]);
}

void __cdecl IN_BackDown()
{
    IN_KeyDown(&playersKb[0][KEY_BACK]);
}

void __cdecl IN_BackUp()
{
    IN_KeyUp(&playersKb[0][KEY_BACK]);
}

void __cdecl IN_LookupDown()
{
    IN_KeyDown(&playersKb[0][KEY_LOOKUP]);
}

void __cdecl IN_LookupUp()
{
    IN_KeyUp(&playersKb[0][KEY_LOOKUP]);
}

void __cdecl IN_LookdownDown()
{
    IN_KeyDown(&playersKb[0][KEY_LOOKDOWN]);
}

void __cdecl IN_LookdownUp()
{
    IN_KeyUp(&playersKb[0][KEY_LOOKDOWN]);
}

void __cdecl IN_MoveleftDown()
{
    IN_KeyDown(&playersKb[0][KEY_MOVELEFT]);
}

void __cdecl IN_MoveleftUp()
{
    IN_KeyUp(&playersKb[0][KEY_MOVELEFT]);
}

void __cdecl IN_MoverightDown()
{
    IN_KeyDown(&playersKb[0][KEY_MOVERIGHT]);
}

void __cdecl IN_MoverightUp()
{
    IN_KeyUp(&playersKb[0][KEY_MOVERIGHT]);
}

void __cdecl IN_SpeedDown()
{
    if (clientUIActives[0].connectionState >= CA_CONNECTED)
    {
        CL_GetLocalClientGlobals(0)->usingAds = 0;
        IN_KeyDown(&playersKb[0][KEY_SPEED]);
    }
}

void __cdecl IN_SpeedUp()
{
    if (clientUIActives[0].connectionState >= CA_CONNECTED)
    {
        CL_GetLocalClientGlobals(0)->usingAds = 0;
        IN_KeyUp(&playersKb[0][KEY_SPEED]);
    }
}

void __cdecl IN_StrafeDown()
{
    IN_KeyDown(&playersKb[0][KEY_STRAFE]);
}

void __cdecl IN_StrafeUp()
{
    IN_KeyUp(&playersKb[0][KEY_STRAFE]);
}

void __cdecl IN_Attack_Down()
{
    IN_KeyDown(&playersKb[0][KEY_ATTACK]);
}

void __cdecl IN_Attack_Up()
{
    IN_KeyUp(&playersKb[0][KEY_ATTACK]);
}

void __cdecl IN_Breath_Down()
{
    IN_KeyDown(&playersKb[0][KEY_BREATH]);
}

void __cdecl IN_Breath_Up()
{
    IN_KeyUp(&playersKb[0][KEY_BREATH]);
}

void __cdecl IN_MeleeBreath_Down()
{
    IN_KeyDown(&playersKb[0][KEY_MELEE]);
    IN_KeyDown(&playersKb[0][KEY_BREATH]);
}

void __cdecl IN_MeleeBreath_Up()
{
    IN_KeyUp(&playersKb[0][KEY_MELEE]);
    IN_KeyUp(&playersKb[0][KEY_BREATH]);
}

void __cdecl IN_Frag_Down()
{
    IN_KeyDown(&playersKb[0][KEY_FRAG]);
}

void __cdecl IN_Frag_Up()
{
    IN_KeyUp(&playersKb[0][KEY_FRAG]);
}

void __cdecl IN_Smoke_Down()
{
    IN_KeyDown(&playersKb[0][KEY_SMOKE]);
}

void __cdecl IN_Smoke_Up()
{
    IN_KeyUp(&playersKb[0][KEY_SMOKE]);
}

void __cdecl IN_BreathSprint_Down()
{
    IN_KeyDown(&playersKb[0][KEY_BREATH]);
    IN_KeyDown(&playersKb[0][KEY_SPRINT]);
}

void __cdecl IN_BreathSprint_Up()
{
    IN_KeyUp(&playersKb[0][KEY_BREATH]);
    IN_KeyUp(&playersKb[0][KEY_SPRINT]);
}

void __cdecl IN_Melee_Down()
{
    IN_KeyDown(&playersKb[0][KEY_MELEE]);
}

void __cdecl IN_Melee_Up()
{
    IN_KeyUp(&playersKb[0][KEY_MELEE]);
}

void __cdecl IN_Activate_Down()
{
    IN_KeyDown(&playersKb[0][KEY_ACTIVATE]);
}

void __cdecl IN_Activate_Up()
{
    IN_KeyUp(&playersKb[0][KEY_ACTIVATE]);
}

void __cdecl IN_Reload_Down()
{
    IN_KeyDown(&playersKb[0][KEY_RELOAD]);
}

void __cdecl IN_Reload_Up()
{
    IN_KeyUp(&playersKb[0][KEY_RELOAD]);
}

void __cdecl IN_UseReload_Down()
{
    IN_KeyDown(&playersKb[0][KEY_USERELOAD]);
}

void __cdecl IN_UseReload_Up()
{
    IN_KeyUp(&playersKb[0][KEY_USERELOAD]);
}

void __cdecl IN_LeanLeft_Down()
{
    IN_KeyDown(&playersKb[0][KEY_LEANLEFT]);
}

void __cdecl IN_LeanLeft_Up()
{
    IN_KeyUp(&playersKb[0][KEY_LEANLEFT]);
}

void __cdecl IN_LeanRight_Down()
{
    IN_KeyDown(&playersKb[0][KEY_LEANRIGHT]);
}

void __cdecl IN_LeanRight_Up()
{
    IN_KeyUp(&playersKb[0][KEY_LEANRIGHT]);
}

void __cdecl IN_Prone_Down()
{
    IN_KeyDown(&playersKb[0][KEY_PRONE]);
}

void __cdecl IN_Prone_Up()
{
    IN_KeyUp(&playersKb[0][KEY_PRONE]);
}

void __cdecl IN_Stance_Down()
{
    bool v0; // [esp+0h] [ebp-Ch]
    clientActive_t *LocalClientGlobals; // [esp+4h] [ebp-8h]

    v0 = playersKb[0][KEY_PRONE].active || playersKb[0][KEY_DOWN].active;
    if (!v0 && clientUIActives[0].connectionState >= CA_CONNECTED)
    {
        LocalClientGlobals = CL_GetLocalClientGlobals(0);
        LocalClientGlobals->stanceHeld = 1;
        LocalClientGlobals->stancePosition = LocalClientGlobals->stance;
        LocalClientGlobals->stanceTime = InputEventMilliseconds();
        if (LocalClientGlobals->stancePosition != CL_STANCE_CROUCH)
            LocalClientGlobals->stance = CL_STANCE_CROUCH;
    }
}

void __cdecl IN_Stance_Up()
{
    bool v0; // [esp+0h] [ebp-Ch]
    clientActive_t *LocalClientGlobals; // [esp+4h] [ebp-8h]

    v0 = playersKb[0][KEY_PRONE].active || playersKb[0][KEY_DOWN].active;
    if (!v0 && clientUIActives[0].connectionState >= CA_CONNECTED)
    {
        LocalClientGlobals = CL_GetLocalClientGlobals(0);
        if (LocalClientGlobals->stanceHeld && LocalClientGlobals->stancePosition == CL_STANCE_CROUCH)
            LocalClientGlobals->stance = CL_STANCE_STAND;
        LocalClientGlobals->stanceHeld = 0;
    }
}

void __cdecl IN_ToggleADS()
{
    clientActive_t *LocalClientGlobals; // edx

    if (clientUIActives[0].connectionState >= CA_CONNECTED)
    {
        LocalClientGlobals = CL_GetLocalClientGlobals(0);
        LocalClientGlobals->usingAds = !LocalClientGlobals->usingAds;
    }
}

void __cdecl IN_LeaveADS()
{
    if (clientUIActives[0].connectionState >= CA_CONNECTED)
        CL_GetLocalClientGlobals(0)->usingAds = 0;
}

void __cdecl IN_Throw_Down()
{
    IN_KeyDown(&playersKb[0][KEY_THROW]);
}

void __cdecl IN_Throw_Up()
{
    IN_KeyUp(&playersKb[0][KEY_THROW]);
}

void __cdecl IN_ToggleADS_Throw_Down()
{
    IN_ToggleADS();
    IN_Throw_Down();
}

void __cdecl IN_ToggleADS_Throw_Up()
{
    IN_Throw_Up();
}

void __cdecl IN_Speed_Throw_Down()
{
    IN_SpeedDown();
    IN_Throw_Down();
}

void __cdecl IN_Speed_Throw_Up()
{
    IN_SpeedUp();
    IN_Throw_Up();
}

void __cdecl IN_LowerStance()
{
    StanceState stance; // [esp+0h] [ebp-10h]
    clientActive_t *LocalClientGlobals; // [esp+8h] [ebp-8h]

    if (clientUIActives[0].connectionState >= CA_CONNECTED && !playersKb[0][KEY_PRONE].active && !playersKb[0][KEY_DOWN].active)
    {
        LocalClientGlobals = CL_GetLocalClientGlobals(0);
        stance = LocalClientGlobals->stance;
        if (stance)
        {
            if (stance == CL_STANCE_CROUCH)
            {
                LocalClientGlobals->stance = CL_STANCE_PRONE;
            }
            else if (LocalClientGlobals->stance != CL_STANCE_PRONE)
            {
                MyAssertHandler(".\\client_mp\\cl_input.cpp", 881, 1, "%s", "cl->stance == CL_STANCE_PRONE");
            }
        }
        else
        {
            LocalClientGlobals->stance = CL_STANCE_CROUCH;
        }
    }
}

void __cdecl IN_RaiseStance()
{
    StanceState stance; // [esp+0h] [ebp-10h]
    clientActive_t *LocalClientGlobals; // [esp+8h] [ebp-8h]

    if (clientUIActives[0].connectionState >= CA_CONNECTED && !playersKb[0][KEY_PRONE].active && !playersKb[0][KEY_DOWN].active)
    {
        LocalClientGlobals = CL_GetLocalClientGlobals(0);
        stance = LocalClientGlobals->stance;
        if (stance == CL_STANCE_CROUCH)
        {
            LocalClientGlobals->stance = CL_STANCE_STAND;
        }
        else if (stance == CL_STANCE_PRONE)
        {
            LocalClientGlobals->stance = CL_STANCE_CROUCH;
        }
        else if (LocalClientGlobals->stance)
        {
            MyAssertHandler(".\\client_mp\\cl_input.cpp", 910, 1, "%s", "cl->stance == CL_STANCE_STAND");
        }
    }
}

void __cdecl IN_ToggleCrouch()
{
    CL_ToggleStance(0, CL_STANCE_CROUCH);
}

void __cdecl CL_ToggleStance(int localClientNum, StanceState preferredStance)
{
    clientActive_t *LocalClientGlobals; // edx

    if (!playersKb[localClientNum][KEY_PRONE].active && !playersKb[localClientNum][KEY_DOWN].active)
    {
        LocalClientGlobals = CL_GetLocalClientGlobals(localClientNum);
        LocalClientGlobals->stance = preferredStance != LocalClientGlobals->stance ? preferredStance : CL_STANCE_STAND;
    }
}

void __cdecl IN_ToggleProne()
{
    CL_ToggleStance(0, CL_STANCE_PRONE);
}

void __cdecl IN_GoProne()
{
    CL_SetStance(0, CL_STANCE_PRONE);
}

void __cdecl IN_GoCrouch()
{
    CL_SetStance(0, CL_STANCE_CROUCH);
}

void __cdecl IN_GoStandDown()
{
    if (clientUIActives[0].connectionState >= CA_CONNECTED)
    {
        IN_KeyDown(&playersKb[0][KEY_UP]);
        if (CL_GetLocalClientGlobals(0)->stance)
            CL_SetStance(0, CL_STANCE_STAND);
        else
            IN_KeyDown(&playersKb[0][KEY_MOVEUP]);
    }
}

void __cdecl IN_GoStandUp()
{
    if (clientUIActives[0].connectionState >= CA_CONNECTED)
    {
        IN_KeyUp(&playersKb[0][KEY_UP]);
        IN_KeyUp(&playersKb[0][KEY_MOVEUP]);
    }
}

void __cdecl IN_SprintDown()
{
    IN_KeyDown(&playersKb[0][KEY_SPRINT]);
}

void __cdecl IN_SprintUp()
{
    IN_KeyUp(&playersKb[0][KEY_SPRINT]);
}

void __cdecl CL_ShutdownInput()
{
    Cmd_RemoveCommand("mouseMove");
    Cmd_RemoveCommand("remoteKey");
    Cmd_RemoveCommand("centerview");
    Cmd_RemoveCommand("+moveup");
    Cmd_RemoveCommand("-moveup");
    Cmd_RemoveCommand("+movedown");
    Cmd_RemoveCommand("-movedown");
    Cmd_RemoveCommand("+left");
    Cmd_RemoveCommand("-left");
    Cmd_RemoveCommand("+right");
    Cmd_RemoveCommand("-right");
    Cmd_RemoveCommand("+forward");
    Cmd_RemoveCommand("-forward");
    Cmd_RemoveCommand("+back");
    Cmd_RemoveCommand("-back");
    Cmd_RemoveCommand("+lookup");
    Cmd_RemoveCommand("-lookup");
    Cmd_RemoveCommand("+lookdown");
    Cmd_RemoveCommand("-lookdown");
    Cmd_RemoveCommand("+strafe");
    Cmd_RemoveCommand("-strafe");
    Cmd_RemoveCommand("+moveleft");
    Cmd_RemoveCommand("-moveleft");
    Cmd_RemoveCommand("+moveright");
    Cmd_RemoveCommand("-moveright");
    Cmd_RemoveCommand("+speed");
    Cmd_RemoveCommand("-speed");
    Cmd_RemoveCommand("+attack");
    Cmd_RemoveCommand("-attack");
    Cmd_RemoveCommand("+melee");
    Cmd_RemoveCommand("-melee");
    Cmd_RemoveCommand("+holdbreath");
    Cmd_RemoveCommand("-holdbreath");
    Cmd_RemoveCommand("+melee_breath");
    Cmd_RemoveCommand("-melee_breath");
    Cmd_RemoveCommand("+frag");
    Cmd_RemoveCommand("-frag");
    Cmd_RemoveCommand("+smoke");
    Cmd_RemoveCommand("-smoke");
    Cmd_RemoveCommand("+breath_sprint");
    Cmd_RemoveCommand("-breath_sprint");
    Cmd_RemoveCommand("+activate");
    Cmd_RemoveCommand("-activate");
    Cmd_RemoveCommand("+reload");
    Cmd_RemoveCommand("-reload");
    Cmd_RemoveCommand("+usereload");
    Cmd_RemoveCommand("-usereload");
    Cmd_RemoveCommand("+leanleft");
    Cmd_RemoveCommand("-leanleft");
    Cmd_RemoveCommand("+leanright");
    Cmd_RemoveCommand("-leanright");
    Cmd_RemoveCommand("+prone");
    Cmd_RemoveCommand("-prone");
    Cmd_RemoveCommand("+stance");
    Cmd_RemoveCommand("-stance");
    Cmd_RemoveCommand("+mlook");
    Cmd_RemoveCommand("-mlook");
    Cmd_RemoveCommand("toggleads");
    Cmd_RemoveCommand("leaveads");
    Cmd_RemoveCommand("+throw");
    Cmd_RemoveCommand("-throw");
    Cmd_RemoveCommand("+speed_throw");
    Cmd_RemoveCommand("-speed_throw");
    Cmd_RemoveCommand("+toggleads_throw");
    Cmd_RemoveCommand("-toggleads_throw");
    Cmd_RemoveCommand("lowerstance");
    Cmd_RemoveCommand("raisestance");
    Cmd_RemoveCommand("togglecrouch");
    Cmd_RemoveCommand("toggleprone");
    Cmd_RemoveCommand("goprone");
    Cmd_RemoveCommand("gocrouch");
    Cmd_RemoveCommand("+gostand");
    Cmd_RemoveCommand("-gostand");
    Cmd_RemoveCommand("+sprint");
    Cmd_RemoveCommand("-sprint");
    Cmd_RemoveCommand("+talk");
    Cmd_RemoveCommand("-talk");
}

void __cdecl CL_ClearKeys(int localClientNum)
{
    memset((uint8_t *)playersKb[localClientNum], 0, sizeof(kbutton_t[30]));
}


#ifdef COD4X1KHZ
namespace {
struct TypedBinding { const char* name; void (*down)(); void (*up)(); };
// All of these handlers mutate only owner CL/kbutton state. Weapon cycling uses
// the native gameplay helper without invoking its main-owned menu notification.
void OwnerNextWeapon() {
    cg_s* game = CG_GetLocalClientGlobals(0);
    if (game->nextSnap && WeaponCycleAllowed(game)) {
        game->weaponSelectTime = game->time;
        KHZ_ClientOwnerQueueMenuNotify(1);
        CycleWeapPrimary(0, 1, 0);
    }
}
void OwnerPreviousWeapon() {
    cg_s* game = CG_GetLocalClientGlobals(0);
    if (game->nextSnap && WeaponCycleAllowed(game)) {
        game->weaponSelectTime = game->time;
        CycleWeapPrimary(0, 0, 0);
    }
}
const TypedBinding typedBindings[] = {
    {"centerview", IN_CenterView, nullptr},
    {"+moveup", IN_UpDown, IN_UpUp},
    {"+movedown", IN_DownDown, IN_DownUp},
    {"+left", IN_LeftDown, IN_LeftUp},
    {"+right", IN_RightDown, IN_RightUp},
    {"+forward", IN_ForwardDown, IN_ForwardUp},
    {"+back", IN_BackDown, IN_BackUp},
    {"+lookup", IN_LookupDown, IN_LookupUp},
    {"+lookdown", IN_LookdownDown, IN_LookdownUp},
    {"+strafe", IN_StrafeDown, IN_StrafeUp},
    {"+moveleft", IN_MoveleftDown, IN_MoveleftUp},
    {"+moveright", IN_MoverightDown, IN_MoverightUp},
    {"+speed", IN_SpeedDown, IN_SpeedUp},
    {"+attack", IN_Attack_Down, IN_Attack_Up},
    {"+melee", IN_Melee_Down, IN_Melee_Up},
    {"+holdbreath", IN_Breath_Down, IN_Breath_Up},
    {"+melee_breath", IN_MeleeBreath_Down, IN_MeleeBreath_Up},
    {"+frag", IN_Frag_Down, IN_Frag_Up},
    {"+smoke", IN_Smoke_Down, IN_Smoke_Up},
    {"+breath_sprint", IN_BreathSprint_Down, IN_BreathSprint_Up},
    {"+activate", IN_Activate_Down, IN_Activate_Up},
    {"+reload", IN_Reload_Down, IN_Reload_Up},
    {"+usereload", IN_UseReload_Down, IN_UseReload_Up},
    {"+leanleft", IN_LeanLeft_Down, IN_LeanLeft_Up},
    {"+leanright", IN_LeanRight_Down, IN_LeanRight_Up},
    {"+prone", IN_Prone_Down, IN_Prone_Up},
    {"+stance", IN_Stance_Down, IN_Stance_Up},
    {"+mlook", IN_MLookDown, IN_MLookUp},
    {"toggleads", IN_ToggleADS, nullptr},
    {"leaveads", IN_LeaveADS, nullptr},
    {"+throw", IN_Throw_Down, IN_Throw_Up},
    {"+speed_throw", IN_Speed_Throw_Down, IN_Speed_Throw_Up},
    {"+toggleads_throw", IN_ToggleADS_Throw_Down, IN_ToggleADS_Throw_Up},
    {"lowerstance", IN_LowerStance, nullptr},
    {"raisestance", IN_RaiseStance, nullptr},
    {"togglecrouch", IN_ToggleCrouch, nullptr},
    {"toggleprone", IN_ToggleProne, nullptr},
    {"goprone", IN_GoProne, nullptr},
    {"gocrouch", IN_GoCrouch, nullptr},
    {"+gostand", IN_GoStandDown, IN_GoStandUp},
    {"+talk", IN_TalkDown, IN_TalkUp},
    {"+sprint", IN_SprintDown, IN_SprintUp},
    {"weapnext", OwnerNextWeapon, nullptr},
    {"weapprev", OwnerPreviousWeapon, nullptr},
};
struct PlannedBinding { const TypedBinding* handler = nullptr; int actionSlot = 0; };
struct BindingPlan { PlannedBinding handlers[16]{}; unsigned count = 0; };
bool BuildBindingPlan(const char* text, BindingPlan* plan) {
    *plan = {};
    while (*text) {
        while (*text == ' ' || *text == '\t' || *text == ';') ++text;
        if (!*text) break;
        const char* begin = text;
        while (*text && *text != ';') ++text;
        const char* end = text;
        while (end != begin && (end[-1] == ' ' || end[-1] == '\t')) --end;
        const TypedBinding* found = nullptr;
        for (const auto& binding : typedBindings) {
            const auto length = std::strlen(binding.name);
            if (length == unsigned(end - begin) && !_strnicmp(begin, binding.name, length)) { found = &binding; break; }
        }
        int actionSlot = 0;
        if (!found && unsigned(end - begin) >= 12 && !_strnicmp(begin, "+actionslot", 11)) {
            const char* argument = begin + 11;
            if (*argument != ' ' && *argument != '\t') return false;
            while (argument < end && (*argument == ' ' || *argument == '\t')) ++argument;
            if (argument + 1 != end || *argument < '1' || *argument > '4') return false;
            actionSlot = *argument - '0';
        }
        if ((!found && !actionSlot) || plan->count == 16) return false;
        plan->handlers[plan->count++] = { found, actionSlot };
    }
    return true;
}
void ReleaseOwnerInput() {
    std::memset(ownerPlayersKb, 0, sizeof(ownerPlayersKb));
    ownerKeyDown.fill(false);
    ownerLocSelInputState = LOC_SEL_INPUT_NONE;
    auto* client = CL_GetLocalClientGlobals(0);
    client->mouseDx[0] = client->mouseDx[1] = client->mouseDy[0] = client->mouseDy[1] = 0;
    client->stanceHeld = 0;
    client->usingAds = false;
}
void RequestMainKey(unsigned key, bool down, unsigned time) {
    if (ownerMainPending) return;
    const bool navigation = key == K_ESCAPE || frozenBindings[key].consoleKey
        || (KHZ_ClientOwnerKeyCatchers() & ~KEYCATCH_LOCATION_SELECTION);
    const bool heldBinding = frozenBindings[key].text[0] == '+';
    if (!down && (navigation || !heldBinding)) return;
    // Independent menu/console controls bypass delayed legacy suppression.
    // Compound/custom gameplay commands execute once under main's acknowledged
    // owner control barrier, including a real key-up for held '+' bindings.
    if (frozenBindings[key].consoleKey)
        for (unsigned modifier : { unsigned(K_SHIFT), unsigned(K_CTRL), unsigned(K_ALT) })
            if (ownerKeyDown[modifier]) KHZ_ClientOwnerRequestMainKey(modifier, 1, time);
    KHZ_ClientOwnerRequestMainKey(key, down ? 1 : 0, time);
    if (down && (navigation || !heldBinding)) KHZ_ClientOwnerRequestMainKey(key, 0, time);
    if (frozenBindings[key].consoleKey)
        for (unsigned modifier : { unsigned(K_SHIFT), unsigned(K_CTRL), unsigned(K_ALT) })
            if (ownerKeyDown[modifier]) KHZ_ClientOwnerRequestMainKey(modifier, 0, time);
    if (navigation) ReleaseOwnerInput();
    ownerMainPending = true;
    KHZ_ClientOwnerRequestPark("Input requires main-thread menu/console or custom binding");
}
void ApplyOwnerKey(unsigned key, bool down, unsigned time) {
    if (!key || key >= frozenBindings.size() || ownerMainPending) return;
    if (ownerAwaitRelease[key]) { if (!down) ownerAwaitRelease[key] = false; return; }
    // Key repeats never replay weapon selection, toggles or menu opens.
    if (ownerKeyDown[key] == down) return;
    ownerKeyDown[key] = down;
    const int catchers = KHZ_ClientOwnerKeyCatchers();
    if (catchers & KEYCATCH_LOCATION_SELECTION) {
        if (down && key == K_ESCAPE) ownerLocSelInputState = LOC_SEL_INPUT_CANCEL;
        else if (down && !_stricmp(frozenBindings[key].text, "+attack")) ownerLocSelInputState = LOC_SEL_INPUT_CONFIRM;
        return;
    }
    if (key == K_ESCAPE || frozenBindings[key].consoleKey || (catchers & ~KEYCATCH_LOCATION_SELECTION)) {
        RequestMainKey(key, down, time);
        return;
    }
    BindingPlan plan;
    if (!BuildBindingPlan(frozenBindings[key].text, &plan)) { RequestMainKey(key, down, time); return; }
    ownerTypedInput = true; ownerTypedKey = int(key); ownerTypedMilliseconds = time;
    for (unsigned i = 0; i < plan.count; ++i) {
        const auto& binding = plan.handlers[i];
        if (binding.actionSlot) {
            if (down) KHZ_ActionSlotDown(binding.actionSlot);
            else KHZ_ActionSlotUp(binding.actionSlot);
        } else if (down) binding.handler->down();
        else if (binding.handler->up) binding.handler->up();
    }
    ownerTypedInput = false;
}
int AddMouseDelta(int current, int delta) {
    const long long sum = static_cast<long long>(current) + delta;
    return sum > (std::numeric_limits<int>::max)() ? (std::numeric_limits<int>::max)()
        : sum < (std::numeric_limits<int>::min)() ? (std::numeric_limits<int>::min)() : int(sum);
}
void ApplyOwnedEvent(const KHZ_OwnedInputEvent& event,unsigned milliseconds) {
    if(event.kind==KHZ_OwnedInputKind::Reset){ReleaseOwnerInput();return;}
    if(event.kind==KHZ_OwnedInputKind::Key){
        unsigned key=0;
        if(event.scanCode==41)key='~';
        else if(event.virtualKey<256)key=frozenKeyMap[event.virtualKey][(event.keyboardFlags&RI_KEY_E0)?1:0];
        ApplyOwnerKey(key,event.keyDown,milliseconds);
    }else if(event.kind==KHZ_OwnedInputKind::Mouse){
        auto* client=CL_GetLocalClientGlobals(0);
        client->mouseDx[client->mouseIndex]=AddMouseDelta(client->mouseDx[client->mouseIndex],event.dx);
        client->mouseDy[client->mouseIndex]=AddMouseDelta(client->mouseDy[client->mouseIndex],event.dy);
        for(unsigned button=0;button<5;++button){
            if(event.mouseButtonFlags&(1u<<(button*2)))ApplyOwnerKey(K_MOUSE1+button,true,milliseconds);
            if(event.mouseButtonFlags&(2u<<(button*2)))ApplyOwnerKey(K_MOUSE1+button,false,milliseconds);
        }
        if((event.mouseButtonFlags&RI_MOUSE_WHEEL)&&event.wheelDelta){
            const unsigned key=event.wheelDelta>0?K_MWHEELUP:K_MWHEELDOWN;
            const unsigned steps=(unsigned(abs(event.wheelDelta))+WHEEL_DELTA-1)/WHEEL_DELTA;
            for(unsigned step=0;step<steps&&step<64;++step){ApplyOwnerKey(key,true,milliseconds);ApplyOwnerKey(key,false,milliseconds);}
        }
    }
}
bool InputReleased() {
    for(const auto& button:ownerPlayersKb[0])if(button.down[0]||button.down[1]||button.active||button.wasPressed||button.msec)return false;
    for(bool held:ownerKeyDown)if(held)return false;
    for(bool waiting:ownerAwaitRelease)if(waiting)return false;
    return ownerLocSelInputState==LOC_SEL_INPUT_NONE;
}
bool InputCheckpointContext() {
    return KHZ_IsClientSimulationThread()&&KHZ_ClientOwnerCheckpointContext()&&ownerInputInitialized
        &&!ownerTypedInput&&!ownerMainPending;
}
bool ReplayKey(unsigned key) {
    if(!key)return true;
    if(key>=frozenBindings.size()||key==K_ESCAPE||frozenBindings[key].consoleKey)return false;
    BindingPlan plan;return BuildBindingPlan(frozenBindings[key].text,&plan);
}
bool ValidReplayReceipt(const KHZ_ClientInputReceipt& receipt) {
    if(!khz::input_replay::ValidFields(receipt))return false;
    const auto& event=receipt.event;
    if(event.kind==KHZ_OwnedInputKind::Reset)return unsigned(event.resetReason)<=unsigned(KHZ_InputResetReason::Stop);
    if(event.kind==KHZ_OwnedInputKind::Key){
        const unsigned key=event.scanCode==41?'~':frozenKeyMap[event.virtualKey][(event.keyboardFlags&RI_KEY_E0)?1:0];
        return ReplayKey(key);
    }
    for(unsigned button=0;button<5;++button)if((event.mouseButtonFlags&(3u<<(button*2)))&&!ReplayKey(K_MOUSE1+button))return false;
    return !(event.mouseButtonFlags&RI_MOUSE_WHEEL)||!event.wheelDelta||ReplayKey(event.wheelDelta>0?K_MWHEELUP:K_MWHEELDOWN);
}
void CanonicalReceipt(khz::canonical::Hash& hash,const KHZ_ClientInputReceipt& receipt) {
    khz::input_replay::ReceiptIdentity(hash,receipt);
}
void PollInputReplay(std::uint64_t deadline) {
    if(!KHZ_InputOwnerCheckpointParked()){replayFailed=true;KHZ_ClientOwnerRequestPark("Replay producer park/drain fence changed");return;}
    while(replayCursor<replayCount){
        const auto& receipt=replayReceipts[replayCursor];std::uint64_t qpc;
        if(!khz::checkpoint::receiptDeadline(replayQpcEpoch,replayFrequency,receipt.tickOffset,receipt.microseconds,&qpc)){
            replayFailed=true;KHZ_ClientOwnerRequestPark("Invalid replay receipt deadline");return;
        }
        if(qpc>deadline)return;
        ownerLastInputSequence=replaySequenceBase+replayCursor+1;ownerLastInputQpc=qpc;
        ApplyOwnedEvent(receipt.event,replayNativeMilliseconds+static_cast<unsigned>(receipt.tickOffset));++replayCursor;
        if(ownerMainPending){replayFailed=true;return;}
    }
}
}

bool KHZ_ClientInputFreezeBindings() {
    if (KHZ_IsClientSimulationThread()) return false;
    for (unsigned key = 0; key < frozenBindings.size(); ++key) {
        const char* binding = Key_GetBinding(0, key);
        if (!binding) binding = "";
        const auto length = std::strlen(binding);
        if (length >= sizeof(frozenBindings[key].text)) return false;
        std::memcpy(frozenBindings[key].text, binding, length + 1);
        frozenBindings[key].consoleKey = CL_IsConsoleKey(key);
    }
    KHZ_ExportGameplayKeyMap(frozenKeyMap);
    std::uint64_t hash = 1469598103934665603ull;
    for (const auto& binding : frozenBindings) {
        for (const unsigned char* c = reinterpret_cast<const unsigned char*>(binding.text); *c; ++c) { hash ^= *c; hash *= 1099511628211ull; }
        hash ^= 0; hash *= 1099511628211ull;
        hash ^= binding.consoleKey ? 1 : 0; hash *= 1099511628211ull;
    }
    for (const auto& pair : frozenKeyMap) for (unsigned char value : pair) { hash ^= value; hash *= 1099511628211ull; }
    frozenBindingCanonical = khz::input_canonical::BindingsIdentity(frozenBindings, frozenKeyMap);
    if (!frozenBindingCanonical.valid) return false;
    frozenBindingRevision.store(hash, std::memory_order_release);
    return true;
}
void KHZ_ClientInputOwnerInit(std::uint64_t epoch) {
    KHZ_InputOwnerDiscardPending();
    ownerInputEpoch = epoch;
    ownerBindingRevision = frozenBindingRevision.load(std::memory_order_acquire);
    ownerBindingCanonical = frozenBindingCanonical;
    ownerLastInputSequence = ownerLastInputQpc = 0;
    ownerMainPending = ownerTypedInput = false;
    ownerInputInitialized = true;
    ownerAwaitRelease.fill(false);
    ReleaseOwnerInput();
}
void KHZ_ClientInputOwnerResume(std::uint64_t epoch) {
    if(replayActive.load(std::memory_order_acquire))return; // explicit replay resume preserves restored TLS
    const bool bindingsChanged = ownerBindingRevision != frozenBindingRevision.load(std::memory_order_acquire);
    if (!ownerInputInitialized || epoch != ownerInputEpoch || bindingsChanged
        || !KHZ_InputOwnerOwnsGameplay()) {
        const auto heldBeforeChange = ownerKeyDown;
        KHZ_ClientInputOwnerInit(epoch);
        if (bindingsChanged) ownerAwaitRelease = heldBeforeChange;
        return;
    }
    // Ordinary console/dvar/control barriers preserve real held input and all
    // native owner-state command effects. Raw events accumulated during the
    // barrier retain their deadlines and will be consumed in timestamp order.
    ownerTypedInput = ownerMainPending = false;
}
void KHZ_ClientInputOwnerPoll(std::uint64_t deadline) {
    if (!ownerInputInitialized || ownerMainPending) return;
    // Location-selection is gameplay state; the owner never asks mutable UI for
    // the current menu nor writes playerKeys from this thread.
    KHZ_ClientOwnerSetLocationSelection(CG_GetLocalClientGlobals(0)->predictedPlayerState.locationSelectionInfo != 0);
    if(replayActive.load(std::memory_order_acquire)){PollInputReplay(deadline);return;}
    KHZ_OwnedInputEvent event;
    for (unsigned count = 0; count < 4096 && KHZ_InputOwnerPopDue(&event, deadline); ++count) {
        if (event.controlEpoch != ownerInputEpoch || event.sequence <= ownerLastInputSequence || event.qpc < ownerLastInputQpc) continue;
        ownerLastInputSequence = event.sequence; ownerLastInputQpc = event.qpc;
        const unsigned milliseconds = KHZ_ClientOwnerMillisecondsAt(event.qpc);
        ApplyOwnedEvent(event,milliseconds);
        if (ownerMainPending) break;
    }
}
void KHZ_ClientInputOwnerStop() {
    replayActive=false;replayFailed=false;replayCount=replayCursor=0;
    ReleaseOwnerInput();
    ownerInputInitialized = ownerTypedInput = ownerMainPending = false;
}
bool KHZ_ClientInputCanonical(khz::canonical::Hash& hash) {
    if (!KHZ_IsClientSimulationThread() || !ownerInputInitialized || ownerTypedInput || ownerMainPending
        || !ownerBindingCanonical.valid
        || ownerBindingRevision != frozenBindingRevision.load(std::memory_order_acquire)) {
        hash.reject(); return false;
    }
    hash.text("native MP private held input v1");
    khz::input_canonical::Buttons(hash, ownerPlayersKb[0]);
    hash.scalar(ownerLocSelInputState);
    hash.u32(static_cast<std::uint32_t>(ownerKeyDown.size()));
    for (bool held : ownerKeyDown) hash.boolean(held);
    hash.u32(static_cast<std::uint32_t>(ownerAwaitRelease.size()));
    for (bool waiting : ownerAwaitRelease) hash.boolean(waiting);
    hash.digest(ownerBindingCanonical);
    if(replayFailed.load()){hash.reject();return false;}
    hash.boolean(replayActive.load(std::memory_order_acquire));
    if(replayActive.load()){hash.scalar(replayCount);hash.scalar(replayCursor);hash.digest(replayIdentity);}
    // Input sequence, control epoch and QPC are lifecycle/timeline guards.
    // Typed key/time are inactive scratch here; CL mouse/ADS/stance state is
    // serialized separately by KHZ_CanonicalClientInput from the private CL.
    return hash.finish().valid;
}
bool KHZ_ClientInputCheckpointPrepareReleased() {
    if(!InputCheckpointContext()||!KHZ_InputOwnerParkAcknowledged()||replayActive.load())return false;
    // The real sole consumer drains pending hardware receipts. This explicit
    // fixture baseline discards their unscheduled actions, processes actual
    // reset records, then releases all private input before any world capture.
    KHZ_OwnedInputEvent event;
    for(unsigned count=0;count<4096&&KHZ_InputOwnerPop(&event);++count){
        if(event.kind==KHZ_OwnedInputKind::Reset)ReleaseOwnerInput();
        if(event.controlEpoch==ownerInputEpoch&&event.sequence>ownerLastInputSequence){
            ownerLastInputSequence=event.sequence;ownerLastInputQpc=event.qpc;
        }
    }
    if(!KHZ_InputOwnerCheckpointParked())return false;
    ReleaseOwnerInput();ownerAwaitRelease.fill(false);ownerTypedKey=0;ownerTypedMilliseconds=0;
    replayFailed=false;return InputReleased()&&KHZ_InputOwnerCheckpointParked();
}
bool KHZ_ClientInputCheckpointCapture(KHZ_ClientInputCheckpoint* output) {
    if(!output||!InputCheckpointContext()||!KHZ_InputOwnerCheckpointParked()||replayActive.load()
        ||!InputReleased()||!ownerBindingCanonical.valid
        ||ownerBindingRevision!=frozenBindingRevision.load()
        ||!khz::canonical::equal(ownerBindingCanonical,frozenBindingCanonical))return false;
    output->schema=1;
    for(unsigned index=0;index<30;++index){
        const auto& input=ownerPlayersKb[0][index];auto& saved=output->buttons[index];
        saved.down[0]=input.down[0];saved.down[1]=input.down[1];saved.downtime=input.downtime;saved.msec=input.msec;
        saved.active=input.active;saved.wasPressed=input.wasPressed;
    }
    output->keyDown=ownerKeyDown;output->awaitRelease=ownerAwaitRelease;output->locationSelection=ownerLocSelInputState;
    output->initialized=ownerInputInitialized;output->typedInput=ownerTypedInput;output->mainPending=ownerMainPending;
    output->allReleased=InputReleased();output->controlEpoch=ownerInputEpoch;output->lastSequence=ownerLastInputSequence;
    output->lastQpc=ownerLastInputQpc;output->bindingRevision=ownerBindingRevision;output->bindingIdentity=ownerBindingCanonical;
    return KHZ_InputOwnerCheckpointParked();
}
bool KHZ_ClientInputCheckpointValidateRestore(const KHZ_ClientInputCheckpoint& checkpoint) {
    if(!InputCheckpointContext()||!KHZ_InputOwnerCheckpointParked()||!khz::input_replay::CheckpointShape(checkpoint)
        ||checkpoint.controlEpoch!=ownerInputEpoch
        ||checkpoint.bindingRevision!=frozenBindingRevision.load()
        ||!khz::canonical::equal(checkpoint.bindingIdentity,frozenBindingCanonical)
        ||!khz::canonical::equal(checkpoint.bindingIdentity,ownerBindingCanonical))return false;
    return true;
}
bool KHZ_ClientInputCheckpointRestore(const KHZ_ClientInputCheckpoint& checkpoint,std::uint64_t qpcEpoch) {
    if(!qpcEpoch||!KHZ_ClientInputCheckpointValidateRestore(checkpoint))return false;
    replayActive=false;replayFailed=false;replayCount=replayCursor=0;
    for(unsigned index=0;index<30;++index){
        auto& input=ownerPlayersKb[0][index];const auto& saved=checkpoint.buttons[index];
        input.down[0]=saved.down[0];input.down[1]=saved.down[1];input.downtime=saved.downtime;input.msec=saved.msec;
        input.active=saved.active;input.wasPressed=saved.wasPressed;
    }
    ownerKeyDown=checkpoint.keyDown;ownerAwaitRelease=checkpoint.awaitRelease;
    ownerLocSelInputState=static_cast<LocSelInputState>(checkpoint.locationSelection);
    ownerInputInitialized=checkpoint.initialized;ownerTypedInput=checkpoint.typedInput;ownerMainPending=checkpoint.mainPending;
    ownerInputEpoch=checkpoint.controlEpoch;ownerLastInputSequence=checkpoint.lastSequence;ownerLastInputQpc=qpcEpoch;
    ownerBindingRevision=checkpoint.bindingRevision;ownerBindingCanonical=checkpoint.bindingIdentity;
    ownerTypedKey=0;ownerTypedMilliseconds=0;return true;
}
void KHZ_ClientInputCheckpointDiscard() {
    if(!KHZ_IsClientSimulationThread()||!KHZ_ClientOwnerCheckpointContext())return;
    replayActive=false;replayFailed=false;replayCount=replayCursor=0;savedInputValid=false;
}
bool KHZ_ClientInputReplayBegin(const KHZ_ClientInputReceipt* receipts,unsigned count,std::uint64_t qpcEpoch,
    std::uint64_t frequency,unsigned millisecondsEpoch) {
    if(!InputCheckpointContext()||!KHZ_InputOwnerCheckpointParked()||!InputReleased()||replayActive.load()
        ||!savedInputValid||!KHZ_ClientCheckpointPinned()||!qpcEpoch||frequency!=savedInputFrame.qpcFrequency
        ||millisecondsEpoch!=savedInputFrame.nativeInputMilliseconds||count>KHZ_MaxInputReplayReceipts
        ||(count&&!receipts)||ownerLastInputSequence>UINT64_MAX-count)return false;
    std::uint64_t previous=0;khz::canonical::Hash identity;identity.text("normalized native input receipts v1");identity.scalar(count);
    for(unsigned index=0;index<count;++index){
        std::uint64_t deadline;
        if(!ValidReplayReceipt(receipts[index])||!khz::checkpoint::receiptDeadline(qpcEpoch,frequency,
            receipts[index].tickOffset,receipts[index].microseconds,&deadline)||(index&&deadline<previous))return false;
        previous=deadline;CanonicalReceipt(identity,receipts[index]);
    }
    for(unsigned index=0;index<count;++index)replayReceipts[index]=receipts[index];
    replayCount=count;replayCursor=0;replayNativeMilliseconds=millisecondsEpoch;replayQpcEpoch=qpcEpoch;
    replayFrequency=frequency;replaySequenceBase=ownerLastInputSequence;replayIdentity=identity.finish();
    replayFailed=false;replayActive.store(true,std::memory_order_release);return true;
}
void KHZ_ClientInputReplayEnd() {
    if(!KHZ_IsClientSimulationThread()||!KHZ_ClientOwnerCheckpointContext())return;
    if(!replayActive.load(std::memory_order_acquire))return;
    if(!khz::input_replay::EndReceiptGuards(true,KHZ_InputOwnerCheckpointParked(),ownerLastInputSequence,ownerLastInputQpc)){
        replayFailed=true;KHZ_ClientOwnerRequestPark("Replay end requires acknowledged drained hardware producer");return;
    }
    replayActive.store(false,std::memory_order_release);replayCount=replayCursor=0;
}
bool KHZ_ClientInputReplayRebaseEpoch(std::uint64_t epoch) {
    if(!KHZ_IsClientSimulationThread()||!replayActive.load()||replayCursor||!epoch
        ||!KHZ_InputOwnerCheckpointParked())return false;
    if(replayCount){
        const auto& last=replayReceipts[replayCount-1];std::uint64_t deadline;
        if(!khz::checkpoint::receiptDeadline(epoch,replayFrequency,last.tickOffset,last.microseconds,&deadline))return false;
    }
    replayQpcEpoch=epoch;ownerLastInputQpc=epoch;return true;
}
bool KHZ_ClientInputReplayActive(){return replayActive.load(std::memory_order_acquire);}
bool KHZ_ClientInputReplayFailed(){return replayFailed.load(std::memory_order_acquire);}
namespace {
bool InputProviderReject(char* reason,unsigned capacity,const char* message){if(reason&&capacity)std::snprintf(reason,capacity,"%s",message);return false;}
bool InputProviderCapture(const KHZ_ClientCheckpointFrame& frame,char* reason,unsigned capacity){
    savedInputValid=false;
    if(!khz::checkpoint::valid(frame)||frame.generation!=ownerInputEpoch||!KHZ_ClientInputCheckpointCapture(&savedInputCheckpoint))
        return InputProviderReject(reason,capacity,"input_requires_acknowledged_drained_released_frozen_baseline");
    savedInputFrame=frame;savedInputValid=true;return true;
}
bool InputProviderValidate(const KHZ_ClientCheckpointFrame& frame,char* reason,unsigned capacity){
    if(!savedInputValid||!khz::checkpoint::sameLifetime(savedInputFrame,frame)
        ||savedInputFrame.logicalTick!=frame.logicalTick||savedInputFrame.nativeInputMilliseconds!=frame.nativeInputMilliseconds
        ||!KHZ_ClientInputCheckpointValidateRestore(savedInputCheckpoint))
        return InputProviderReject(reason,capacity,"input_checkpoint_lifetime_binding_or_producer_fence_changed");
    return true;
}
bool InputProviderRestore(const KHZ_ClientCheckpointFrame& frame,std::uint64_t epoch,char* reason,unsigned capacity){
    return InputProviderValidate(frame,reason,capacity)&&KHZ_ClientInputCheckpointRestore(savedInputCheckpoint,epoch);
}
}
void KHZ_ClientInputRegisterCheckpoint() {
    const KHZ_ClientCheckpointProvider provider{KHZ_ClientCheckpointDomain::Input,
        sizeof(savedInputCheckpoint)+sizeof(savedInputFrame)+sizeof(savedInputValid),
        InputProviderCapture,InputProviderValidate,InputProviderRestore,KHZ_ClientInputCheckpointDiscard};
    KHZ_ClientCheckpointRegister(provider);
}
bool KHZ_ClientInputSelfTest() {
    if (!GameplayInputAllowedState(false,0) || !GameplayInputAllowedState(false,KEYCATCH_LOCATION_SELECTION)) return false;
    if (GameplayInputAllowedState(true,0) || GameplayInputAllowedState(false,KEYCATCH_MESSAGE)
        || GameplayInputAllowedState(false,KEYCATCH_CONSOLE) || GameplayInputAllowedState(false,KEYCATCH_UI)
        || GameplayInputAllowedState(false,KEYCATCH_SCRIPT|KEYCATCH_LOCATION_SELECTION)) return false;
    BindingPlan plan;
    if (!BuildBindingPlan("+forward;+speed", &plan) || plan.count != 2) return false;
    if (!BuildBindingPlan(" weapnext ; toggleads ", &plan) || plan.count != 2) return false;
    if (!BuildBindingPlan("+actionslot 4", &plan) || plan.count != 1 || plan.handlers[0].actionSlot != 4) return false;
    if (BuildBindingPlan("+actionslot 0", &plan) || BuildBindingPlan("+actionslot 5", &plan)
        || BuildBindingPlan("+actionslot 1 extra", &plan)) return false;
    if (BuildBindingPlan("+forward;vstr custom", &plan) || BuildBindingPlan("+attack 42", &plan)) return false;
    if (AddMouseDelta((std::numeric_limits<int>::max)(), 1) != (std::numeric_limits<int>::max)()) return false;
    if (AddMouseDelta((std::numeric_limits<int>::min)(), -1) != (std::numeric_limits<int>::min)()) return false;
    return true;
}
#endif
