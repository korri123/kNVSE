#pragma once

#include <memory>
#include "additive_anims.h"

class SequenceExtraData 
{
public:
    bool startInReloadTargets = false;
    bool needsStoreTargets = false;
    std::unique_ptr<AdditiveSequenceMetadata> additiveMetadata = nullptr;
};

class SequenceExtraDatas
{
    std::vector<std::pair<NiControllerSequence*, SequenceExtraData>> extraData;
public:
    static SequenceExtraData* Get(const NiControllerSequence* sequence);
    static SequenceExtraData* GetOrCreate(NiControllerSequence* sequence);

    static void Delete(NiControllerSequence* sequence);
};