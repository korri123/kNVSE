#pragma once
#include "GameProcess.h"
#include "NiNodes.h"


namespace AnimFixes
{
    void FixWrongAKeyInRespectEndKey(AnimData* animData, BSAnimGroupSequence* anim);
    void FixInconsistentEndTime(BSAnimGroupSequence* anim);
    void ApplyFixes(AnimData* animData, BSAnimGroupSequence* anim);
}
