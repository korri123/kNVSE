#pragma once

#include "blend_curves.h"
#include "NiNodes.h"

class SequenceExtraData;

// One ease of a sequence: the time it spends in EASEIN, EASEOUT, TRANSDEST or TRANSSOURCE after something eased it
// there. The engine has no such thing, only the sequence's state and the window its spinner runs over (m_fStartTime,
// m_fEndTime), and whatever eases a sequence rewrites those. What kNVSE knows about an ease is kept in this record, in
// the sequence's SequenceExtraData:
//  - the switch of animations that eased the sequence (AnimSwitch)
//  - the curve that switch runs on and where on it this ease started (BlendCurves)
//  - whether the sequence continues a variant of its group (BlendFixes)
// The record is the sequence's ease while the sequence is in the state it was begun for (Eases::Find). It is dropped
// where the engine starts another one: when the sequence is activated (AttachInterpolatorsHooked: Activate, StartBlend,
// StartMorph) and when it is eased out (the Deactivate hooks). kNVSE's own changes of an ease carry it over: turning
// around (Eases::Reverse, from ActivateNoReset / DeactivateNoReset), PoseBlendFixes putting back the ease a hard cut
// interrupted. An ease nothing has a record of is begun by the first to ask (Eases::Get), where it is by then: until
// then it ran on the default curve, which needs no record.
struct Ease
{
    // the state the sequence was eased into; INACTIVE: no ease
    NiControllerSequence::AnimState eState = NiControllerSequence::INACTIVE;
    // the AnimSwitch that eased it; 0: nothing declared the ease
    UInt32 uiSwitch = 0;
    // Where it starts: the engine's spinner there (0 easing in, 1 easing out; in between for one that continues another
    // ease, or that was asked for when under way) and how far along its curve that is
    float fFromSpinner = 0.0f;
    float fFromProgress = 0.0f;
    // the engine's spinner of its last evaluation (BlendCurves::Shape); where it starts before the first
    float fLastSpinner = 0.0f;
    // The curve it runs on, and the curve of its switch as last resolved (BlendCurves::Shape moves the ease to that one).
    // Not resolved yet for a sequence new to its switch
    BlendCurve curve;
    BlendCurve switchCurve;
    bool bCurveResolved = false;
    // Cross faded in from another variant of its group at the same point (BlendFixes' TransitionToVariant): it continues
    // that sequence, so Animation::Update acts on its group's keys and scales its clock while it eases in, as for the
    // ANIMATING sequence it replaced
    bool bContinuesVariant = false;
    // An ease-out that makes way for the sequences its switch eases in: it lasts until they are in (AnimSwitch::TakeOver)
    bool bMakesWay = false;

    // An ease into the state that is at the spinner, on the default curve
    static Ease At(NiControllerSequence::AnimState eState, float fSpinner);

    // The sequence's weight at the engine's spinner (ease, or transition: the weight divided by the sequence weight)
    float GetWeight(float fSpinner) const;
    // Goes on from the weight it has at the spinner on another curve, at the point of that curve that has the weight
    void SetCurve(const BlendCurve& newCurve, float fSpinner);
};

namespace Eases
{
    // EASEIN, TRANSDEST: the spinner rises to 1
    bool IsRising(NiControllerSequence::AnimState eState);
    // rising, or EASEOUT, TRANSSOURCE: the spinner falls to 0
    bool IsEasing(NiControllerSequence::AnimState eState);

    // The engine's spinner of the ease the sequence is in at its owner's time (0 or 1 while it has not been updated)
    float GetSpinner(const NiControllerSequence& sequence, float fTime);

    // The record of the ease the sequence is in; null if it is not easing or nothing has asked for one
    Ease* Find(const NiControllerSequence& sequence);
    // The same for a sequence that is easing, begun at the engine's spinner if there was none
    Ease& Get(SequenceExtraData& extraData, const NiControllerSequence& sequence, float fSpinner);

    // The sequence is about to turn around at the engine's spinner (ActivateNoReset and DeactivateNoReset mirror the
    // window, so the spinner of the ease into eNewState starts at the same value): that ease starts with the weight the
    // sequence has there, on the same curve until its switch is resolved
    void Reverse(const NiControllerSequence& sequence, NiControllerSequence::AnimState eNewState, float fSpinner);

    void WriteHooks();
}
