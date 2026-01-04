#pragma once
#include "GameProcess.h"
#include "NiNodes.h"

inline const char* sAdditiveSequenceMetadata = "AdditiveSequenceMetadata";

struct AdditiveSequenceMetadata
{
    const char* type = sAdditiveSequenceMetadata;
    NiPointer<NiControllerSequence> referencePoseSequence;
    float referenceTimePoint;
    bool ignorePriorities;
    NiControllerManager* controllerManager;
    bool firstPerson;
    bool operator==(const AdditiveSequenceMetadata& other) const = default;
};

namespace AdditiveManager
{
    bool IsAdditiveSequence(NiControllerSequence* sequence);
    void EraseAdditiveSequence(NiControllerSequence* sequence);
    void InitAdditiveSequence(AnimData* animData, NiControllerSequence* additiveSequence, NiControllerSequence* referencePoseSequence, float
                              timePoint, bool ignorePriorities);
    void PlayManagedAdditiveAnim(AnimData* animData, BSAnimGroupSequence* referenceAnim, BSAnimGroupSequence* additiveAnim);
    void SetAdditiveInterpWeightMult(NiObjectNET* target, NiInterpolator* interpolator, float weightMult);
    void WriteHooks();
    bool IsAdditiveInterpolator(kBlendInterpolatorExtraData* extraData, NiInterpolator* interpolator);
    void MarkInterpolatorsAsAdditive(const NiControllerSequence* additiveSequence);
    void AddReferencePoseTransforms(NiControllerSequence* additiveSequence, NiControllerSequence* referencePoseSequence, float timePoint, bool ignorePriorities);
}
