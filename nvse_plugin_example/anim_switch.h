#pragma once

#include "GameProcess.h"
#include "NiNodes.h"

// One switch of animations on an Animation: the sequences it eases out (from) for the ones it eases in (to) between two
// of its updates. The engine makes one decision in several calls (ClearGroup then StartGroup; aim, aim up and aim down
// are three StartGroups; Actor::ForceIdleAnimation clears the weapon sections for an idle), so the unit is the update,
// not the call or the section.
// Whoever switches the animations declares it here. kNVSE's own switches ease through EaseOut/EaseIn, and the hooks
// around the engine's (Animation::StartGroup, Animation::ClearGroup) and the script commands call Add for what was
// eased. Nothing is inferred later. BlendTransitions blends the sequences of a switch as one unit and leaves an ease
// nobody declared to the regular blend. PoseBlendFixes finds the sequences a switch cut off.
class AnimSwitch
{
public:
    // Returns the switch the Animation is making now; a new one once its animation time has moved on
    static AnimSwitch Current(AnimData* animData);

    // Eases the sequence out over fDuration. An ease-in in progress turns around at the weight it has reached and lasts
    // as long as the switch's ease-ins (see CoverEaseIns). A sequence already easing out is left alone. A duration of 0
    // cuts the sequence off.
    bool EaseOut(NiControllerSequence* sequence, float fDuration) const;
    // Eases the sequence in over fDuration from its start, or from the weight it has left if it is still easing out
    bool EaseIn(NiControllerSequence* sequence, float fDuration) const;
    // Adds a sequence that something else eased in or out for this switch
    void Add(NiControllerSequence* sequence) const;

    UInt32 GetID() const { return m_uiID; }

    // Returns the switch that eased the sequence into its current state; 0 if it is not easing or no switch declared it
    static UInt32 Of(const NiControllerSequence* sequence);

    static void WriteHooks();

private:
    AnimSwitch(const NiControllerManager* manager, UInt32 uiID, float fTime) : m_pkManager(manager), m_uiID(uiID), m_fTime(fTime) {}

    // Makes the sequence a member. Returns false if it is not this Animation's or not easing
    bool Join(NiControllerSequence* sequence) const;
    // Called when the sequence was eased out while switch uiEasedInBy was still easing it in. What that switch was
    // easing out was making way for the sequence, and now makes way for what this switch eases in instead, so the
    // members of that switch become this one's. The sequence itself lasts until this switch's ease-ins are in
    // (CoverEaseIns). The ease-outs it takes over keep their own windows.
    void TakeOver(NiControllerSequence* sequence, UInt32 uiEasedInBy) const;
    // Stretches the ease-outs that make way for this switch's ease-ins (Ease::bMakesWay) to end with the last of those
    // ease-ins, starting from the weight they have. With its own window, a sequence that was eased out while easing in
    // fades in the time it took to get that far (DeactivateNoReset mirrors the window). That keeps it and a sequence
    // turned around with it (aim tapped twice) adding up to 1.
    // Without this, what the ease-outs and ease-ins leave of a bone falls to the lower priorities. Example: aiming down
    // the sights just as an attack ended gave the first person arms' mtidle (the bind pose) up to half of the blend.
    // While the stretch lasts, the ease-ins below it take what it and the other ease-outs leave (the regular blend's
    // levels).
    void CoverEaseIns() const;

    const NiControllerManager* m_pkManager;
    // 0 if the Animation has no manager or scene root to keep the switch on
    UInt32 m_uiID;
    // The Animation's time when the switch began
    float m_fTime;
};
