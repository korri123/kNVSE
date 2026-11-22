#pragma once

#include <cmath>
#include <optional>
#include <vector>

#include "actor_overrides.h"
#include "additive_anims.h"
#include "anchor.h"
#include "blend_curves.h"
#include "ease.h"
#include "reload_state.h"
#include "skeleton_nodes.h"
#include "spawn_keys.h"
#include "transition_anims.h"
#include "weapon_fire.h"

class SequenceExtraData
{
public:
    bool startInReloadTargets = false;
    // Set when the sequence is played as a start or stop animation (TransitionAnims): by MovementTransitions for a walk
    // or run, by AimTransitions for aiming (bAimTransition). These files are only played as such (normal picks skip
    // the tagged files), so the value holds for as long as the sequence belongs to the manager.
    TransitionKind transition = TransitionKind::None;
    bool bAimTransition : 1 = false;
    // Set for an aim transition played backwards (AimTransitions): its time runs backwards while it plays
    bool bAimReversed : 1 = false;
    // An NPC's stop animation (MovementTransitions' arrivals): the goal of the path it was started for (set once per
    // approach) and the distance to the stop point as of the last update
    NiPoint3 arrivalGoal{ NAN, NAN, NAN };
    float fArrivalDistance = NAN;
    // A stop animation's total root motion at scale 1 (MovementTransitions' arrivals), once sampled
    float fRootDistance = NAN;
    std::optional<AdditiveSequenceMetadata> additiveMetadata;
    // The sequence's spawn:/despawn: keys (SpawnKeys), parsed when it joined the manager or its KF's keys were replaced.
    // Player sequences only
    std::optional<SpawnKeys::Keys> spawnKeys;
    // The sequence's blendCurve/blendInCurve/blendOutCurve keys (BlendCurves), parsed when it joined the manager, and
    // again when it resolves a curve after any KF's keys were replaced (blendCurveKeysGeneration)
    BlendCurves::Keys blendCurveKeys;
    UInt32 blendCurveKeysGeneration = 0;
    // The ease the sequence is in or was last in (see Eases::Find)
    Ease ease;
};

// kNVSE's data about the sequences of the NiControllerManagers animating a node, kept as extra data on that node (the
// managers' target, an actor's animation root). Like kBlendInterpolatorExtraData on the bones, it needs no locks: an
// actor's animations are updated by one thread at a time, and sequences join and leave managers in the sequential
// animation update or on the main thread. It is not stored on the sequence's text keys because every clone of a KF
// shares those across actors.
class kAnimationExtraData : public NiExtraData
{
public:
    // The sequences kNVSE has data for and, at the same index, that data. Kept apart so that finding a sequence reads
    // 8 bytes per sequence instead of its data. BlendTransitions asks for the ease of every easing animation of every bone.
    struct SequenceKey
    {
        const NiControllerManager* manager;
        const NiControllerSequence* sequence;

        bool operator==(const SequenceKey&) const = default;
    };
    std::vector<SequenceKey> sequenceKeys;
    std::vector<SequenceExtraData> sequenceData;
    // Index of the last lookup of each sequence, by a hash of its address. Tried first, because the same few easing
    // sequences are looked up for each of their bones. A stale index is harmless since it is checked against
    // sequenceKeys.
    static constexpr unsigned int kSequenceHintBits = 6;
    UInt16 sequenceHints[1u << kSequenceHintBits];
    // Sequences that Animation::ClearGroup deactivated without an ease-out (PoseBlendFixes): their state, window and
    // ease just before, and the switch that cut them. Only the cuts of a manager's latest switch that cut any are kept.
    // A sequence's cut is removed when it leaves the manager (SequenceExtraDatas::Delete), because the address of a
    // destroyed sequence can be reused by a new one.
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
    // Sequences to bind to the skeleton again at their next activation (AttachInterpolatorsHooked). The hook at 0x49918B
    // (Animation::ClearControllersInterpolators: dying actors on every update, RevertTES) deactivates their manager and
    // clears their blend interpolators. Kept apart from sequenceData, which would get an entry for every sequence of
    // every dying actor. Removed with the sequence (SequenceExtraDatas::Delete), like a hard cut.
    std::vector<SequenceKey> pendingStoreTargets;
    // The switch the Animation animating this node is making (AnimSwitch::Current); one per update of the Animation
    struct OpenSwitch
    {
        const NiControllerManager* manager = nullptr;
        UInt32 uiID = 0;
        float fTime = 0.0f;
    };
    OpenSwitch openSwitch;
    // The actor's pitch in radians, positive looking down, as of its last bone update (AimPitch::Update).
    // Third person skeletons only.
    float aimPitch = 0.0f;
    // The latest end, in the managers' time, of the eases that BlendCurves shaped with a named curve. A sequence whose
    // ease ends after it has no named curve (BlendCurves::HasNamedCurve, asked for every bone).
    float namedCurveEaseEnd = -NI_INFINITY;
    // The nodes kNVSE added to this skeleton (SkeletonNodes); added again when the model they were under returns
    std::vector<SkeletonNodes::Node> addedNodes;
    // A sequence anchored a bone of this skeleton (Anchors::Attach), and the skeleton's rest pose, found before its
    // next bone update (Anchors::Update). The pose is kept for the session.
    bool wantsRestPose = false;
    const Anchors::RestPose* restPose = nullptr;
    // The override animations of the Animation whose controller manager animates this node (ActorOverrides)
    ActorOverrides overrides;
    // The reload of the actor this is the 3rd person Animation of (AmmoKeys, NPCLoopingReloadFix)
    ReloadState reload;
    // The shots of the actor this is the 3rd person Animation of (WeaponFire)
    FireState fire;

    NiNewRTTI(kAnimationExtraData, NiExtraData)

    void Destroy(bool freeMem);
    bool IsEqualEx(const kAnimationExtraData* other) const;

    static kAnimationExtraData* Get(NiObjectNET* obj);
    static kAnimationExtraData* GetOrCreate(NiObjectNET* obj);
};

// Data lives while the sequence belongs to a manager (m_pkOwner) that has a target. It is dropped when the sequence
// leaves the manager (NiControllerManager::RemoveSequence, RemoveAllSequences from ~Animation) or with the node.
// Returned pointers are valid until the next GetOrCreate or Delete on the same node.
// A sequence joins a manager in NiControllerManager::AddSequence (the engine's Animation::AddAnimation, or kNVSE adding
// a cached sequence back). WriteHooks installs a hook there that runs on the adding thread, with m_pkOwner set and
// before the sequence is bound to the skeleton's nodes. It prepares what depends on the sequence alone (its text keys)
// and gives the skeleton the nodes it animates (SkeletonNodes::Require).
namespace SequenceExtraDatas
{
    SequenceExtraData* Get(const NiControllerSequence* sequence);
    // Returns null if the sequence has no manager or the manager has no target
    SequenceExtraData* GetOrCreate(NiControllerSequence* sequence);
    // Call while sequence->m_pkOwner is still set. Also drops the other data the node keeps for the sequence (its hard cut)
    void Delete(const NiControllerSequence* sequence);
    // Marks every sequence of the manager to be bound again at its next activation (see TakeStoreTargets)
    void MarkStoreTargets(NiControllerManager* manager);
    // Returns whether MarkStoreTargets marked the sequence, and clears the mark
    bool TakeStoreTargets(const NiControllerSequence* sequence);

    void WriteHooks();
}
