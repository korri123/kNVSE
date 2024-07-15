#pragma once
#include "GameProcess.h"
#include "NiNodes.h"

bool HasNoFixTextKey(BSAnimGroupSequence* anim);

namespace AnimFixes
{
    void FixWrongAKeyInRespectEndKey(AnimData* animData, BSAnimGroupSequence* anim);
    void FixInconsistentEndTime(BSAnimGroupSequence* anim);
    void EraseNullTextKeys(const BSAnimGroupSequence* anim);
    void ApplyFixes(AnimData* animData, BSAnimGroupSequence* anim);
}
