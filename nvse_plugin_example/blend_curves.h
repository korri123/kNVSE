#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "NiNodes.h"

// Shapes how a blend between animations progresses over its time. The engine moves ease and transition spinners at
// constant speed. Every spinner passes through BlendCurves::Shape (called from
// NiControllerSequence::SetInterpsWeightAndTime), which either returns it unchanged (the default curve) or applies a
// curve that an animation names in a text key:
//   "blendCurve: <curve>"      blends into and out of this animation
//   "blendInCurve: <curve>"    blends into it
//   "blendOutCurve: <curve>"   blends out of it
// <curve> is one of:
//   default                    the game's curve
//   linear, smooth             smooth is smoothstep
//   in/out/inOut + Sine, Quad, Cubic, Quart, Expo or Circ (outSine is fast at the start of the blend, slow at its end)
//   "bezier x1 y1 x2 y2"       CSS cubic-bezier, each value in 0..1, commas allowed between them
// Curves that overshoot (back, elastic, bounce) would give bones a weight outside 0..1 and are not offered.
// A key holds from its time until the next key for the same direction. A blend uses the key in effect at the animation
// time when the blend starts, so a blendOutCurve late in a reload applies when the reload ends, not when it is cut
// short. A blendInCurve or blendOutCurve wins over a blendCurve at the same time.
//
// The curve belongs to a switch of animations (AnimSwitch), not to one animation. Every sequence of a switch uses the
// same curve, so the sequences blending in and out stay complementary (in + out = 1). A switch uses the blendInCurve
// of a sequence it blends in, else the blendOutCurve of one it blends out, else the default.
// An ease continues from the weight it already has, at the matching point of the new curve, and still ends when the
// engine says it ends, if it
//   - turns around (ActivateNoReset/DeactivateNoReset, e.g. aim tapped twice),
//   - comes back (PoseBlendFixes), or
//   - gets another curve because a sequence with a key joined its switch later in the update.
// With bBlendSmoothing, a named curve is what the bone shows. Gamebryo gives a higher priority that is easing in
// about s^2 / (s^2 + 1 - s) of its ease spinner s over a lower one, while a named curve gives it s (see
// BlendCurves::HasNamedCurve in ComputeNormalizedWeights and BlendTransitions).
// Until an animation with one of the keys is loaded, Shape returns the spinner unchanged and does nothing else.
struct BlendCurve
{
    enum class Type : UInt8
    {
        Default, // the engine's spinner as is; not a named curve, so Gamebryo's share across priorities applies
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
    // Bezier: control points (x1, y1) and (x2, y2) between (0, 0) and (1, 1)
    float fX1 = 0.0f;
    float fY1 = 0.0f;
    float fX2 = 1.0f;
    float fY2 = 1.0f;

    // True if a text key named this curve (any curve but the default)
    bool IsNamed() const { return type != Type::Default; }
    bool operator==(const BlendCurve&) const = default;

    // Returns the share of the switch (0..1) at a progress through its time (0..1). 0 at 0, 1 at 1, never decreasing
    float Evaluate(float fProgress) const;
    // Returns the progress at which the curve reaches the share
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
    // Empty if the sequence has no curve keys
    using Keys = std::vector<Key>;

    // Called when a sequence joins a controller manager (from SequenceExtraDatas' hook); keeps its curve keys with it
    void OnSequenceJoin(NiControllerSequence& sequence);
    // SetAnimTextKeys replaced the keys of the sequence's KF (every clone of it shares them): each sequence reads its
    // curve keys again when it next resolves a curve
    void OnTextKeysChanged(const NiControllerSequence& sequence);

    // Shapes the spinner of the sequence's evaluation at the scaled time (ease, or transition: the weight divided by
    // the sequence weight) with the curve of its ease (see ease.h)
    float Shape(NiControllerSequence& sequence, float fSpinner, float fScaledTime);

    // True if the sequence's ease is shaped by a curve a text key named, so the bone shows that curve (see above)
    bool HasNamedCurve(const NiControllerSequence& sequence);
}
