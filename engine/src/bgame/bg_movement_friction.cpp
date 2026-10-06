#include <universal/q_shared.h>
#include <universal/surfaceflags.h>
#include "bg_local.h"
#include <universal/com_math.h>
#include <qcommon/qcommon.h>
#include <algorithm>

void __cdecl PM_Friction(playerState_s *ps, pml_t *pml)
{
    float scale = 0; // [esp+8h] [ebp-2Ch]
    float value = 0; // [esp+Ch] [ebp-28h]
    float control = 0; // [esp+14h] [ebp-20h]

    float newspeed = 0; // [esp+1Ch] [ebp-18h]
    float drop = 0; // [esp+20h] [ebp-14h]
    float speed = 0; // [esp+24h] [ebp-10h]
    float vec[3] = { 0 }; // [esp+28h] [ebp-Ch] BYREF

    iassert(ps);

    float* vel = ps->velocity; // [esp+18h] [ebp-1Ch]
    vec[0] = ps->velocity[0];
    vec[1] = ps->velocity[1];
    vec[2] = ps->velocity[2];
    if (pml->walking)
        vec[2] = 0.0;

    speed = Vec3Length(vec);

    // Preserve the retail 50 ms stopping cutoff without erasing the first
    // sub-unit acceleration steps of ADS, crouching and bot movement at 1 kHz.
#ifdef COD4X1KHZ
    const float stoppingThreshold = (std::min)(1.0f, 20.0f * pml->frametime);
#else
    const float stoppingThreshold = 1.0f;
#endif
    if (speed >= stoppingThreshold && speed > 0.0f)
    {
        drop = 0.0;
        if ((ps->pm_flags & PMF_MELEE_CHARGE) != 0)
        {
            drop = player_meleeChargeFriction->current.value * pml->frametime;
        }
        else if (pml->walking && (pml->groundTrace.surfaceFlags & SURF_SLICK) == 0 && (ps->pm_flags & PMF_TIME_KNOCKBACK) == 0)
        {
            if (stopspeed->current.value <= (double)speed)
                value = speed;
            else
                value = stopspeed->current.value;

            control = value;

            if ((ps->pm_flags & PMF_TIME_HARDLANDING) != 0)
            {
                control = value * 0.300000011920929;
            }
            else if ((ps->pm_flags & PMF_JUMPING) != 0)
            {
                control = Jump_ReduceFriction(ps) * value;
            }

            drop = control * friction->current.value * pml->frametime + drop;
        }

#ifdef KISAK_MP
        if (ps->pm_type == PM_SPECTATOR)
            drop = speed * 5.0 * pml->frametime + drop;
#endif
        newspeed = speed - drop;
        if (newspeed < 0.0)
            newspeed = 0.0;
        scale = newspeed / speed;
        Vec3Scale(vel, scale, vel);
    }
    else
    {
        *vel = 0.0;
        ps->velocity[1] = 0.0;
        ps->velocity[2] = 0.0;
    }
}

void __cdecl PM_Accelerate(playerState_s *ps, const pml_t *pml, const float *wishdir, float wishspeed, float accel)
{
    float value; // [esp+Ch] [ebp-44h]
    float wishVelocity[3]; // [esp+1Ch] [ebp-34h] BYREF
    float pushDir[3]; // [esp+28h] [ebp-28h] BYREF
    float pushLen; // [esp+34h] [ebp-1Ch]
    float canPush; // [esp+38h] [ebp-18h]
    float inertiaspeed; // [esp+3Ch] [ebp-14h]
    float control; // [esp+40h] [ebp-10h]
    float addspeed; // [esp+44h] [ebp-Ch]
    float currentspeed; // [esp+48h] [ebp-8h]
    float accelspeed; // [esp+4Ch] [ebp-4h]

    if ((ps->pm_flags & PMF_LADDER) != 0)
    {
        Vec3Scale(wishdir, wishspeed, wishVelocity);
        Vec3Sub(wishVelocity, ps->velocity, pushDir);
        pushLen = Vec3Normalize(pushDir);

        canPush = accel * pml->frametime * wishspeed;
        if (canPush > pushLen)
            canPush = pushLen;

        Vec3Mad(ps->velocity, canPush, pushDir, ps->velocity);
    }
    else
    {
        iassert(ps);

        currentspeed = Vec3Dot(ps->velocity, wishdir);
        addspeed = wishspeed - currentspeed;
        if (addspeed <= 0)
        {
            return;
        }

        if (stopspeed->current.value <= (double)wishspeed)
            value = wishspeed;
        else
            value = stopspeed->current.value;

        control = value;
        accelspeed = accel * pml->frametime * value;

        if (accelspeed > addspeed)
            accelspeed = addspeed;

        // Inertia system slows down immediate turn arounds (A/D/A/D spam, or 180 sprint's)
        // It's set to 50, which is so high it doesn't trigger. I couldn't get it to trigger over 10
        inertiaspeed = PM_PlayerInertia(ps, accelspeed, wishdir);
        Vec3Mad(ps->velocity, inertiaspeed, wishdir, ps->velocity);

        // blops removed inertia and just did this:
        //Vec3Mad(ps->velocity, accelspeed, wishdir, ps->velocity);
    }
}

double __cdecl PM_PlayerInertia(const playerState_s *ps, float accelspeed, const float *wishdir)
{
    if (ps->pm_type == PM_NOCLIP)
        return accelspeed;

    if (accelspeed <= (double)inertiaMax->current.value)
        return accelspeed;

    float v4 = ps->oldVelocity[1] * ps->oldVelocity[1] + ps->oldVelocity[0] * ps->oldVelocity[0]; // [esp+8h] [ebp-8h]
    if (v4 < 0.0001)
        return accelspeed;

    if (PM_DoPlayerInertia(ps, accelspeed, wishdir))
        return inertiaMax->current.value;

    return accelspeed;
}

bool __cdecl PM_DoPlayerInertia(const playerState_s *ps, float accelspeed, const float *wishdir)
{
    float v4; // [esp+28h] [ebp-34h]
    float v5; // [esp+2Ch] [ebp-30h]
    float v6; // [esp+30h] [ebp-2Ch]
    float scaledDotAngle; // [esp+3Ch] [ebp-20h]
    float velocity; // [esp+40h] [ebp-1Ch]
    float velocity_4; // [esp+44h] [ebp-18h]
    float oldVelLenSq; // [esp+48h] [ebp-14h]
    float newVelLenSq; // [esp+4Ch] [ebp-10h]
    float oldVelocity; // [esp+54h] [ebp-8h]
    float oldVelocity_4; // [esp+58h] [ebp-4h]

    velocity = accelspeed * *wishdir + ps->velocity[0];
    velocity_4 = accelspeed * wishdir[1] + ps->velocity[1];
    oldVelocity = ps->oldVelocity[0];
    oldVelocity_4 = ps->oldVelocity[1];
    oldVelLenSq = oldVelocity_4 * oldVelocity_4 + oldVelocity * oldVelocity;
    newVelLenSq = velocity_4 * velocity_4 + velocity * velocity;
    v6 = newVelLenSq * oldVelLenSq;
    v5 = sqrt(v6);
    scaledDotAngle = velocity_4 * oldVelocity_4 + velocity * oldVelocity;
    v4 = inertiaAngle->current.value * v5;

    if (scaledDotAngle >= (double)v4)
        return false;

    if (inertiaDebug->current.enabled)
    {
        Com_Printf(
            CON_CHANNEL_PLAYERWEAP,
            "angle is %f (oldVel is (%f,%f), vel is (%f, %f))\n",
            scaledDotAngle / v5,
            oldVelocity,
            oldVelocity_4,
            velocity,
            velocity_4);
        Com_Printf(CON_CHANNEL_PLAYERWEAP, "clamping acceleration from %f to %f\n", accelspeed, inertiaMax->current.value);
    }

    return true;
}
