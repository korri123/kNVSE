#pragma once

#include <cmath>
#include <optional>
#include <vector>

#include "additive_anims.h"
#include "blend_curves.h"
#include "ease.h"
#include "skeleton_nodes.h"
#include "spawn_keys.h"

// A start or stop animation of a walk or run (an AnimGroupOverride file tagged _start or _stop), as MovementTransitions
// plays it in the movement section
enum class MovementTransition : UInt8
{
    None,
    Start,
    Stop,
};

class SequenceExtraData
{
public:
    bool startInReloadTargets = false;
    bool needsStoreTargets = false;
    // Set by MovementTransitions on the sequences it plays as start or stop animations. Those are only ever played as such
    // (normal picks leave the tagged files out), so it holds for as long as the sequence belongs to the manager
    MovementTransition movementTransition = MovementTransition::None;
    // An NPC's stop animation (MovementTransitions' arrivals): the goal of the path it was started for (once per approach),
    // and the distance to the stop point as of the last update
    NiPoint3 arrivalGoal{ NAN, NAN, NAN };
    float fArrivalDistance = NAN;
    // A stop animation's whole root motion at scale 1 (MovementTransitions' arrivals), once sampled
    float fRootDistance = NAN;
    std::optional<AdditiveSequenceMetadata> additiveMetadata;
    // The sequence's spawn:/despawn: keys (SpawnKeys), parsed when it joined the manager. The player's sequences only
    std::optional<SpawnKeys::Keys> spawnKeys;
    // The sequence's blendCurve/blendInCurve/blendOutCurve keys (BlendCurves), parsed when it joined the manager
    BlendCurves::Keys blendCurveKeys;
    // The ease the sequence is in, or was last in (Eases::Find tells)
    Ease ease;
};

// kNVSE's data about the sequences of the NiControllerManagers animating a node, kept as extra data on that node (the
// managers' target, an actor's animation root). Like kBlendInterpolatorExtraData on the bones, it needs no locks: an actor's
// animations are updated by one thread at a time, and sequences join and leave managers in the sequential animation update
// or on the main thread. It used to hang off the sequence's text keys, which every clone of a KF shares across actors.
class kAnimationExtraData : public NiExtraData
{
public:
    // The sequences kNVSE has data of and, at the same index, that data. Apart, so that finding a sequence reads 8 bytes
    // per sequence and not its data: BlendTransitions asks for the ease of every easing animation of every bone
    struct SequenceKey
    {
        const NiControllerManager* manager;
        const NiControllerSequence* sequence;

        bool operator==(const SequenceKey&) const = default;
    };
    std::vector<SequenceKey> sequenceKeys;
    std::vector<SequenceExtraData> sequenceData;
    // Where the sequences looked up last were found, by a hash of their address: tried first, as the same few easing
    // sequences are looked up for each of their bones. An index that is out of date is harmless: it is checked against
    // sequenceKeys
    static constexpr unsigned int kSequenceHintBits = 6;
    UInt16 sequenceHints[1u << kSequenceHintBits];
    // Animation::ClearGroup deactivating a sequence without an ease-out (PoseBlendFixes): its state, window and ease
    // just before, and the switch that cut it. Only the cuts of a manager's latest switch that cut any are kept, and a
    // sequence's cut goes with the sequence when it leaves the manager (SequenceExtraDatas::Delete): the address of a
    // sequence that is gone could be that of a new one
    struct HardCut
    {
        const NiControllerManager* manager;
        NiControllerSequence* sequence;
        NiControllerSequence::AnimState eState;
        float fStartTime;
        float fEndTime;
        UInt32 uiSwitch;
        Ease ease;
    };
    std::vector<HardCut> hardCuts;
    // The switch the Animation animating this node is making (AnimSwitch::Current): one per update of the Animation
    struct OpenSwitch
    {
        const NiControllerManager* manager = nullptr;
        UInt32 uiID = 0;
        float fTime = 0.0f;
    };
    OpenSwitch openSwitch;
    // The actor's pitch in radians, positive looking down, as of its last bone update (AimPitch::Update). Third person
    // skeletons only
    float aimPitch = 0.0f;
    // The latest end, in the managers' time, of the eases BlendCurves shaped with a curve a text key named: a sequence
    // whose ease ends after it has none (BlendCurves::HasNamedCurve, asked for every bone)
    float namedCurveEaseEnd = -NI_INFINITY;
    // The nodes kNVSE added to this skeleton (SkeletonNodes), added again when the model they were under comes back
    std::vector<SkeletonNodes::Node> addedNodes;

    NiNewRTTI(kAnimationExtraData, NiExtraData)

    void Destroy(bool freeMem);
    bool IsEqualEx(const kAnimationExtraData* other) const;

    static kAnimationExtraData* Get(NiObjectNET* obj);
    static kAnimationExtraData* GetOrCreate(NiObjectNET* obj);
};

// Data lives while the sequence belongs to a manager (m_pkOwner) that has a target. It is dropped when the sequence leaves
// the manager (NiControllerManager::RemoveSequence, RemoveAllSequences from ~Animation) or with the node. Returned pointers
// are valid until the next GetOrCreate or Delete on the same node.
// A sequence joins a manager in NiControllerManager::AddSequence (the engine's Animation::AddAnimation, kNVSE adding a
// cached sequence back); WriteHooks' hook there runs on the thread adding it, with m_pkOwner set and before the sequence
// is bound to the skeleton's nodes: the place to prepare what depends on the sequence alone (its text keys) and to give
// the skeleton the nodes it animates (SkeletonNodes::Require).
namespace SequenceExtraDatas
{
    SequenceExtraData* Get(const NiControllerSequence* sequence);
    // null if the sequence has no manager or the manager no target
    SequenceExtraData* GetOrCreate(NiControllerSequence* sequence);
    // call while sequence->m_pkOwner is still set. Also drops what else the node keeps of the sequence (its hard cut)
    void Delete(const NiControllerSequence* sequence);

    void WriteHooks();
}
