#pragma once
#include "GameProcess.h"
#include "NiNodes.h"

bool HasNoFixTextKey(const BSAnimGroupSequence* anim);
bool HasRespectEndKey(BSAnimGroupSequence* anim);

namespace AnimFixes
{
    void FixWrongAKeyInRespectEndKey(AnimData* animData, BSAnimGroupSequence* anim);
    void FixInconsistentEndTime(BSAnimGroupSequence* anim);
    void EraseNullTextKeys(const BSAnimGroupSequence* anim);
    void EraseNegativeAnimKeys(const BSAnimGroupSequence* anim);
    void ApplyFixes(AnimData* animData, BSAnimGroupSequence* anim);
    void FixWrongKFName(BSAnimGroupSequence* anim, const char* filePath);
    void FixWrongPrnKey(BSAnimGroupSequence* anim);
    void FixMissingPrnKey(BSAnimGroupSequence* anim, const char* filePath);
    void AddNoBlendSmoothingKeys(BSAnimGroupSequence* anim);
    // Blend keys of a walk or run below the game's default blend: raised to it in the text keys, before they are parsed
    void FixShortGaitBlend(BSAnimGroupSequence* anim, const char* filePath);

    void ApplyHooks();
}
