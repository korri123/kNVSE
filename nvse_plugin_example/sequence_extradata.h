#pragma once

#include <optional>
#include <vector>

#include "additive_anims.h"

class SequenceExtraData
{
public:
    bool startInReloadTargets = false;
    bool needsStoreTargets = false;
    std::optional<AdditiveSequenceMetadata> additiveMetadata;
};

// kNVSE's data about the sequences of the NiControllerManagers animating a node, kept as extra data on that node (the
// managers' target, an actor's animation root). Like kBlendInterpolatorExtraData on the bones, it needs no locks: an actor's
// animations are updated by one thread at a time, and sequences join and leave managers in the sequential animation update
// or on the main thread. It used to hang off the sequence's text keys, which every clone of a KF shares across actors.
class kAnimationExtraData : public NiExtraData
{
public:
    struct Entry
    {
        NiControllerManager* manager;
        NiControllerSequence* sequence;
        SequenceExtraData data;
    };
    std::vector<Entry> entries;

    NiNewRTTI(kAnimationExtraData, NiExtraData)

    void Destroy(bool freeMem);
    bool IsEqualEx(const kAnimationExtraData* other) const;

    static kAnimationExtraData* Get(NiObjectNET* obj);
    static kAnimationExtraData* GetOrCreate(NiObjectNET* obj);
};

// Data lives while the sequence belongs to a manager (m_pkOwner) that has a target. It is dropped when the sequence leaves
// the manager (NiControllerManager::RemoveSequence, RemoveAllSequences from ~Animation) or with the node. Returned pointers
// are valid until the next GetOrCreate or Delete on the same node.
namespace SequenceExtraDatas
{
    SequenceExtraData* Get(const NiControllerSequence* sequence);
    // null if the sequence has no manager or the manager no target
    SequenceExtraData* GetOrCreate(NiControllerSequence* sequence);
    // call while sequence->m_pkOwner is still set
    void Delete(const NiControllerSequence* sequence);

    void WriteHooks();
}
