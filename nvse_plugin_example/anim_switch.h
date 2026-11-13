#pragma once

#include "GameProcess.h"
#include "NiNodes.h"

// One switch of animations on an Animation: the sequences it eases out (from) for the ones it eases in (to) between two
// of its updates. The engine makes one decision in several calls (ClearGroup, then StartGroup; aim, aim up and aim down
// are three StartGroups; Actor::ForceIdleAnimation clears the weapon sections for an idle), so the unit is the update,
// not the call or the section.
// Whoever switches the animations declares it here: kNVSE's own switches ease through EaseOut/EaseIn, the hooks around
// the engine's (Animation::StartGroup, Animation::ClearGroup) and the script commands Add what was eased. Nothing is
// inferred later: BlendTransitions blends the sequences of a switch as one unit and leaves an ease nobody declared to the
// regular blend, PoseBlendFixes finds the sequences a switch cut off.
class AnimSwitch
{
public:
    // The switch the Animation is making now: a new one once its animation time has moved on
    static AnimSwitch Current(AnimData* animData);

    // Eases the sequence out over fDuration. An ease-in in progress turns around at the weight it has reached (and lasts
    // as long as the switch's ease-ins, see CoverEaseIns), a sequence already easing out is left to it; a duration of 0
    // cuts it off
    bool EaseOut(NiControllerSequence* sequence, float fDuration) const;
    // Eases the sequence in over fDuration from its start, or from the weight it has left while it is still easing out
    bool EaseIn(NiControllerSequence* sequence, float fDuration) const;
    // A sequence something else eased in or out for this switch
    void Add(NiControllerSequence* sequence) const;

    UInt32 GetID() const { return m_uiID; }

    // The switch that eased the sequence into the state it is in; 0 if it is not easing or nothing declared the ease
    static UInt32 Of(const NiControllerSequence* sequence);

    static void WriteHooks();

private:
    AnimSwitch(const NiControllerManager* manager, UInt32 uiID, float fTime) : m_pkManager(manager), m_uiID(uiID), m_fTime(fTime) {}

    // Makes the sequence a member; false if it is not this Animation's or not easing
    bool Join(NiControllerSequence* sequence) const;
    // The sequence was eased out while the switch uiEasedInBy was still easing it in. What that switch was easing out
    // was making way for it, and now does for what this switch eases in instead: the members of that switch become
    // this one's, and its ease-outs, like the sequence itself, last until this switch's ease-ins are in (CoverEaseIns)
    void TakeOver(NiControllerSequence* sequence, UInt32 uiEasedInBy) const;
    // The ease-outs that make way for this switch's ease-ins (Ease::bMakesWay) are stretched to end with the last of
    // those, from the weight they have. Left to their own windows they end first: a sequence eased out while easing in
    // fades in the time it took to get that far (DeactivateNoReset mirrors the window; right beside a sequence turned
    // around with it, aim tapped twice: both take that long and keep adding up to 1), and what the interrupted switch
    // was easing out ends when that switch would have. What they and the ease-ins then leave of a bone falls to the
    // lower priorities: aiming down the sights just as an attack ended (the attack easing out, aim barely in and
    // turned around, aimIS starting) gave the first person arms' mtidle, the bind pose, up to half of them for the blend
    void CoverEaseIns() const;

    const NiControllerManager* m_pkManager;
    // 0: the Animation has no manager or scene root to keep it on
    UInt32 m_uiID;
    // the Animation's time
    float m_fTime;
};
