#include "blend_curves.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <string>
#include <utility>

#include "hooks.h"
#include "sequence_extradata.h"
#include "string_view_util.h"
#include "text_key_arguments.h"
#include "utility.h"

using BlendCurves::Direction;

namespace
{
    constexpr std::pair<std::string_view, Direction> kKeyNames[] = {
        { "blendCurve", Direction::Both }, { "blendInCurve", Direction::In }, { "blendOutCurve", Direction::Out },
    };

    // Bisection steps: 2^-24 of the range, below a float's resolution near 1
    constexpr int kSteps = 24;

    // Where in 0..1 a function that never decreases reaches the value
    template <typename F>
    float FindWhere(F&& function, float fValue)
    {
        float fLow = 0.0f;
        float fHigh = 1.0f;
        for (int i = 0; i < kSteps; ++i)
        {
            const float fMid = (fLow + fHigh) * 0.5f;
            (function(fMid) < fValue ? fLow : fHigh) = fMid;
        }
        return (fLow + fHigh) * 0.5f;
    }

    // Set once a sequence with curve keys joined a manager: until then every ease takes the default curve
    std::atomic<bool> g_anyKeys = false;

    void LogOnce(const std::string& message)
    {
        ERROR_LOG_ONCE("BlendCurves: " + message);
    }

    float Smoothstep(float x)
    {
        return x * x * (3.0f - 2.0f * x);
    }

    // A family's In form: slow at the start, fast at the end
    float EaseIn(BlendCurve::Type type, float x)
    {
        switch (type)
        {
        case BlendCurve::Type::Sine:
            return 1.0f - std::cos(x * 1.57079633f);
        case BlendCurve::Type::Quad:
            return x * x;
        case BlendCurve::Type::Cubic:
            return x * x * x;
        case BlendCurve::Type::Quart:
            return x * x * x * x;
        case BlendCurve::Type::Expo:
        {
            // 2^(10x - 10) starts at 2^-10: shifted and scaled to start at 0
            constexpr float kStart = 1.0f / 1024.0f;
            return (std::exp2(10.0f * x - 10.0f) - kStart) / (1.0f - kStart);
        }
        case BlendCurve::Type::Circ:
            return 1.0f - std::sqrt(max(1.0f - x * x, 0.0f));
        default:
            return x;
        }
    }

    // A coordinate of the cubic Bezier from 0 to 1 with control values p1, p2
    float Bezier(float t, float p1, float p2)
    {
        const float s = 1.0f - t;
        return 3.0f * s * s * t * p1 + 3.0f * s * t * t * p2 + t * t * t;
    }

    BlendCurves::Keys ParseKeys(const NiControllerSequence& sequence)
    {
        BlendCurves::Keys keys;
        for (const NiTextKey& key : sequence.m_spTextKeys->GetKeys())
        {
            const std::string_view text = key.m_kText.Str();
            for (const auto& [name, direction] : kKeyNames)
            {
                const auto arguments = TextKeys::GetArguments(text, name);
                if (!arguments)
                    continue;
                if (const auto curve = BlendCurve::Parse(*arguments))
                    keys.push_back({ key.m_fTime, direction, *curve });
                else
                    LogOnce(FormatString("%s: can't use \"%s\" (expected %.*s: default, linear, smooth, in/out/inOut + Sine, "
                        "Quad, Cubic, Quart, Expo or Circ, or bezier x1 y1 x2 y2 with each in 0..1)", sequence.m_kName.CStr(),
                        key.m_kText.CStr(), static_cast<int>(name.size()), name.data()));
                break;
            }
        }
        return keys;
    }

    // The key for the direction in effect at the time: the latest at or before it, a directional one over a blendCurve at
    // the same time
    const BlendCurve* FindKey(const BlendCurves::Keys& keys, Direction direction, float fTime)
    {
        const BlendCurves::Key* found = nullptr;
        for (const auto& key : keys)
        {
            if (key.fTime > fTime || (key.direction != Direction::Both && key.direction != direction))
                continue;
            if (found && (key.fTime < found->fTime ||
                (key.fTime == found->fTime && found->direction != Direction::Both && key.direction == Direction::Both)))
                continue;
            found = &key;
        }
        return found ? &found->curve : nullptr;
    }

    // Resolves the curve of the ease's switch (just its own for an ease no switch declared) and gives it to every
    // sequence in the switch. Sequences keep joining a switch after its first ones were shaped (Animation::PlayGroup
    // updates the bones after each group it starts: aim, then aim up and aim down; a gait after the idle of a stance
    // change; BlendFixes' variants join from kMessage_MainGameLoop) and their keys count as well: each one new to it
    // resolves it again, and Shape moves the eases already in it to the curve if that changed it. fScaledTime: the time
    // the sequence is evaluated at
    void Resolve(const NiControllerSequence& sequence, const Ease& ease, kAnimationExtraData& nodeData, float fScaledTime)
    {
        const auto memberEase = [&](kAnimationExtraData::Entry& entry) -> Ease*
        {
            Ease& other = entry.data.ease;
            if (entry.manager != sequence.m_pkOwner || other.eState != entry.sequence->m_eState)
                return nullptr;
            return &other == &ease || (ease.uiSwitch && other.uiSwitch == ease.uiSwitch) ? &other : nullptr;
        };
        const auto find = [&](Direction direction) -> const BlendCurve*
        {
            for (auto& entry : nodeData.entries)
            {
                if (entry.data.blendCurveKeys.empty() || !memberEase(entry))
                    continue;
                if (Eases::IsRising(entry.sequence->m_eState) != (direction == Direction::In))
                    continue;
                const float fTime = entry.sequence == &sequence ? fScaledTime : entry.sequence->GetSequenceTime();
                if (const auto* curve = FindKey(entry.data.blendCurveKeys, direction, fTime))
                    return curve;
            }
            return nullptr;
        };
        const BlendCurve* found = find(Direction::In);
        if (!found)
            found = find(Direction::Out);
        const BlendCurve curve = found ? *found : BlendCurve();
        for (auto& entry : nodeData.entries)
        {
            if (Ease* member = memberEase(entry))
            {
                member->switchCurve = curve;
                member->bCurveResolved = true;
            }
        }
    }
}

float BlendCurve::Evaluate(float fProgress) const
{
    if (fProgress <= 0.0f)
        return 0.0f;
    if (fProgress >= 1.0f)
        return 1.0f;
    const float x = fProgress;
    switch (type)
    {
    case Type::Default:
        return x + (Smoothstep(x) - x) * g_pluginSettings.blendEaseCurve;
    case Type::Linear:
        return x;
    case Type::Smooth:
        return Smoothstep(x);
    case Type::Bezier:
        // with control points in 0..1 neither coordinate decreases: the parameter where x is reached
        return Bezier(FindWhere([&](float t) { return Bezier(t, fX1, fX2); }, x), fY1, fY2);
    default:
        switch (mode)
        {
        case Mode::In:
            return EaseIn(type, x);
        case Mode::Out:
            return 1.0f - EaseIn(type, 1.0f - x);
        default:
            return x < 0.5f ? EaseIn(type, 2.0f * x) * 0.5f : 1.0f - EaseIn(type, 2.0f - 2.0f * x) * 0.5f;
        }
    }
}

float BlendCurve::Invert(float fShare) const
{
    if (fShare <= 0.0f)
        return 0.0f;
    if (fShare >= 1.0f)
        return 1.0f;
    return FindWhere([&](float fProgress) { return Evaluate(fProgress); }, fShare);
}

std::optional<BlendCurve> BlendCurve::Parse(std::string_view text)
{
    constexpr std::string_view kSeparators = " \t,";
    const auto nameEnd = text.find_first_of(kSeparators);
    const auto name = text.substr(0, nameEnd);
    std::string_view rest = nameEnd == std::string_view::npos ? std::string_view() : text.substr(nameEnd);

    BlendCurve curve;
    if (sv::equals_ci(name, "bezier"))
    {
        float values[4];
        unsigned int count = 0;
        while (true)
        {
            const auto first = rest.find_first_not_of(kSeparators);
            if (first == std::string_view::npos)
                break;
            rest.remove_prefix(first);
            const auto token = rest.substr(0, rest.find_first_of(kSeparators));
            rest.remove_prefix(token.size());
            float value;
            const auto* tokenEnd = token.data() + token.size();
            const auto [ptr, error] = std::from_chars(token.data(), tokenEnd, value);
            if (count == 4 || error != std::errc() || ptr != tokenEnd || !(value >= 0.0f && value <= 1.0f))
                return std::nullopt;
            values[count++] = value;
        }
        if (count != 4)
            return std::nullopt;
        curve.type = Type::Bezier;
        curve.fX1 = values[0];
        curve.fY1 = values[1];
        curve.fX2 = values[2];
        curve.fY2 = values[3];
        return curve;
    }
    if (rest.find_first_not_of(kSeparators) != std::string_view::npos)
        return std::nullopt;

    constexpr std::pair<std::string_view, Type> kPlain[] = {
        { "default", Type::Default }, { "linear", Type::Linear }, { "smooth", Type::Smooth },
    };
    for (const auto& [plainName, type] : kPlain)
    {
        if (sv::equals_ci(name, plainName))
        {
            curve.type = type;
            return curve;
        }
    }
    // inOut before in: it starts with it
    constexpr std::pair<std::string_view, Mode> kModes[] = {
        { "inOut", Mode::InOut }, { "in", Mode::In }, { "out", Mode::Out },
    };
    constexpr std::pair<std::string_view, Type> kFamilies[] = {
        { "Sine", Type::Sine }, { "Quad", Type::Quad }, { "Cubic", Type::Cubic }, { "Quart", Type::Quart },
        { "Expo", Type::Expo }, { "Circ", Type::Circ },
    };
    for (const auto& [modeName, mode] : kModes)
    {
        if (!sv::starts_with_ci(name, modeName))
            continue;
        const auto family = name.substr(modeName.size());
        for (const auto& [familyName, type] : kFamilies)
        {
            if (sv::equals_ci(family, familyName))
            {
                curve.type = type;
                curve.mode = mode;
                return curve;
            }
        }
    }
    return std::nullopt;
}

void BlendCurves::OnSequenceJoin(NiControllerSequence& sequence)
{
    if (!sequence.m_spTextKeys)
        return;
    auto keys = ParseKeys(sequence);
    if (keys.empty())
        return;
    if (auto* extraData = SequenceExtraDatas::GetOrCreate(&sequence))
    {
        extraData->blendCurveKeys = std::move(keys);
        g_anyKeys.store(true, std::memory_order_relaxed);
    }
}

float BlendCurves::Shape(NiControllerSequence& sequence, float fSpinner, float fScaledTime)
{
    if (!Eases::IsEasing(sequence.m_eState))
        return fSpinner;
    // No animation has named a curve: every ease is on the default one, which is point symmetric (1 - curve(x) ==
    // curve(1 - x)). A cross fade's two spinners then add up to 1 and an ease turned around where it is (ActivateNoReset,
    // DeactivateNoReset mirror its window) continues from the spinner it had: the weight is the curve of the spinner. The
    // same for a sequence kNVSE has no data of (no switch, no keys)
    auto* extraData = g_anyKeys.load(std::memory_order_relaxed) ? SequenceExtraDatas::Get(&sequence) : nullptr;
    if (!extraData)
        return BlendCurve().Evaluate(fSpinner);

    Ease& ease = Eases::Get(*extraData, sequence, fSpinner);
    if (!ease.bCurveResolved || ease.switchCurve.IsNamed())
    {
        // extraData is part of the data of the manager's target
        auto& nodeData = *kAnimationExtraData::Get(sequence.m_pkOwner->m_pkTarget);
        if (!ease.bCurveResolved)
            Resolve(sequence, ease, nodeData, fScaledTime);
        if (ease.switchCurve.IsNamed())
            nodeData.namedCurveEaseEnd = max(nodeData.namedCurveEaseEnd, sequence.m_fEndTime);
    }
    // a sequence that joined the switch changed its curve, or this one is new to it: the curve it had ends with the weight
    // last written
    if (ease.curve != ease.switchCurve)
        ease.SetCurve(ease.switchCurve, ease.fLastSpinner);
    ease.fLastSpinner = fSpinner;
    return ease.GetWeight(fSpinner);
}

bool BlendCurves::HasNamedCurve(const NiControllerSequence& sequence)
{
    if (!g_anyKeys.load(std::memory_order_relaxed))
        return false;
    // asked for every bone: most sequences are ruled out by their node's data alone, without looking up their own
    const auto* manager = sequence.m_pkOwner;
    const auto* nodeData = manager && manager->m_pkTarget ? kAnimationExtraData::Get(manager->m_pkTarget) : nullptr;
    if (!nodeData || sequence.m_fEndTime > nodeData->namedCurveEaseEnd)
        return false;
    const Ease* ease = Eases::Find(sequence);
    return ease && ease->curve.IsNamed();
}
