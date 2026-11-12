#pragma once
#include <cmath>

#include "GameProcess.h"
#include "NiNodes.h"

namespace MovementBlendFixes
{
    // Cross fades the movement section to pkSequence over fDuration (its ease-in time when NAN)
    BSAnimGroupSequence* PlayMovementAnim(AnimData* animData, BSAnimGroupSequence* pkSequence, float fDuration = NAN);
    void TrackLastEaseOut(AnimData* animData);
    // How far the feet of the sequence at fTime are from those of the skeleton's last evaluated pose: the squared
    // distance between the two strides (the left foot relative to the right one, in the thighs' parent space). NAN when
    // the skeleton or the sequence has no leg bones
    float GetStrideMismatch(AnimData* animData, const NiControllerSequence* sequence, float fTime);
}
