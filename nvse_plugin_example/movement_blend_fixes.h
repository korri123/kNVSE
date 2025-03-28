#pragma once
#include "GameProcess.h"
#include "NiNodes.h"

namespace MovementBlendFixes
{
    BSAnimGroupSequence* PlayMovementAnim(AnimData* animData, BSAnimGroupSequence* pkSequence);
    NiControllerSequence* CreateExtraIdleSequence(AnimData* animData, BSAnimGroupSequence* pkIdleSequence);
}