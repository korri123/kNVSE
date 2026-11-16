#pragma once

#include "blend_curves.h"
#include "NiNodes.h"

class SequenceExtraData;

// One ease of a sequence: the time it spends in EASEIN, EASEOUT, TRANSDEST or TRANSSOURCE after something eased it
// there. The engine only keeps the sequence's state and the window its spinner runs over (m_fStartTime, m_fEndTime),
// and whatever eases a sequence rewrites those. Everything else kNVSE knows about an ease is kept in this record, in
// the sequence's SequenceExtraData:
//  - the switch of animations that eased the sequence (AnimSwitch)
//  - the curve that switch runs on and where on it this ease started (BlendCurves)
//  - whether the sequence continues a variant of its group (BlendFixes)
// The record is the sequence's ease while the sequence is in the state it was created for (Eases::Find). It is dropped
// when the engine starts another ease: when the sequence is activated (AttachInterpolatorsHooked: Activate, StartBlend,
// StartMorph) and when it is eased out (the Deactivate hooks). kNVSE's own changes carry it over: turning around
// (Eases::Reverse, from ActivateNoReset / DeactivateNoReset) and PoseBlendFixes restoring the ease a hard cut
// interrupted. An ease without a record is created by the first caller of Eases::Get, at its current position. Until
// then it ran on the default curve, which needs no record.
struct Ease
{
    // The state the sequence was eased into; INACTIVE means no ease
    NiControllerSequence::AnimState eState = NiControllerSequence::INACTIVE;
    // The AnimSwitch that eased it; 0 if no switch declared the ease
    UInt32 uiSwitch = 0;
    // Where the ease starts: the engine's spinner there (0 easing in, 1 easing out, in between for an ease that
    // continues another or was first asked for while under way) and how far along its curve that is
    float fFromSpinner = 0.0f;
    float fFromProgress = 0.0f;
    // The engine's spinner at its last evaluation (BlendCurves::Shape); the start value before the first one
    float fLastSpinner = 0.0f;
    // The curve the ease runs on, and the curve of its switch as last resolved (BlendCurves::Shape moves the ease onto
    // that one). Not resolved yet for a sequence new to its switch.
    BlendCurve curve;
    BlendCurve switchCurve;
    bool bCurveResolved = false;
    // Set when the sequence was cross faded in from another variant of its group at the same point (BlendFixes'
    // TransitionToVariant). It continues that sequence, so Animation::Update acts on its group's keys and scales its
    // clock while it eases in, as for the ANIMATING sequence it replaced. Applies from fContinuesFrom (its owner's
    // time), because a replaced sequence that was still easing in counts as ANIMATING only once that ease ends.
    bool bContinuesVariant = false;
    float fContinuesFrom = -NI_INFINITY;
    // The other side: set on the ease-out of the sequence a variant continues. Animation::Update runs only the
    // variant's clock at the group's speed, so this sequence is kept at the variant's point of the sequence while both
    // blend (BlendFixes' LockVariantSources).
    bool bContinuedByVariant = false;
    // Set when an ease-out was turned around from an ease-in (AnimSwitch::TakeOver). It lasts until the switch's
    // ease-ins finish.
    bool bMakesWay = false;

    // Returns an ease into the state at the given spinner, on the default curve
    static Ease At(NiControllerSequence::AnimState eState, float fSpinner);

    // Returns the sequence's weight at the engine's spinner (ease, or transition: the weight divided by the sequence weight)
    float GetWeight(float fSpinner) const;
    // Switches to another curve, keeping the weight the ease has at the spinner (at the matching point of the new curve)
    void SetCurve(const BlendCurve& newCurve, float fSpinner);
};

namespace Eases
{
    // EASEIN, TRANSDEST: the spinner rises to 1
    bool IsRising(NiControllerSequence::AnimState eState);
    // Rising states, or EASEOUT, TRANSSOURCE: the spinner falls to 0
    bool IsEasing(NiControllerSequence::AnimState eState);

    // Returns the engine's spinner of the ease the sequence is in at its owner's time (0 or 1 if not yet updated)
    float GetSpinner(const NiControllerSequence& sequence, float fTime);

    // Returns the record of the ease the sequence is in; null if it is not easing or no record exists yet
    Ease* Find(const NiControllerSequence& sequence);
    // Like Find for a sequence that is easing, but creates the record at the engine's spinner if there is none
    Ease& Get(SequenceExtraData& extraData, const NiControllerSequence& sequence, float fSpinner);

    // Called when the sequence is about to turn around at the engine's spinner. ActivateNoReset and DeactivateNoReset
    // mirror the window, so the spinner of the ease into eNewState starts at the same value. That ease starts with the
    // weight the sequence has there, on the same curve until its switch is resolved.
    void Reverse(const NiControllerSequence& sequence, NiControllerSequence::AnimState eNewState, float fSpinner);

    void WriteHooks();
}
