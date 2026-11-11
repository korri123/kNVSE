#pragma once

#include <optional>
#include <vector>

#include "additive_anims.h"

class SequenceExtraData
{
public:
    // Animation::ClearGroup deactivating the sequence without an ease-out (PoseBlendFixes): its state just before, and
    // the switch that cut it
    struct HardCut
    {
        NiControllerSequence::AnimState eState;
        float fStartTime;
        float fEndTime;
        UInt32 uiSwitch;
    };

    // The switch that eased the sequence in or out (AnimSwitch). Holds while the sequence stays in that state, and is
    // dropped when it is activated again
    struct SwitchMembership
    {
        UInt32 uiSwitch;
        NiControllerSequence::AnimState eState;
    };

    bool startInReloadTargets = false;
    bool needsStoreTargets = false;
    std::optional<AdditiveSequenceMetadata> additiveMetadata;
    std::optional<HardCut> hardCut;
    std::optional<SwitchMembership> switchMembership;
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
    // The switch the Animation animating this node is making (AnimSwitch::Current): one per update of the Animation
    struct OpenSwitch
    {
        const NiControllerManager* manager = nullptr;
        UInt32 uiID = 0;
        float fTime = 0.0f;
    };
    OpenSwitch openSwitch;

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
