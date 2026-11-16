#pragma once

#include <memory>

#include "NiNodes.h"

// One pass of a skeleton's NiMultiTargetTransformController over its bones, each blend writing its bone's local
// transform. Every such pass is kNVSE's: the engine's bone update (BSTransCon::UpdateAllButTarget 0x4F0380, called at
// 0x4F0087 and 0x4EFE53, where Stewie's Tweaks puts its copy of it) and the controller's virtual Update
// (NiAVObject::UpdateObjectControllers).
//
// A bone's blend can need the local transforms other bones get in the same pass: the Weapon node's the arms'
// (WeaponHandBlend), an anchored bone's those of the bones above it (Anchors). Require evaluates such a bone first and
// the pass then leaves it out, so no feature depends on the order of the controller's slots (the order in which the
// skeleton's KFs first named the nodes). Call it before the blend that needs the bone starts: a bone's evaluation
// uses per-thread scratch data that a nested one would overwrite.
namespace BoneEvaluation
{
    class Pass
    {
    public:
        enum class Rule
        {
            // BSTransCon::UpdateAllButTarget with bSelective false (the bones the engine updates every frame) or true
            // (the bones bone LOD switched off, BSAnimGroupSequence::Deactivate): each bone whose selective update
            // flag differs from it, and the last one if either is set
            AllButTarget,
            AllButTargetSelective,
            // NiMultiTargetTransformController::Update: each bone whose selective update flag is set
            Selected,
        };

        Pass(NiMultiTargetTransformController& controller, float fTime, Rule rule);
        ~Pass();
        Pass(const Pass&) = delete;
        Pass& operator=(const Pass&) = delete;

        // Evaluates every bone the rule updates that was not evaluated yet
        void Run();

    private:
        friend bool Require(const NiAVObject* node);

        bool Updates(unsigned short slot) const;
        bool IsDone(unsigned short slot) const;
        void Evaluate(unsigned short slot);

        static constexpr unsigned int kInlineSlots = 512;

        NiMultiTargetTransformController& m_controller;
        float m_fTime;
        Rule m_rule;
        Pass* m_outer;
        UInt32 m_inlineDone[kInlineSlots / 32];
        std::unique_ptr<UInt32[]> m_heapDone;
        UInt32* m_done;
    };

    // Makes the node's local transform this pass's, evaluating its blend now if the current pass on this thread updates
    // it and has not yet. Returns true if the node's local transform is this pass's, or the pass's controller does not
    // animate the node (nothing changes it). Returns false outside a pass and for bones the pass leaves alone.
    bool Require(const NiAVObject* node);
}
