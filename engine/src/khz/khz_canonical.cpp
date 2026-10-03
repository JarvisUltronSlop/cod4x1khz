#include "khz_canonical.h"
#include <universal/q_shared.h>
#include <bgame/bg_local.h>
#include <client_mp/client_mp.h>
#include <cgame_mp/cg_local_mp.h>
#include <qcommon/msg_mp.h>
#include <qcommon/sv_msg_write_mp.h>
#include <aim_assist/aim_assist.h>
#include <EffectsCore/fx_system.h>
#include <khz/khz_client_owner.h>
#include <khz/khz_runtime.h>
#include <universal/com_math.h>
#include <qcommon/threads.h>
#include <cstdio>
#include <memory>
#include <cstring>

using khz::canonical::Hash;
#define I(field) hash.scalar(value.field)
#define A(field) hash.array(value.field)

namespace {
void hud(Hash& hash, const hudelem_s& value) {
    I(type); I(x); I(y); I(z); I(targetEntNum); I(fontScale); I(font); I(alignOrg); I(alignScreen);
    I(color.rgba); I(fromColor.rgba); I(fadeStartTime); I(fadeTime); I(label); I(width); I(height);
    I(materialIndex); I(offscreenMaterialIdx); I(fromWidth); I(fromHeight); I(scaleStartTime); I(scaleTime);
    I(fromX); I(fromY); I(fromAlignOrg); I(fromAlignScreen); I(moveStartTime); I(moveTime); I(time); I(duration);
    I(value); I(text); I(sort); I(glowColor.rgba); I(fxBirthTime); I(fxLetterTime); I(fxDecayStartTime);
    I(fxDecayDuration); I(soundID); I(flags);
}
void sprint(Hash& hash, const SprintState& value) {
    I(sprintButtonUpRequired); I(sprintDelay); I(lastSprintStart); I(lastSprintEnd); I(sprintStartMaxLength);
}
void lerpFrame(Hash& hash, const lerpFrame_t& value) {
    I(yawAngle); I(yawing); I(pitchAngle); I(pitching); I(animationNumber);
    hash.text(value.animation ? value.animation->name : nullptr, 64);
    I(animationTime); A(oldFramePos); I(animSpeedScale); I(oldFrameSnapshotTime);
}
void frame(Hash& hash, const FxSpatialFrame& value) { A(quat); A(origin); }
void camera(Hash& hash, const FxCamera& value) {
    A(origin); I(isValid); A(axis); I(frustumPlaneCount); A(viewOffset);
    if (value.frustumPlaneCount > ARRAY_COUNT(value.frustum)) { hash.reject(); return; }
    for (unsigned i = 0; i < value.frustumPlaneCount; ++i) hash.array(value.frustum[i]);
}
bool fxReject(Hash& hash) { hash.reject(); return false; }
template<class Item, unsigned Capacity, class Used>
bool freeList(Hash& hash, const FxPool<Item>* pool, int head, const Used& used) {
    hash.scalar(head);
    bool seen[Capacity]{};
    for (unsigned count = 0; head != -1; ++count) {
        if (head < 0 || static_cast<unsigned>(head) >= Capacity || count >= Capacity || seen[head] || used[head]) return fxReject(hash);
        seen[head] = true; hash.scalar(head); head = pool[head].nextFree;
    }
    for (unsigned i = 0; i < Capacity; ++i) if (!used[i] && !seen[i]) return fxReject(hash);
    return true;
}
}

void KHZ_CanonicalPlayer(Hash& hash, const playerState_s& value) {
    hash.u32(1); // native field schema, distinct from the output file version
    I(commandTime); I(pm_type); I(bobCycle); I(pm_flags); I(weapFlags); I(otherFlags); I(pm_time);
    A(origin); A(velocity); A(oldVelocity); I(weaponTime); I(weaponDelay); I(grenadeTimeLeft);
    I(throwBackGrenadeOwner); I(throwBackGrenadeTimeLeft); I(weaponRestrictKickTime); I(foliageSoundTime);
    I(gravity); I(leanf); I(speed); A(delta_angles); I(groundEntityNum); A(vLadderVec); I(jumpTime); I(jumpOriginZ);
    I(legsTimer); I(legsAnim); I(torsoTimer); I(torsoAnim); I(legsAnimDuration); I(torsoAnimDuration);
    I(damageTimer); I(damageDuration); I(flinchYawAnim); I(movementDir); I(eFlags); I(eventSequence);
    A(events); A(eventParms); I(oldEventSequence); I(clientNum); I(offHandIndex); I(offhandSecondary);
    I(weapon); I(weaponstate); I(weaponShotCount); I(fWeaponPosFrac); I(adsDelayTime); I(spreadOverride);
    I(spreadOverrideState); I(viewmodelIndex); A(viewangles); I(viewHeightTarget); I(viewHeightCurrent);
    I(viewHeightLerpTime); I(viewHeightLerpTarget); I(viewHeightLerpDown); A(viewAngleClampBase); A(viewAngleClampRange);
    I(damageEvent); I(damageYaw); I(damagePitch); I(damageCount); A(stats); A(ammo); A(ammoclip);
    A(weapons); A(weaponold); A(weaponrechamber); I(proneDirection); I(proneDirectionPitch); I(proneTorsoPitch);
    I(viewlocked); I(viewlocked_entNum); I(cursorHint); I(cursorHintString); I(cursorHintEntIndex);
    I(iCompassPlayerInfo); I(radarEnabled); I(locationSelectionInfo); sprint(hash, value.sprintState);
    I(fTorsoPitch); I(fWaistPitch); I(holdBreathScale); I(holdBreathTimer); I(moveSpeedScaleMultiplier);
    I(mantleState.yaw); I(mantleState.timer); I(mantleState.transIndex); I(mantleState.flags);
    I(meleeChargeYaw); I(meleeChargeDist); I(meleeChargeTime); I(perks); A(actionSlotType);
    for (const auto& action : value.actionSlotParam) hash.scalar(action.specifyWeapon.index);
    I(entityEventSequence); I(weapAnim); I(aimSpreadScale); I(shellshockIndex); I(shellshockTime); I(shellshockDuration);
    I(dofNearStart); I(dofNearEnd); I(dofFarStart); I(dofFarEnd); I(dofNearBlur); I(dofFarBlur);
    I(dofViewmodelStart); I(dofViewmodelEnd); I(hudElemLastAssignedSoundID);
    for (const auto& objective : value.objective) {
        hash.scalar(objective.state); hash.array(objective.origin); hash.scalar(objective.entNum);
        hash.scalar(objective.teamNum); hash.scalar(objective.icon);
    }
    A(weaponmodels); I(deltaTime); I(killCamEntity);
    for (const auto& element : value.hud.current) hud(hash, element);
    for (const auto& element : value.hud.archival) hud(hash, element);
    I(khzBobRemainder);
}
void KHZ_CanonicalCommand(Hash& hash, const usercmd_s& value) {
    I(serverTime); I(buttons); A(angles); I(weapon); I(offHandIndex); I(forwardmove); I(rightmove);
    I(meleeChargeYaw); I(meleeChargeDist); A(selectedLocation);
}
void KHZ_CanonicalEntity(Hash& hash, const entityState_s& value) {
    I(number);
    if (value.eType < 0) { hash.reject(); return; }
    // Native entity-type tables select the transmitted union words explicitly.
    // Each entry is a reviewed four-byte field, rather than a struct/union dump.
    // This also keeps event subtype payloads in sync with the native protocol.
    const auto* fields = MSG_GetStateFieldListForEntityType(value.eType);
    hash.u32(fields->count);
    for (unsigned i = 0; i < fields->count; ++i) {
        const auto& field = fields->array[i];
        if (field.offset > sizeof(value) - sizeof(std::uint32_t)) { hash.reject(); return; }
        std::uint32_t bits; std::memcpy(&bits, reinterpret_cast<const char*>(&value) + field.offset, sizeof(bits));
        hash.u32(static_cast<std::uint32_t>(field.offset)); hash.u32(bits);
    }
}
void KHZ_CanonicalClient(Hash& hash, const clientState_s& value) {
    I(clientIndex); I(team); I(modelindex); A(attachModelIndex); A(attachTagIndex); hash.text(value.name, sizeof(value.name));
    I(maxSprintTimeMultiplier); I(rank); I(prestige); I(perks); I(attachedVehEntNum); I(attachedVehSlotIndex);
}
void KHZ_CanonicalController(Hash& hash, const clientInfo_t& value) {
    I(infoValid); I(nextValid); I(clientNum); I(team); I(oldteam); I(rank); I(prestige); I(perks);
    I(score); I(location); I(health); hash.text(value.name, sizeof(value.name)); hash.text(value.model, sizeof(value.model));
    for (const auto& name : value.attachModelNames) hash.text(name, sizeof(name));
    for (const auto& name : value.attachTagNames) hash.text(name, sizeof(name));
    lerpFrame(hash, value.legs); lerpFrame(hash, value.torso); I(lerpMoveDir); I(lerpLean); A(playerAngles);
    I(leftHandGun); I(dobjDirty); A(control.angles); A(control.tag_origin_angles); A(control.tag_origin_offset);
    A(clientConditions); I(iDObjWeapon); I(weaponModel); I(stanceTransitionTime); I(turnAnimEndTime); I(turnAnimType);
    I(attachedVehEntNum); I(attachedVehSlotIndex); I(hideWeapon); I(usingKnife);
}
void KHZ_CanonicalClientFeedback(Hash& hash, const cg_s& value) {
    I(time); I(oldTime); I(physicsTime); I(frametime); I(mapRestart); I(frameInterpolation);
    I(predictedErrorTime); A(predictedError); I(landChange); I(landTime); I(heightToCeiling);
    I(zoomSensitivity); I(invalidCmdHintType); I(invalidCmdHintTime); I(proneBlockedEndTime);
    I(weaponSelect); I(weaponSelectTime); I(weaponLatestPrimaryIdx); I(prevViewmodelWeapon); I(equippedOffHand);
    A(kickAVel); A(kickAngles); A(offsetAngles); A(vGunOffset); A(vGunSpeed); A(selectedLocation);
    I(lastActionSlotTime); I(extraButtons); I(playerTeleported); I(stepViewStart); I(stepViewChange); I(lastFrame.aimSpreadScale);
    I(playerEntity.fLastWeaponPosFrac); I(playerEntity.bPositionToADS); A(playerEntity.vPositionLastOrg);
    I(playerEntity.fLastIdleFactor); A(playerEntity.vLastMoveOrg); A(playerEntity.vLastMoveAng);
    I(shellshock.startTime); I(shellshock.duration); I(shellshock.loopEndTime); I(shellshock.sensitivity); A(shellshock.viewDelta);
    I(holdBreathTime); I(holdBreathInTime); I(holdBreathDelay); I(holdBreathFrac); sprint(hash, value.sprintStates);
    A(refdef.vieworg); A(refdef.viewaxis); A(refdefViewAngles); A(refdef.viewOffset); I(refdef.tanHalfFovX); I(refdef.tanHalfFovY);
    I(bgs.time); I(bgs.latestSnapshotTime); I(bgs.frametime);
}
void KHZ_CanonicalClientInput(Hash& hash, const clientActive_t& value) {
    I(usingAds); I(serverTime); I(oldServerTime); I(oldFrameServerTime); I(serverTimeDelta); I(oldSnapServerTime);
    I(extrapolatedSnapshot); I(newSnapshots); A(mouseDx); A(mouseDy); I(mouseIndex);
    I(stanceHeld); I(stance); I(stancePosition); I(stanceTime);
    I(cgameUserCmdWeapon); I(cgameUserCmdOffHandIndex); I(cgameFOVSensitivityScale); I(cgameMaxPitchSpeed); I(cgameMaxYawSpeed);
    A(cgameKickAngles); A(cgameOrigin); A(cgameVelocity); A(cgameViewangles); I(cgameBobCycle); I(cgameMovementDir);
    I(cgameExtraButtons); I(cgamePredictedDataServerTime); A(viewangles); I(cmdNumber);
    // Baseline/packet/history rings belong in the input/snapshot journal. The
    // current owner control state and command cursor are observed here once.
}
void KHZ_CanonicalAim(Hash& hash, const AimAssistGlobals& value) {
    I(initialized); I(tweakables.slowdownRegionWidth); I(tweakables.slowdownRegionHeight);
    I(tweakables.autoAimRegionWidth); I(tweakables.autoAimRegionHeight); I(tweakables.autoMeleeRegionWidth);
    I(tweakables.autoMeleeRegionHeight); I(tweakables.lockOnRegionWidth); I(tweakables.lockOnRegionHeight);
    A(viewOrigin); A(viewAngles); A(viewAxis); I(fovTurnRateScale); I(fovScaleInv); I(adsLerp); I(pitchDelta); I(yawDelta);
    I(screenWidth); I(screenHeight); A(screenMtx); A(invScreenMtx); I(screenTargetCount);
    if (value.screenTargetCount < 0 || value.screenTargetCount > 64) { hash.reject(); return; }
    for (int i = 0; i < value.screenTargetCount; ++i) {
        const auto& target = value.screenTargets[i]; hash.scalar(target.entIndex); hash.array(target.clipMins); hash.array(target.clipMaxs);
        hash.array(target.aimPos); hash.array(target.velocity); hash.scalar(target.distSqr); hash.scalar(target.crosshairDistSqr);
    }
    I(autoAimTargetEnt); I(autoAimPressed); I(autoAimActive); I(autoAimPitch); I(autoAimPitchTarget); I(autoAimYaw); I(autoAimYawTarget);
    I(autoMeleeTargetEnt); I(autoMeleeActive); I(autoMeleePressed); I(autoMeleePitch); I(autoMeleePitchTarget);
    I(autoMeleeYaw); I(autoMeleeYawTarget); I(lockOnTargetEnt);
}

bool KHZ_CanonicalGameplayFX(Hash& hash, const FxSystem& value) {
    I(isInitialized); I(isArchiving); I(needsGarbageCollection); I(localClientNum);
    if (!value.isInitialized || !value.effects || !value.elems || !value.trails || !value.trailElems ||
        value.isArchiving || value.iteratorCount || value.deferredElemCount ||
        value.firstFreeEffect != value.firstNewEffect || value.firstNewEffect - value.firstActiveEffect < 0 ||
        value.firstNewEffect - value.firstActiveEffect > 1024) return fxReject(hash);
    I(msecNow); I(msecDraw); I(frameCount); camera(hash, value.camera); camera(hash, value.cameraPrev);
    I(firstActiveEffect); I(firstNewEffect); I(firstFreeEffect); I(activeElemCount); I(activeTrailCount); I(activeTrailElemCount);
    A(restartList);
    bool effects[1024]{}, elements[2048]{}, trails[128]{}, trailElements[2048]{};
    unsigned countedElements = 0, countedTrails = 0, countedTrailElements = 0;
    for (int active = value.firstActiveEffect; active != value.firstNewEffect; ++active) {
        const auto handle = value.allEffectHandles[active & 1023];
        constexpr unsigned EffectHandleStride = sizeof(FxEffect) / 4;
        const unsigned index = handle / EffectHandleStride;
        // Native effect handles are four-byte-scaled offsets into the pool.
        if (index >= 1024 || handle % EffectHandleStride || effects[index]) return fxReject(hash);
        effects[index] = true; hash.scalar(handle);
        const auto& effect = value.effects[index];
        hash.text(effect.def ? effect.def->name : nullptr); if (!effect.def) return fxReject(hash);
        const int definitions = effect.def->elemDefCountLooping + effect.def->elemDefCountOneShot + effect.def->elemDefCountEmission;
        if (definitions < 0 || definitions > 128 || (definitions && !effect.def->elemDefs)) return fxReject(hash);
        hash.scalar(effect.status); hash.array(effect.firstElemHandle); hash.scalar(effect.firstTrailHandle); hash.scalar(effect.randomSeed);
        hash.scalar(effect.owner); hash.scalar(effect.boltAndSortOrder.dobjHandle); hash.scalar(effect.boltAndSortOrder.temporalBits);
        hash.scalar(effect.boltAndSortOrder.boneIndex); hash.scalar(effect.frameCount); hash.scalar(effect.msecBegin); hash.scalar(effect.msecLastUpdate);
        frame(hash, effect.frameAtSpawn); frame(hash, effect.frameNow); frame(hash, effect.framePrev); hash.scalar(effect.distanceTraveled);
        for (unsigned list = 0; list < 3; ++list) {
            unsigned short previous = FX_HANDLE_NONE;
            for (auto elementHandle = effect.firstElemHandle[list]; elementHandle != FX_HANDLE_NONE; ) {
                const unsigned elementIndex = elementHandle / (sizeof(FxElem) / FxElem::HANDLE_SCALE);
                if (elementHandle % (sizeof(FxElem) / FxElem::HANDLE_SCALE) || elementIndex >= 2048 || elements[elementIndex]) return fxReject(hash);
                elements[elementIndex] = true; ++countedElements; hash.scalar(elementHandle);
                const auto& element = value.elems[elementIndex].item;
                if (element.defIndex >= definitions || element.prevElemHandleInEffect != previous) return fxReject(hash);
                hash.scalar(element.defIndex); hash.scalar(element.sequence); hash.scalar(element.atRestFraction); hash.scalar(element.emitResidual);
                hash.scalar(element.nextElemHandleInEffect); hash.scalar(element.prevElemHandleInEffect); hash.scalar(element.msecBegin);
                hash.array(element.baseVel); hash.array(element.origin);
                if (effect.def->elemDefs[element.defIndex].elemType == FX_ELEM_TYPE_TRAIL) hash.scalar(element.u.trailTexCoord);
                previous = elementHandle; elementHandle = element.nextElemHandleInEffect;
            }
        }
        for (auto trailHandle = effect.firstTrailHandle; trailHandle != FX_HANDLE_NONE; ) {
            const unsigned trailIndex = trailHandle / (sizeof(FxTrail) / FxTrail::HANDLE_SCALE);
            if (trailHandle % (sizeof(FxTrail) / FxTrail::HANDLE_SCALE) || trailIndex >= 128 || trails[trailIndex]) return fxReject(hash);
            trails[trailIndex] = true; ++countedTrails; hash.scalar(trailHandle);
            const auto& trail = value.trails[trailIndex].item;
            if (trail.defIndex < 0 || trail.defIndex >= definitions) return fxReject(hash);
            hash.scalar(trail.nextTrailHandle); hash.scalar(trail.firstElemHandle); hash.scalar(trail.lastElemHandle);
            hash.scalar(trail.defIndex); hash.scalar(trail.sequence);
            unsigned short last = FX_HANDLE_NONE;
            for (auto elementHandle = trail.firstElemHandle; elementHandle != FX_HANDLE_NONE; ) {
                const unsigned elementIndex = elementHandle / (sizeof(FxTrailElem) / FxTrailElem::HANDLE_SCALE);
                if (elementHandle % (sizeof(FxTrailElem) / FxTrailElem::HANDLE_SCALE) || elementIndex >= 2048 || trailElements[elementIndex]) return fxReject(hash);
                trailElements[elementIndex] = true; ++countedTrailElements; hash.scalar(elementHandle);
                const auto& element = value.trailElems[elementIndex].item;
                hash.array(element.origin); hash.scalar(element.spawnDist); hash.scalar(element.msecBegin); hash.scalar(element.nextTrailElemHandle);
                hash.scalar(element.baseVelZ); hash.array(element.basis); hash.scalar(element.sequence);
                last = elementHandle; elementHandle = element.nextTrailElemHandle;
            }
            if (last != trail.lastElemHandle) return fxReject(hash);
            trailHandle = trail.nextTrailHandle;
        }
    }
    if (countedElements != value.activeElemCount || countedTrails != value.activeTrailCount || countedTrailElements != value.activeTrailElemCount)
        return fxReject(hash);
    // Effect-handle order and free-pool order influence future deterministic
    // allocation. Observe those lists without hashing stale freed contents.
    for (unsigned offset = 0; offset < 1024; ++offset) hash.scalar(value.allEffectHandles[(value.firstActiveEffect + offset) & 1023]);
    if (!freeList<FxElem,2048>(hash, value.elems, value.firstFreeElem, elements) ||
        !freeList<FxTrail,128>(hash, value.trails, value.firstFreeTrail, trails) ||
        !freeList<FxTrailElem,2048>(hash, value.trailElems, value.firstFreeTrailElem, trailElements)) return false;
    const auto* blockers = value.visStateBufferRead;
    if (!blockers || blockers->blockerCount < 0 || blockers->blockerCount > 256) return fxReject(hash);
    hash.scalar(blockers->blockerCount);
    for (int i = 0; i < blockers->blockerCount; ++i) {
        hash.array(blockers->blocker[i].origin); hash.scalar(blockers->blocker[i].radius); hash.scalar(blockers->blocker[i].visibility);
    }
    return hash.finish().valid;
}
const char* KHZ_ClientCanonicalGuardsJSON() {
    iassert(Sys_IsMainThread());
    static char output[1024];
    if (!KHZ_ClientOwnerRunning() || !KHZ_ClientOwnerControlsActive() || KHZ_IsPresentationContext()) {
        std::snprintf(output, sizeof(output), "{\"completed\":false,\"passed\":false,\"failure\":\"native_owner_controls_barrier_required\"}"); return output;
    }
    // playerState_s contains reviewed native arithmetic records and fixed HUD
    // payloads only; no process pointer or anonymous padding exists in this MP
    // layout. Complete byte coverage therefore catches a forgotten future field.
    const auto& source = CG_GetLocalClientGlobals(0)->predictedPlayerState;
    auto copy = std::make_unique<playerState_s>(source); Hash baseline; KHZ_CanonicalPlayer(baseline, source);
    const auto original = baseline.finish();
    unsigned checked = 0, missed = 0;
    auto* bytes = reinterpret_cast<unsigned char*>(copy.get());
    for (unsigned i = 0; i < sizeof(source); ++i) {
        bytes[i] ^= 1; Hash changed; KHZ_CanonicalPlayer(changed, *copy); const auto digest = changed.finish(); bytes[i] ^= 1;
        ++checked; if (digest.first == original.first && digest.second == original.second) ++missed;
    }
    // Conversely, usercmd_s ends in one native alignment byte. That byte must
    // not appear in canonical state or make replay depend on stack contents.
    usercmd_s command{}; CL_GetUserCmd(0, CL_GetLocalClientGlobals(0)->cmdNumber, &command);
    Hash originalCommand; KHZ_CanonicalCommand(originalCommand, command);
    auto padded = command; reinterpret_cast<unsigned char*>(&padded)[sizeof(padded) - 1] ^= 0xFF;
    Hash changedPadding; KHZ_CanonicalCommand(changedPadding, padded);
    const bool paddingExcluded = khz::canonical::equal(originalCommand.finish(), changedPadding.finish());
    // Exercise actual engine wrappers under this acknowledged owner control
    // scope. Preserve both native seeds exactly; no live tick sees test draws.
    const auto randomCheckpoint = KHZ_GetGameplayRandomState();
    KHZ_Srand(1); Rand_Init(0x89ABCDEFu);
    std::uint32_t expectedCrt = 1, expectedHold = 0x89ABCDEFu;
    bool randomPassed = true;
    constexpr unsigned RandomCases = 256;
    for (unsigned i = 0; i < RandomCases; ++i) {
        if (KHZ_Rand() != khz::msvcRandom(expectedCrt)) randomPassed = false;
        const float floatBits = static_cast<float>(khz::stockRandomBits(expectedHold));
        const float expectedFloat = static_cast<float>((8.5f - -3.5f) * floatBits / 32768.0 + -3.5f);
        if (flrand(-3.5f, 8.5f) != expectedFloat) randomPassed = false;
        const auto integerBits = khz::stockRandomBits(expectedHold);
        const int expectedInteger = static_cast<int>((std::int64_t(integerBits) * 2000) >> 15) - 1000;
        if (irand(-1000, 1000) != expectedInteger) randomPassed = false;
    }
    const auto advanced = KHZ_GetGameplayRandomState();
    randomPassed = randomPassed && advanced.crt == expectedCrt && advanced.hold == expectedHold;
    KHZ_SetGameplayRandomState(randomCheckpoint);
    const auto restored = KHZ_GetGameplayRandomState();
    const bool randomRestored = restored.crt == randomCheckpoint.crt && restored.hold == randomCheckpoint.hold;
    std::snprintf(output, sizeof(output),
        "{\"completed\":true,\"passed\":%s,\"native_player_bytes_checked\":%u,\"unobserved_player_bytes\":%u,\"initial_state_finite\":%s,\"native_command_padding_excluded\":%s,\"native_random_cases\":%u,\"native_random_wrappers_passed\":%s,\"native_random_restored\":%s,\"scope\":\"actual loaded private PS coverage, native command padding and exact dual-RNG wrapper roundtrip; no world restore/replay comparison performed\"}",
        !missed && original.valid && paddingExcluded && randomPassed && randomRestored ? "true" : "false", checked, missed,
        original.valid ? "true" : "false", paddingExcluded ? "true" : "false", RandomCases, randomPassed ? "true" : "false", randomRestored ? "true" : "false");
    return output;
}
#undef I
#undef A
