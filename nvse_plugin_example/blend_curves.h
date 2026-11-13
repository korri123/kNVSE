#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "NiNodes.h"

// How a blend between animations moves over its time. The engine moves ease and transition spinners at constant speed;
// every spinner of every blend passes through BlendCurves::Shape (NiControllerSequence::SetInterpsWeightAndTime), which
// shapes it with fBlendEaseCurve or with a curve an animation names in a text key:
//   "blendCurve: <curve>"      blends into and out of this animation
//   "blendInCurve: <curve>"    blends into it
//   "blendOutCurve: <curve>"   blends out of it
// <curve>: default (fBlendEaseCurve), linear, smooth (smoothstep), in/out/inOut + Sine, Quad, Cubic, Quart, Expo or Circ
// (outSine: fast at the start of the blend, slow at its end), or "bezier x1 y1 x2 y2" (CSS cubic-bezier, each in 0..1;
// commas allowed between them). Curves that overshoot (back, elastic, bounce) would weigh bones outside 0..1 and are
// not offered.
// Each key holds from its time until the next key for the same direction: a blend takes the key in effect at the time
// the animation is at when the blend starts (a blendOutCurve late in a reload applies when the reload ends, not when it
// is cut early). A blendInCurve or blendOutCurve wins over a blendCurve at the same time.
//
// The curve is the progress of a switch of animations (AnimSwitch), not of one animation: every sequence of a switch
// takes the same curve, so the ones blending in and the ones blending out stay complementary (in + out = 1). A switch's
// curve: the blendInCurve of a sequence it blends in, else the blendOutCurve of one it blends out, else the default. An
// ease that turns around (ActivateNoReset/DeactivateNoReset: aim tapped twice), comes back (PoseBlendFixes) or whose
// switch takes another curve (a sequence with a key joined it later in the update) continues from the weight it had, at
// the point of the new curve that has it, and still ends when the engine said.
// With bBlendSmoothing a curve a key named is what the bone shows: Gamebryo gives a higher priority easing in over a
// lower one about s^2 / (s^2 + 1 - s) of its ease spinner s; with a named curve it takes s (BlendCurves::HasNamedCurve in
// ComputeNormalizedWeights and BlendTransitions).
// Until an animation with one of the keys is loaded, Shape is the default curve of the spinner and nothing else runs.
struct BlendCurve
{
    enum class Type : UInt8
    {
        Default, // fBlendEaseCurve
        Linear,
        Smooth,
        Sine,
        Quad,
        Cubic,
        Quart,
        Expo,
        Circ,
        Bezier,
    };
    enum class Mode : UInt8
    {
        In,
        Out,
        InOut,
    };

    Type type = Type::Default;
    Mode mode = Mode::InOut;
    // Bezier: the control points (x1, y1) and (x2, y2) between (0, 0) and (1, 1)
    float fX1 = 0.0f;
    float fY1 = 0.0f;
    float fX2 = 1.0f;
    float fY2 = 1.0f;

    // A text key named it: any curve but the default
    bool IsNamed() const { return type != Type::Default; }
    bool operator==(const BlendCurve&) const = default;

    // The share of the switch at a progress through its time, both 0..1: 0 at 0, 1 at 1, never decreasing
    float Evaluate(float fProgress) const;
    // The progress at which it reaches the share
    float Invert(float fShare) const;

    static std::optional<BlendCurve> Parse(std::string_view text);
};

namespace BlendCurves
{
    enum class Direction : UInt8
    {
        Both,
        In,
        Out,
    };

    struct Key
    {
        float fTime;
        Direction direction;
        BlendCurve curve;
    };
    // empty: the sequence has no curve keys
    using Keys = std::vector<Key>;

    // A sequence joins a controller manager (SequenceExtraDatas' hook): keeps its curve keys with it
    void OnSequenceJoin(NiControllerSequence& sequence);

    // The spinner (ease, or transition: the weight divided by the sequence weight) of the sequence's evaluation at the scaled
    // time, shaped by the curve of its ease (Ease, ease.h)
    float Shape(NiControllerSequence& sequence, float fSpinner, float fScaledTime);

    // Its ease is shaped by a curve a text key named: the bone shows that curve (see above)
    bool HasNamedCurve(const NiControllerSequence& sequence);
}
