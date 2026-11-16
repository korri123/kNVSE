#include "weapon_hand_blend.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>

#include "bone_evaluation.h"
#include "blend_smoothing.h"
#include "hooks.h"
#include "NiObjects.h"
#include "string_view_util.h"
#include "text_key_arguments.h"
#include "utility.h"
#include "transform_math.h"
#include "weapon_parent_switch.h"

using WeaponHandBlend::Acceptance;
using WeaponHandBlend::ArmReach;
using WeaponHandBlend::ChainLocals;
using WeaponHandBlend::HandChains;
using WeaponHandBlend::ItemPose;
using WeaponHandBlend::SequenceInfo;
using namespace TransformMath;

namespace
{
    // Classification sampling: rate in Hz and the maximum number of samples per sequence
    constexpr float kSampleRate = 30.0f;
    constexpr unsigned int kMaxSamples = 512;
    // Half width of the window over which motion relative to a reference is accumulated (seconds)
    constexpr float kMotionWindowHalf = 0.1f;
    // Half width of the box filter applied to the acceptance curves (seconds)
    constexpr float kSmoothingHalf = 0.05f;
    // Converts a rotation difference (radians) into game units so it can be added to a translation
    constexpr float kLeverArm = 10.0f;
    // Below this accumulated motion over a window, the weapon is rigid relative to a reference
    constexpr float kMinWindowMotion = 0.75f;
    // A hand is fully accepted while the weapon moves no more relative to it than relative to the steadiest reference
    // (or kMinWindowMotion), and not at all once it moves kLooseRatio times as much
    constexpr float kLooseRatio = 2.0f;
    // If neither hand moves relative to the weapon, a hand this far (game units) from the right hand is not holding it
    constexpr float kFarFromRightHand = 15.0f;
    constexpr float kMinAcceptance = 0.001f;
    // Sharpens agreement: a hand accepted by only part of the blend's weight quickly yields to the next reference
    constexpr int kAgreementExponent = 8;
    constexpr float kMinReferenceWeight = 1e-4f;
    // The left hand touches the weapon while within this distance outside the weapon model's bounding sphere (game units)
    constexpr float kContactMargin = 4.0f;
    // A smaller bound means no weapon model is attached; contact is then not tested
    constexpr float kMinWeaponRadius = 1.0f;
    // Key gaps longer than this (below 20 fps) let a counter-animated weapon visibly drift between keys
    constexpr float kSparseKeyInterval = 1.5f / kSampleRate;
    constexpr unsigned int kMaxLeftSpaceKeys = 2048;
    constexpr float kSameKeyTime = 1e-4f;

    enum Reference { kRight, kLeft, kBody, kReferenceCount };

    const NiFixedString& RightHandName()
    {
        static const NiFixedString name("Bip01 R Hand");
        return name;
    }

    const NiFixedString& LeftHandName()
    {
        static const NiFixedString name("Bip01 L Hand");
        return name;
    }

    const NiFixedString& WeaponName()
    {
        static const NiFixedString name("Weapon");
        return name;
    }

    float RotationAngle(const NiMatrix33& a, const NiMatrix33& b)
    {
        // trace(a^T * b) = 1 + 2 cos(angle)
        float trace = 0.0f;
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col)
                trace += a.m_pEntry[row][col] * b.m_pEntry[row][col];
        const float cosAngle = std::clamp((trace - 1.0f) * 0.5f, -1.0f, 1.0f);
        return std::acos(cosAngle);
    }

    float Distance(const NiTransform& a, const NiTransform& b)
    {
        return (a.m_Translate - b.m_Translate).Length() + kLeverArm * RotationAngle(a.m_Rotate, b.m_Rotate);
    }

    bool IsFinite(const NiTransform& transform)
    {
        if (!std::isfinite(transform.m_fScale) || !std::isfinite(transform.m_Translate.x) || !std::isfinite(transform.m_Translate.y)
            || !std::isfinite(transform.m_Translate.z))
            return false;
        for (const auto& row : transform.m_Rotate.m_pEntry)
            for (const float entry : row)
                if (!std::isfinite(entry))
                    return false;
        return true;
    }

    // An interpolator's value as a transform. Channels it does not provide come from the node's current local transform.
    NiTransform ToTransform(const NiQuatTransform& value, const NiAVObject* node)
    {
        NiTransform result = CurrentLocal(node);
        if (value.IsTranslateValid())
            result.m_Translate = value.GetTranslate();
        if (value.IsRotateValid())
            ToUnitRotation(value.GetRotate(), result.m_Rotate);
        if (value.IsScaleValid())
            result.m_fScale = value.GetScale();
        return result;
    }

    // Local transform of a node as animated by one of a sequence's interpolators. Without one (the sequence does not
    // animate the node), the node's current (blended) local transform.
    NiTransform SampleLocal(NiInterpolator* interpolator, NiAVObject* node, float fTime, bool bInvalidateCache)
    {
        if (!interpolator)
            return CurrentLocal(node);
        NiQuatTransform value;
        if (!interpolator->Update(fTime, node, value))
            value.MakeInvalid();
        if (bInvalidateCache)
        {
            // The interpolator caches its last evaluation by time, so sampling at other times must not leave it stale
            interpolator->m_fLastTime = -NI_INFINITY;
        }
        return ToTransform(value, node);
    }

    NiTransform SampleChain(NiAVObject* const* nodes, const NiPointer<NiInterpolator>* interpolators, unsigned int count, float fTime,
                            bool bInvalidateCache)
    {
        NiTransform result = IdentityTransform();
        for (unsigned int i = 0; i < count; ++i)
            result = result * SampleLocal(interpolators[i], nodes[i], fTime, bInvalidateCache);
        return result;
    }

    // The hand chains' nodes as the sequence poses them at a time
    void SampleLocals(const SequenceInfo& info, const HandChains& chains, float fTime, ChainLocals& locals)
    {
        for (unsigned int i = 0; i < chains.rightCount; ++i)
            locals.right[i] = SampleLocal(info.rightInterpolators[i], chains.right[i], fTime, false);
        for (unsigned int i = 0; i < chains.leftCount; ++i)
            locals.left[i] = SampleLocal(info.leftInterpolators[i], chains.left[i], fTime, false);
    }

    // The hand chains' nodes as they are: this pass's blend (Context::RequireChains)
    void CurrentLocals(const HandChains& chains, ChainLocals& locals)
    {
        for (unsigned int i = 0; i < chains.rightCount; ++i)
            locals.right[i] = CurrentLocal(chains.right[i]);
        for (unsigned int i = 0; i < chains.leftCount; ++i)
            locals.left[i] = CurrentLocal(chains.left[i]);
    }

    // The last of a chain's first count nodes relative to the chain's top
    NiTransform Compose(const NiTransform* locals, unsigned int count)
    {
        NiTransform result = IdentityTransform();
        for (unsigned int i = 0; i < count; ++i)
            result = result * locals[i];
        return result;
    }

    NiInterpolator* FindInterpolator(const NiControllerSequence& sequence, const NiAVObject* node)
    {
        const auto* block = sequence.GetControlledBlock(node->m_pcName);
        return block ? block->m_spInterpolator.data : nullptr;
    }

    bool CollectChain(NiAVObject* hand, const NiAVObject* commonAncestor, NiAVObject** chain, unsigned int& count)
    {
        NiAVObject* reversed[WeaponHandBlend::kMaxChainLength];
        unsigned int length = 0;
        for (NiAVObject* node = hand; node != commonAncestor; node = node->m_pkParent)
        {
            if (!node || length == WeaponHandBlend::kMaxChainLength)
                return false;
            reversed[length++] = node;
        }
        for (unsigned int i = 0; i < length; ++i)
            chain[i] = reversed[length - 1 - i];
        count = length;
        return length != 0;
    }

    bool BuildChains(NiAVObject* weapon, HandChains& chains)
    {
        NiAVObject* rightHand = weapon->m_pkParent;
        for (NiAVObject* ancestor = rightHand->m_pkParent; ancestor; ancestor = ancestor->m_pkParent)
        {
            if (NiAVObject* leftHand = ancestor->GetObjectByName(LeftHandName()))
            {
                return CollectChain(rightHand, ancestor, chains.right, chains.rightCount)
                    && CollectChain(leftHand, ancestor, chains.left, chains.leftCount);
            }
        }
        return false;
    }

    float Rigidity(float motion, float steadiest)
    {
        return std::clamp((kLooseRatio * steadiest - motion) / ((kLooseRatio - 1.0f) * steadiest), 0.0f, 1.0f);
    }

    NiTransform Orthonormal(NiTransform transform)
    {
        NiQuaternion rotation;
        rotation.FromRotation(transform.m_Rotate);
        ToUnitRotation(rotation, transform.m_Rotate);
        return transform;
    }

    // 1 while the left hand touches the weapon model's bounding sphere, falling to 0 at kContactMargin outside of it.
    // 1 when there is no model to test against.
    float Contact(const NiTransform& weaponRelativeToLeft, const WeaponHandBlend::WeaponBound& bound)
    {
        if (bound.radius < kMinWeaponRadius)
            return 1.0f;
        const NiPoint3 hand = Inverse(weaponRelativeToLeft).m_Translate; // left hand in Weapon node space
        const float outside = (hand - bound.center).Length() - bound.radius;
        if (!std::isfinite(outside))
            return 1.0f;
        return std::clamp(1.0f - outside / kContactMargin, 0.0f, 1.0f);
    }

    // "weaponSpace: right|left|body|auto|off" text keys set the space from their time until the next key. Auto (and the
    // time before the first key) leaves it to the measurement. Off keeps the regular blend for this animation's share.
    enum class Space : unsigned char { Auto, Right, Left, Body, Off };

    struct SpaceKey
    {
        float time;
        Space space;
    };

    // Scratch buffers for Measure, reused per thread because measuring runs on whichever thread blends the actor
    thread_local std::vector<NiTransform> t_relative[kReferenceCount];
    thread_local std::vector<float> t_motion[kReferenceCount];
    thread_local std::vector<float> t_contact;
    thread_local std::vector<Acceptance> t_raw;
    thread_local std::vector<Acceptance> t_smoothed;
    thread_local std::vector<SpaceKey> t_spaceKeys;
    thread_local std::vector<float> t_keyTimes;

    void CollectSpaceKeys(const NiControllerSequence& sequence)
    {
        t_spaceKeys.clear();
        if (!sequence.m_spTextKeys)
            return;
        for (const NiTextKey& key : sequence.m_spTextKeys->GetKeys())
        {
            const auto value = TextKeys::GetArguments(key.m_kText.Str(), "weaponSpace");
            if (!value || value->empty())
                continue;
            Space space;
            if (sv::equals_ci(*value, "right"))
                space = Space::Right;
            else if (sv::equals_ci(*value, "left"))
                space = Space::Left;
            else if (sv::equals_ci(*value, "body"))
                space = Space::Body;
            else if (sv::equals_ci(*value, "auto"))
                space = Space::Auto;
            else if (sv::equals_ci(*value, "off"))
                space = Space::Off;
            else
            {
                DebugPrint(FormatString("WeaponHandBlend: %s has an unknown text key \"%s\" (expected weaponSpace: right, left, body, auto or off)",
                    sequence.m_kName.CStr(), key.m_kText.CStr()));
                continue;
            }
            t_spaceKeys.push_back({ key.m_fTime, space });
        }
        std::ranges::stable_sort(t_spaceKeys, {}, &SpaceKey::time);
    }

    Space SpaceAt(float fTime)
    {
        Space space = Space::Auto;
        for (const SpaceKey& key : t_spaceKeys)
        {
            if (key.time > fTime + kSameKeyTime)
                break;
            space = key.space;
        }
        return space;
    }

    void CollectOffRanges(std::vector<WeaponHandBlend::TimeRange>& ranges)
    {
        ranges.clear();
        for (size_t i = 0; i < t_spaceKeys.size(); ++i)
        {
            if (t_spaceKeys[i].space != Space::Off)
                continue;
            size_t next = i + 1;
            while (next < t_spaceKeys.size() && t_spaceKeys[next].space == Space::Off)
                ++next;
            ranges.push_back({ t_spaceKeys[i].time, next < t_spaceKeys.size() ? t_spaceKeys[next].time : INFINITY });
            i = next;
        }
    }

    // Adds a keyframed track's key times. Returns false for a track without keys (B-spline compressed). Vanilla KFs
    // store most arm tracks and many Weapon tracks that way: a smooth curve fitted to densely sampled animation, which
    // drifts no more between its control points than a densely keyed track.
    bool AddKeyTimes(NiInterpolator* interpolator)
    {
        if (!interpolator)
            return true;
        auto* transform = NI_DYNAMIC_CAST(NiTransformInterpolator, interpolator);
        if (!transform)
            return false;
        unsigned int numKeys;
        NiAnimationKey::KeyType keyType;
        unsigned char keySize;
        if (auto* posData = transform->GetPosData(numKeys, keyType, keySize))
        {
            for (unsigned int i = 0; i < numKeys; ++i)
                t_keyTimes.push_back(posData->GetKeyAt(i, keySize)->m_fTime);
        }
        if (auto* rotData = transform->GetRotData(numKeys, keyType, keySize); rotData && numKeys)
        {
            if (keyType == NiAnimationKey::EULERKEY)
            {
                // An Euler key's own time is never set. Its three float tracks hold the timing.
                const auto* euler = static_cast<NiEulerRotKey*>(rotData->GetKeyAt(0, keySize));
                for (unsigned int axis = 0; axis < 3; ++axis)
                    for (unsigned int i = 0; i < euler->m_uiNumKeys[axis]; ++i)
                        t_keyTimes.push_back(euler->m_apkKeys[axis]->GetKeyAt(i, euler->m_ucSizes[axis])->m_fTime);
            }
            else
            {
                for (unsigned int i = 0; i < numKeys; ++i)
                    t_keyTimes.push_back(rotData->GetKeyAt(i, keySize)->m_fTime);
            }
        }
        return true;
    }

    // Computes the weapon relative to the left hand at every key time of the weapon and hand tracks. Between keys each
    // track interpolates on its own, so a weapon keyed relative to the right hand and a left hand keyed down its own
    // chain drift apart. Interpolating their relation instead keeps a counter-animated weapon on the left hand. The
    // result is exact at the keys.
    void BuildLeftSpaceKeys(SequenceInfo& info, NiAVObject* weapon, const HandChains& chains, float beginTime, float endTime)
    {
        auto& times = t_keyTimes;
        times.clear();
        // A track without keys counts as densely keyed. Using the begin and end time for all of its keys would make
        // the relation a spline between the end poses and move the weapon off the hand.
        bool keyed = AddKeyTimes(info.weaponInterpolator.data);
        for (unsigned int i = 0; i < chains.rightCount; ++i)
            keyed &= AddKeyTimes(info.rightInterpolators[i]);
        for (unsigned int i = 0; i < chains.leftCount; ++i)
            keyed &= AddKeyTimes(info.leftInterpolators[i]);
        if (!keyed)
            return;
        times.push_back(beginTime);
        times.push_back(endTime);
        std::erase_if(times, [&](float time) { return !(time >= beginTime && time <= endTime); });
        std::ranges::sort(times);
        times.erase(std::unique(times.begin(), times.end(), [](float a, float b) { return b - a < kSameKeyTime; }), times.end());

        // Keys this dense already keep the tracks together
        bool sparse = false;
        for (size_t i = 1; i < times.size(); ++i)
            sparse |= times[i] - times[i - 1] > kSparseKeyInterval;
        if (!sparse || times.size() > kMaxLeftSpaceKeys)
            return;

        info.leftSpaceKeys.reserve(times.size());
        for (const float time : times)
        {
            const NiTransform weaponLocal = SampleLocal(info.weaponInterpolator.data, weapon, time, true);
            const NiTransform rightHand = SampleChain(chains.right, info.rightInterpolators, chains.rightCount, time, true);
            const NiTransform leftHand = SampleChain(chains.left, info.leftInterpolators, chains.leftCount, time, true);
            const NiTransform relativeToLeft = Inverse(leftHand) * rightHand * weaponLocal;
            if (!IsFinite(relativeToLeft))
                continue; // a track that ends before the sequence does
            NiQuaternion rotation;
            rotation.FromRotation(relativeToLeft.m_Rotate);
            rotation.Normalize();
            float q[4] = { rotation.GetW(), rotation.GetX(), rotation.GetY(), rotation.GetZ() };
            if (!info.leftSpaceKeys.empty())
            {
                const float* previous = info.leftSpaceKeys.back().rotate;
                if (previous[0] * q[0] + previous[1] * q[1] + previous[2] * q[2] + previous[3] * q[3] < 0.0f)
                    for (float& component : q)
                        component = -component;
            }
            info.leftSpaceKeys.push_back({ time, relativeToLeft.m_Translate, { q[0], q[1], q[2], q[3] }, relativeToLeft.m_fScale });
        }
        if (info.leftSpaceKeys.size() < 2)
            info.leftSpaceKeys.clear();
    }

    // Measures how firmly each hand holds the weapon. A counter-animated weapon keeps a near-constant offset from the
    // left hand while its local transform (relative to the right hand) changes. A weapon held in the right hand shows
    // the opposite, and one held with both hands neither. Motion relative to the body (the hands' common ancestor) is
    // measured too, so a weapon moving on its own is not attributed to the hand it moves least relative to. A still
    // pose is rigid relative to everything, so there the left hand counts as holding the weapon only if it touches the
    // weapon model.
    void Measure(SequenceInfo& info, const NiControllerSequence& sequence, NiAVObject* weapon, const HandChains& chains,
                 const WeaponHandBlend::WeaponBound& bound)
    {
        info.acceptance.clear();
        info.leftSpaceKeys.clear();
        // Read first: an animation without left hand tracks can opt out as well
        CollectSpaceKeys(sequence);
        CollectOffRanges(info.offRanges);
        bool animatesLeftHand = false;
        for (unsigned int i = 0; i < chains.rightCount; ++i)
            info.rightInterpolators[i] = FindInterpolator(sequence, chains.right[i]);
        for (unsigned int i = 0; i < chains.leftCount; ++i)
        {
            info.leftInterpolators[i] = FindInterpolator(sequence, chains.left[i]);
            animatesLeftHand |= info.leftInterpolators[i] != nullptr;
        }
        const float beginTime = sequence.m_fBeginKeyTime;
        const float endTime = sequence.m_fEndKeyTime;
        if (!animatesLeftHand || !(endTime > beginTime))
            return;

        const auto sampleCount = std::clamp(static_cast<unsigned int>(std::ceil((endTime - beginTime) * kSampleRate)) + 1, 2u, kMaxSamples);
        const float step = (endTime - beginTime) / static_cast<float>(sampleCount - 1);
        // begin + step * i can overshoot the end key time by a rounding error, and the engine's rotation key search
        // reads past the key array for times beyond a track's last key
        const auto sampleTime = [&](unsigned int i) { return i + 1 == sampleCount ? endTime : beginTime + step * static_cast<float>(i); };

        auto& relative = t_relative;
        for (auto& samples : relative)
            samples.resize(sampleCount);
        auto& contact = t_contact;
        contact.resize(sampleCount);
        for (unsigned int i = 0; i < sampleCount; ++i)
        {
            const float time = sampleTime(i);
            const NiTransform weaponLocal = SampleLocal(info.weaponInterpolator.data, weapon, time, true);
            const NiTransform rightHand = SampleChain(chains.right, info.rightInterpolators, chains.rightCount, time, true);
            const NiTransform leftHand = SampleChain(chains.left, info.leftInterpolators, chains.leftCount, time, true);
            relative[kRight][i] = weaponLocal;
            relative[kBody][i] = rightHand * weaponLocal;
            relative[kLeft][i] = Inverse(leftHand) * relative[kBody][i];
            contact[i] = Contact(relative[kLeft][i], bound);
        }

        // Prefix sums of per-step motion relative to each reference
        auto& motion = t_motion;
        for (auto& sums : motion)
            sums.assign(sampleCount, 0.0f);
        for (unsigned int i = 1; i < sampleCount; ++i)
        {
            float steps[kReferenceCount];
            bool finite = true;
            for (int reference = 0; reference < kReferenceCount; ++reference)
            {
                steps[reference] = Distance(relative[reference][i - 1], relative[reference][i]);
                finite &= std::isfinite(steps[reference]);
            }
            // A non-finite sample (a track that ends before the sequence does) contributes no motion
            for (int reference = 0; reference < kReferenceCount; ++reference)
                motion[reference][i] = motion[reference][i - 1] + (finite ? steps[reference] : 0.0f);
        }

        const auto windowHalf = (std::max)(1u, static_cast<unsigned int>(std::lround(kMotionWindowHalf / step)));
        auto& raw = t_raw;
        raw.resize(sampleCount);
#if _DEBUG
        unsigned int notTouching = 0;
        unsigned int explicitSamples = 0;
#endif
        for (unsigned int i = 0; i < sampleCount; ++i)
        {
            const unsigned int low = i > windowHalf ? i - windowHalf : 0;
            const unsigned int high = (std::min)(sampleCount - 1, i + windowHalf);
            const float right = motion[kRight][high] - motion[kRight][low];
            const float left = motion[kLeft][high] - motion[kLeft][low];
            const float body = motion[kBody][high] - motion[kBody][low];
            const float steadiest = (std::max)(kMinWindowMotion, (std::min)({ right, left, body }));
            raw[i] = { Rigidity(right, steadiest), Rigidity(left, steadiest) };
            // Rigid relative to both hands, so motion cannot tell them apart. A weapon far from the right hand is held
            // by the left hand if that touches it. One touching neither (floating, e.g. a levitation gag) stays with
            // the right hand, which keeps the regular blend.
            if (right < kMinWindowMotion && left < kMinWindowMotion && relative[kRight][i].m_Translate.Length() > kFarFromRightHand)
                raw[i].right = 1.0f - contact[i];
            // A left hand that only stays still next to a weapon the right hand holds is not holding it
            if (raw[i].right > 0.0f)
            {
#if _DEBUG
                notTouching += raw[i].left >= 0.5f && raw[i].left * contact[i] < 0.5f;
#endif
                raw[i].left *= contact[i];
            }
            switch (SpaceAt(sampleTime(i)))
            {
            case Space::Right:
                raw[i] = { 1.0f, 0.0f };
                break;
            case Space::Left:
                raw[i] = { 0.0f, 1.0f };
                break;
            case Space::Body:
                raw[i] = { 0.0f, 0.0f };
                break;
            case Space::Auto:
            case Space::Off: // measured as usual, Blend fades the result out by the animation's weight
                continue;
            }
#if _DEBUG
            ++explicitSamples;
#endif
        }

        auto& smoothed = t_smoothed;
        smoothed.resize(sampleCount);
        const auto smoothingHalf = static_cast<unsigned int>(std::lround(kSmoothingHalf / step));
        bool handOff = false;
        bool anyLeft = false;
#if _DEBUG
        unsigned int rightSamples = 0;
        unsigned int leftSamples = 0;
#endif
        for (unsigned int i = 0; i < sampleCount; ++i)
        {
            const unsigned int low = i > smoothingHalf ? i - smoothingHalf : 0;
            const unsigned int high = (std::min)(sampleCount - 1, i + smoothingHalf);
            Acceptance sum{ 0.0f, 0.0f };
            for (unsigned int j = low; j <= high; ++j)
            {
                sum.right += raw[j].right;
                sum.left += raw[j].left;
            }
            const auto count = static_cast<float>(high - low + 1);
            Acceptance& sample = smoothed[i];
            sample = { sum.right / count, sum.left / count };
            handOff |= !sample.IsRightOnly();
            anyLeft |= sample.left > kMinAcceptance;
#if _DEBUG
            rightSamples += sample.right >= 0.5f;
            leftSamples += sample.left >= 0.5f;
#endif
        }

        if (handOff)
        {
            info.beginTime = beginTime;
            info.step = step;
            info.acceptance.assign(smoothed.begin(), smoothed.end());
            if (anyLeft)
                BuildLeftSpaceKeys(info, weapon, chains, beginTime, endTime);
        }

#if _DEBUG
        if (g_pluginSettings.weaponHandBlendDebugLog)
        {
            const float toPercent = 100.0f / static_cast<float>(sampleCount);
            DebugPrint(FormatString("WeaponHandBlend: %s holds the weapon in the right hand for %.0f%%, in the left hand for %.0f%% of %.2fs"
                " (left hand not touching the weapon for %.0f%%, weaponSpace keys for %.0f%%, %u left hand keys, weapon radius %.1f)",
                sequence.m_kName.CStr(), toPercent * static_cast<float>(rightSamples), toPercent * static_cast<float>(leftSamples),
                endTime - beginTime, toPercent * static_cast<float>(notTouching), toPercent * static_cast<float>(explicitSamples),
                static_cast<unsigned int>(info.leftSpaceKeys.size()), bound.radius));
        }
#endif
    }

    // Weighted average of transforms. Translation and rotation are weighted separately, like the regular blend.
    class TransformBlender
    {
    public:
        void Add(const NiTransform& transform, float fTranslateWeight, float fRotateWeight)
        {
            if (fTranslateWeight > 0.0f)
            {
                m_translate += transform.m_Translate * fTranslateWeight;
                m_scale += transform.m_fScale * fTranslateWeight;
                m_translateWeight += fTranslateWeight;
            }
            if (fRotateWeight > 0.0f)
            {
                NiQuaternion rotate;
                rotate.FromRotation(transform.m_Rotate);
                if (m_rotateWeight > 0.0f && NiQuaternion::Dot(m_rotate, rotate) < 0.0f)
                    rotate = -rotate;
                m_rotate = m_rotate + rotate * fRotateWeight;
                m_rotateWeight += fRotateWeight;
            }
        }

        bool HasTranslate() const { return m_translateWeight > 0.0f; }
        bool HasRotate() const { return m_rotateWeight > 0.0f; }

        NiTransform Get() const
        {
            NiTransform result = IdentityTransform();
            if (HasTranslate())
            {
                result.m_Translate = m_translate * (1.0f / m_translateWeight);
                result.m_fScale = m_scale / m_translateWeight;
            }
            if (HasRotate())
                ToUnitRotation(m_rotate, result.m_Rotate);
            return result;
        }

    private:
        NiPoint3 m_translate = NiPoint3::ZERO;
        NiQuaternion m_rotate = NiQuaternion(0.0f, 0.0f, 0.0f, 0.0f);
        float m_scale = 0.0f;
        float m_translateWeight = 0.0f;
        float m_rotateWeight = 0.0f;
    };

    // One item of the Weapon node's blend interpolator in the current evaluation
    struct Entry
    {
        const void* key = nullptr;
        // The item's sequence while attached. Null for detached items, pose interpolators and items without extra data.
        const NiControllerSequence* sequence = nullptr;
        // NodeData::sequences index: the attached item's sequence, or a detached item's cached pose. -1 if neither.
        int sequenceIndex = -1;
        float time = 0.0f;
        NiQuatTransform transform;
        Acceptance acceptance;
        float translateWeight = 0.0f;
        float rotateWeight = 0.0f;
        bool off = false; // "weaponSpace: off"
        // A sequence that has left the Weapon node's blend while the arms still fade it out (AddFadingSequences)
        bool fading = false;
        // Set by Blend for the items that weigh in. Its arms: the sequence's, a detached item's as cached, or the
        // blended ones for an item without arms of its own (pose interpolators).
        const ChainLocals* locals = nullptr;
        // The left hand relative to the weapon in the item's pose, never from the left hand keys. HoldLeftHand compares
        // it with the items' blended arms, which follow the tracks between keys.
        NiTransform handOnWeapon;
        // Set by Blend while the left hand is held: the weight each hand chain bone's own blend gave the item's
        // sequence in this update (ChainWeight). 0 where the sequence does not animate the bone or is not known.
        float rightChainWeights[WeaponHandBlend::kMaxChainLength] = {};
        float leftChainWeights[WeaponHandBlend::kMaxChainLength] = {};
    };
    // Evaluations of the Weapon node do not nest, so one buffer per blending thread is enough
    thread_local std::vector<Entry> t_entries;
    // The arms of the entries that have their sequence, by entry index
    thread_local std::vector<ChainLocals> t_sampled;

    bool IsWeighted(const Entry& entry)
    {
        return entry.translateWeight > 0.0f || entry.rotateWeight > 0.0f;
    }

    Entry* FindEntry(const void* key)
    {
        for (auto& entry : t_entries)
            if (entry.key == key)
                return &entry;
        return nullptr;
    }

    NiQuaternion Nlerp(const NiQuaternion& from, const NiQuaternion& to, float t)
    {
        const float sign = NiQuaternion::Dot(from, to) < 0.0f ? -1.0f : 1.0f;
        NiQuaternion result(from.GetW() + (sign * to.GetW() - from.GetW()) * t, from.GetX() + (sign * to.GetX() - from.GetX()) * t,
                            from.GetY() + (sign * to.GetY() - from.GetY()) * t, from.GetZ() + (sign * to.GetZ() - from.GetZ()) * t);
        result.Normalize();
        return result;
    }

    NiPoint3 Cross(const NiPoint3& a, const NiPoint3& b)
    {
        return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    }

    // Rotation by fAngle (radians) about a unit axis (right-handed, column vectors: R * v)
    NiMatrix33 AxisAngle(const NiPoint3& axis, float fAngle)
    {
        const float c = std::cos(fAngle);
        const float s = std::sin(fAngle);
        const float t = 1.0f - c;
        const float x = axis.x;
        const float y = axis.y;
        const float z = axis.z;
        return NiMatrix33{ { { c + t * x * x, t * x * y - s * z, t * x * z + s * y },
                             { t * x * y + s * z, c + t * y * y, t * y * z - s * x },
                             { t * x * z - s * y, t * y * z + s * x, c + t * z * z } } };
    }

    // The smallest rotation that turns direction from into direction to
    NiMatrix33 RotationBetween(const NiPoint3& from, const NiPoint3& to)
    {
        const NiPoint3 axis = Cross(from, to);
        const float sine = axis.Length();
        if (!(sine > 1e-6f * from.Length() * to.Length()))
            return IdentityTransform().m_Rotate;
        return AxisAngle(axis / sine, std::atan2(sine, from.Dot(to)));
    }

    constexpr float kMinBoneLength = 0.01f;

    // Rotates the last three nodes of a hand chain (upper arm, forearm, hand; the chain posed as in locals) so the hand
    // reaches handGoal, relative to the chain's top. The elbow bends about its current axis until the wrist is as far
    // from the shoulder as the goal, then the upper arm swings the wrist onto the goal by the smallest rotation, and
    // the hand takes the goal's rotation. Only rotations change, so bone lengths are kept. A goal out of reach is
    // approached as far as the arm stretches. A straight arm has no side to bend to and is left alone.
    void ReachWithArm(NiAVObject* const* chain, const NiTransform* locals, unsigned int count, const NiTransform& handGoal,
                      ArmReach& reach)
    {
        NiAVObject* const* arm = chain + (count - 3);
        const NiTransform parent = Compose(locals, count - 3);
        const NiTransform upperInChain = parent * locals[count - 3];
        const NiTransform foreInChain = upperInChain * locals[count - 2];
        const NiTransform handInChain = foreInChain * locals[count - 1];
        const NiPoint3 shoulder = upperInChain.m_Translate;
        const NiPoint3 elbow = foreInChain.m_Translate;
        const NiPoint3 toShoulder = shoulder - elbow;
        const NiPoint3 toWrist = handInChain.m_Translate - elbow;
        const float upperLength = toShoulder.Length();
        const float foreLength = toWrist.Length();
        const NiPoint3 bendAxis = Cross(toShoulder, toWrist);
        const float bendAxisLength = bendAxis.Length();
        if (!(upperLength > kMinBoneLength) || !(foreLength > kMinBoneLength) || !(bendAxisLength > 1e-4f * upperLength * foreLength))
            return;
        const float minReach = std::fabs(upperLength - foreLength) + kMinBoneLength;
        const float maxReach = upperLength + foreLength - kMinBoneLength;
        if (!(maxReach > minReach))
            return;
        const float reachLength = std::clamp((handGoal.m_Translate - shoulder).Length(), minReach, maxReach);
        const float elbowAngle = std::acos(std::clamp(
            (upperLength * upperLength + foreLength * foreLength - reachLength * reachLength) / (2.0f * upperLength * foreLength), -1.0f, 1.0f));
        // Rotating the wrist about toShoulder x toWrist opens the elbow
        const float currentAngle = std::atan2(bendAxisLength, toShoulder.Dot(toWrist));
        const NiMatrix33 bend = AxisAngle(bendAxis / bendAxisLength, elbowAngle - currentAngle);
        const NiPoint3 bentWrist = elbow + bend * toWrist;
        const NiMatrix33 swing = RotationBetween(bentWrist - shoulder, handGoal.m_Translate - shoulder);

        const NiMatrix33 upperRotate = swing * upperInChain.m_Rotate;
        const NiMatrix33 foreRotate = swing * bend * foreInChain.m_Rotate;
        NiTransform reached[3] = { arm[0]->m_kLocal, arm[1]->m_kLocal, arm[2]->m_kLocal };
        reached[0].m_Rotate = Transpose(parent.m_Rotate) * upperRotate;
        reached[1].m_Rotate = Transpose(upperRotate) * foreRotate;
        reached[2].m_Rotate = Transpose(foreRotate) * handGoal.m_Rotate;
        if (!IsFinite(reached[0]) || !IsFinite(reached[1]) || !IsFinite(reached[2]))
            return;
        for (int i = 0; i < 3; ++i)
        {
            NiMatrix33& rotate = arm[i]->m_kLocal.m_Rotate;
            reach.blended[i] = rotate;
            reach.reached[i] = reached[i].m_Rotate;
            rotate = reached[i].m_Rotate;
        }
        reach.applied = true;
    }

    // ArmReach: bones that nothing has blended since they were rotated get their blend's rotation back
    void UndoReach(WeaponHandBlend::NodeData& data)
    {
        ArmReach& reach = data.leftReach;
        if (!reach.applied)
            return;
        reach.applied = false;
        NiAVObject* const* arm = data.chains.left + (data.chains.leftCount - 3);
        for (int i = 0; i < 3; ++i)
        {
            NiMatrix33& rotate = arm[i]->m_kLocal.m_Rotate;
            if (std::memcmp(&rotate, &reach.reached[i], sizeof(NiMatrix33)) == 0)
                rotate = reach.blended[i];
        }
    }

    // A hand chain node's blend interpolator and the sum of the weights it blended its items with when it was last
    // evaluated. Smoothed weights do not add up to 1 (an item easing out keeps the weight a jump took from it, the
    // others theirs), and BlendValues divides by their sum.
    struct ChainBlend
    {
        const NiBlendInterpolator* blend = nullptr;
        float total = 0.0f;
    };

    ChainBlend MakeChainBlend(const NiBlendInterpolator* blend, NiAVObject* node)
    {
        ChainBlend result{ blend };
        if (blend->m_ucInterpCount == 1)
        {
            result.total = 1.0f;
            return result;
        }
        // Additive items are applied on top of the blend, not in it
        auto* extraData = kBlendInterpolatorExtraData::GetExtraData(node);
        for (const auto& item : blend->GetItems())
        {
            if (!item.m_spInterpolator)
                continue;
            if (const auto* extraItem = extraData ? extraData->GetItem(item.m_spInterpolator) : nullptr; extraItem && extraItem->isAdditive)
                continue;
            result.total += item.m_fNormalizedWeight;
        }
        return result;
    }

    // The hand chains' nodes' blends. A node the skeleton's controller does not animate has none.
    struct ChainBlends
    {
        ChainBlend right[WeaponHandBlend::kMaxChainLength];
        ChainBlend left[WeaponHandBlend::kMaxChainLength];
    };

    // Finds the hand chains' blend interpolators in the skeleton's multi-target controller. Returns false if no item
    // tells the controller.
    bool FindChainBlends(const HandChains& chains, ChainBlends& blends)
    {
        const NiMultiTargetTransformController* controller = nullptr;
        for (const Entry& entry : t_entries)
        {
            if (entry.sequence && entry.sequence->m_pkOwner)
            {
                controller = entry.sequence->m_pkOwner->GetMultiTargetTransformController();
                break;
            }
        }
        if (!controller)
            return false;
        const auto targets = controller->GetTargets();
        const auto blendInterps = controller->GetBlendInterps();
        for (size_t slot = 0; slot < targets.size(); ++slot)
        {
            NiAVObject* target = targets[slot];
            if (!target)
                continue;
            for (unsigned int i = 0; i < chains.rightCount; ++i)
                if (chains.right[i] == target)
                    blends.right[i] = MakeChainBlend(&blendInterps[slot], target);
            for (unsigned int i = 0; i < chains.leftCount; ++i)
                if (chains.left[i] == target)
                    blends.left[i] = MakeChainBlend(&blendInterps[slot], target);
        }
        return true;
    }

    // The share of a bone's blend that one of its interpolators had when the bone was last evaluated: its normalized
    // weight (smoothed; the rotation's, the channel BlendValues computes last) over the total of all of them
    float ChainWeight(const ChainBlend& chainBlend, const NiInterpolator* interpolator)
    {
        const NiBlendInterpolator* blend = chainBlend.blend;
        if (!blend || !interpolator || !(chainBlend.total > 0.0f))
            return 0.0f;
        if (blend->m_ucInterpCount == 1)
            return blend->m_pkSingleInterpolator == interpolator ? 1.0f : 0.0f;
        const auto* item = blend->GetItemByInterpolator(interpolator);
        return item ? item->m_fNormalizedWeight / chainBlend.total : 0.0f;
    }

    bool IsChainWeighted(const Entry& entry)
    {
        for (unsigned int i = 0; i < WeaponHandBlend::kMaxChainLength; ++i)
            if (entry.rightChainWeights[i] > 0.0f || entry.leftChainWeights[i] > 0.0f)
                return true;
        return false;
    }

    // Returns true if nothing but the entry holds its interpolators: the sequence is gone and its items have left every blend
    bool IsReleased(const SequenceInfo& info)
    {
        const auto released = [](const NiPointer<NiInterpolator>& interpolator) { return !interpolator || interpolator.data->m_uiRefCount == 1; };
        return released(info.weaponInterpolator) && std::ranges::all_of(info.rightInterpolators, released)
            && std::ranges::all_of(info.leftInterpolators, released);
    }

    // Adds the sequences that have left the Weapon node's blend while the arms may still fade them out, with no weight
    // on the Weapon node and the pose of their detach. HoldLeftHand models the arms from the Weapon node's sequences,
    // weighted as the arms' blends weigh them. A sequence must not leave that model while an arm still blends it, or
    // the hand moves by the sequence's share of the arm in one frame. A sequence without weight on the Weapon node
    // loses its item there as it detaches (nothing to fade out) but can still have weight on the arms, e.g. a run
    // easing out under an aimIS transition. Returns true if one of them holds the weapon with the left hand.
    bool AddFadingSequences(WeaponHandBlend::NodeData& data)
    {
        bool anyHandOff = false;
        for (size_t i = 0; i < data.sequences.size(); ++i)
        {
            const SequenceInfo& info = data.sequences[i];
            const int index = static_cast<int>(i);
            if (!info.fading || std::ranges::any_of(t_entries, [index](const Entry& entry) { return entry.sequenceIndex == index; }))
                continue;
            Entry& entry = t_entries.emplace_back();
            entry.key = &info;
            entry.sequenceIndex = index;
            entry.transform = info.detachedPose.weapon;
            entry.acceptance = info.detachedPose.acceptance;
            entry.off = info.detachedOff;
            entry.fading = true;
            anyHandOff |= !entry.acceptance.IsRightOnly();
        }
        return anyHandOff;
    }
}

bool WeaponHandBlend::Acceptance::IsRightOnly() const
{
    return right >= 1.0f - kMinAcceptance && left <= kMinAcceptance;
}

Acceptance WeaponHandBlend::SequenceInfo::Sample(float fTime) const
{
    if (acceptance.empty())
        return {};
    const float position = std::clamp((fTime - beginTime) / step, 0.0f, static_cast<float>(acceptance.size() - 1));
    const auto index = static_cast<unsigned int>(position);
    if (index + 1 >= acceptance.size())
        return acceptance.back();
    const float fraction = position - static_cast<float>(index);
    const Acceptance& a = acceptance[index];
    const Acceptance& b = acceptance[index + 1];
    return { a.right + (b.right - a.right) * fraction, a.left + (b.left - a.left) * fraction };
}

bool WeaponHandBlend::SequenceInfo::IsOff(float fTime) const
{
    for (const TimeRange& range : offRanges)
    {
        if (range.begin > fTime + kSameKeyTime)
            break;
        if (fTime + kSameKeyTime < range.end)
            return true;
    }
    return false;
}

bool WeaponHandBlend::SequenceInfo::SampleLeftSpace(float fTime, NiTransform& relativeToLeft) const
{
    const auto& keys = leftSpaceKeys;
    if (keys.empty())
        return false;
    const auto next = std::ranges::upper_bound(keys, fTime, {}, &LeftSpaceKey::time);
    const size_t i1 = std::clamp<size_t>(next - keys.begin(), 1, keys.size() - 1);
    const size_t i0 = i1 - 1;
    const float h = keys[i1].time - keys[i0].time;
    const float s = std::clamp((fTime - keys[i0].time) / h, 0.0f, 1.0f);
    // Cubic Hermite with Catmull-Rom tangents (finite differences over the neighbouring keys, uneven spacing allowed)
    const float s2 = s * s;
    const float s3 = s2 * s;
    const float h00 = 2.0f * s3 - 3.0f * s2 + 1.0f;
    const float h10 = s3 - 2.0f * s2 + s;
    const float h01 = -2.0f * s3 + 3.0f * s2;
    const float h11 = s3 - s2;
    const auto neighbours = [&](size_t i)
    {
        const size_t before = i > 0 ? i - 1 : i;
        const size_t after = (std::min)(i + 1, keys.size() - 1);
        return std::pair{ before, after };
    };
    const auto [b0, a0] = neighbours(i0);
    const auto [b1, a1] = neighbours(i1);
    const float w0 = h / (keys[a0].time - keys[b0].time);
    const float w1 = h / (keys[a1].time - keys[b1].time);
    const auto hermite = [&](float p0, float p1, float p0Before, float p0After, float p1Before, float p1After)
    {
        return h00 * p0 + h10 * w0 * (p0After - p0Before) + h01 * p1 + h11 * w1 * (p1After - p1Before);
    };
    const auto translate = [&](size_t i) -> const NiPoint3& { return keys[i].translate; };
    relativeToLeft.m_Translate.x = hermite(translate(i0).x, translate(i1).x, translate(b0).x, translate(a0).x, translate(b1).x, translate(a1).x);
    relativeToLeft.m_Translate.y = hermite(translate(i0).y, translate(i1).y, translate(b0).y, translate(a0).y, translate(b1).y, translate(a1).y);
    relativeToLeft.m_Translate.z = hermite(translate(i0).z, translate(i1).z, translate(b0).z, translate(a0).z, translate(b1).z, translate(a1).z);
    float q[4];
    for (int c = 0; c < 4; ++c)
        q[c] = hermite(keys[i0].rotate[c], keys[i1].rotate[c], keys[b0].rotate[c], keys[a0].rotate[c], keys[b1].rotate[c], keys[a1].rotate[c]);
    ToUnitRotation(NiQuaternion(q[0], q[1], q[2], q[3]), relativeToLeft.m_Rotate);
    relativeToLeft.m_fScale = keys[i0].scale + (keys[i1].scale - keys[i0].scale) * s;
    return true;
}

bool WeaponHandBlend::IsWeaponNode(const NiObjectNET* node)
{
    return node->m_pcName.data == WeaponName().data;
}

WeaponHandBlend::Context::Context(NiObjectNET* target, kBlendInterpolatorExtraData* extraData)
{
    if (!g_pluginSettings.fixWeaponHandBlend || !target || !extraData || !IsWeaponNode(target))
        return;
    auto* weapon = NI_DYNAMIC_CAST(NiAVObject, target);
    if (!weapon || !weapon->m_pkParent)
        return;
    auto& data = extraData->weaponHandBlend;
    if (!data)
        data = std::make_unique<NodeData>();
#if _DEBUG
    if (g_pluginSettings.weaponHandBlendDebugLog && data->loggedParent != weapon->m_pkParent)
    {
        // Reports when the game moves the Weapon node to another parent (prn: keys)
        data->loggedParent = weapon->m_pkParent;
        DebugPrint(FormatString("WeaponHandBlend: Weapon node %08X is now parented to %s", reinterpret_cast<UInt32>(weapon),
            weapon->m_pkParent->m_pcName.CStr()));
    }
#endif
    if (weapon->m_pkParent->m_pcName.data != RightHandName().data)
        return; // reparented by a prn: key (equip/unequip), not a left hand hand-off
    m_weapon = weapon;
    m_data = data.get();
}

bool WeaponHandBlend::Context::EnsureChains()
{
    if (!m_data->chainsBuilt)
    {
        m_data->chainsValid = BuildChains(m_weapon, m_data->chains);
        m_data->chainsBuilt = true;
    }
    return m_data->chainsValid;
}

void WeaponHandBlend::Context::RequireChains()
{
    if (!m_weapon)
        return;
    m_data->chainsCurrent = false;
    if (!EnsureChains())
        return;
    // Every node is required, not only until one fails: the regular blend of the weapon reads both arms
    bool bCurrent = true;
    for (unsigned int i = 0; i < m_data->chains.rightCount; ++i)
        bCurrent &= BoneEvaluation::Require(m_data->chains.right[i]);
    for (unsigned int i = 0; i < m_data->chains.leftCount; ++i)
        bCurrent &= BoneEvaluation::Require(m_data->chains.left[i]);
    m_data->chainsCurrent = bCurrent;
}

WeaponHandBlend::WeaponBound WeaponHandBlend::Context::GetWeaponBound() const
{
    WeaponBound bound;
    const NiSphere* world = m_weapon->m_kWorldBound;
    if (!world || !(world->radius > 0.0f) || !(m_weapon->m_kWorld.m_fScale > 0.0f))
        return bound;
    const NiTransform toLocal = Inverse(Orthonormal(m_weapon->m_kWorld));
    NiPoint3 center;
    center.x = world->x;
    center.y = world->y;
    center.z = world->z;
    bound.center = toLocal.m_Translate + toLocal.m_Rotate * center * toLocal.m_fScale;
    bound.radius = world->radius * toLocal.m_fScale;
    return bound;
}

const NiAVObject* WeaponHandBlend::Context::GetWeaponModel() const
{
    if (auto* node = m_weapon->GetAsNiNode())
        for (auto* child : node->m_children)
            if (child)
                return child;
    return nullptr;
}

int WeaponHandBlend::Context::FindSequence(const NiInterpolator* weaponInterpolator) const
{
    const auto& sequences = m_data->sequences;
    for (size_t i = 0; i < sequences.size(); ++i)
        if (sequences[i].weaponInterpolator.data == weaponInterpolator)
            return static_cast<int>(i);
    return -1;
}

int WeaponHandBlend::Context::FindOrMeasureSequence(NiInterpolator* weaponInterpolator, const NiControllerSequence& sequence)
{
    int index = FindSequence(weaponInterpolator);
    const NiAVObject* model = GetWeaponModel();
    if (index == -1)
    {
        m_data->sequences.emplace_back().weaponInterpolator = weaponInterpolator;
        index = static_cast<int>(m_data->sequences.size() - 1);
    }
    else if (!model || model == m_data->sequences[index].weaponModel)
    {
        // Measured with this weapon model, or there is no model now to measure the contact against
        return index;
    }
    SequenceInfo& info = m_data->sequences[index];
    info.weaponModel = model;
    // A skeleton without a left hand chain keeps the right hand only entry, so the search is not repeated every frame
    if (EnsureChains())
        Measure(info, sequence, m_weapon, m_data->chains, GetWeaponBound());
    return index;
}

void WeaponHandBlend::Context::AddItem(const void* key, kBlendInterpItem* extraItem, float fTime, const NiQuatTransform& kTransform)
{
    if (!m_weapon)
        return;
    if (!m_collecting)
    {
        t_entries.clear();
        m_collecting = true;
    }
    Entry& entry = t_entries.emplace_back();
    entry.key = key;
    entry.time = fTime;
    entry.transform = kTransform;
    if (extraItem && extraItem->interpolator && !extraItem->isAdditive)
    {
        if (const NiControllerSequence* sequence = extraItem->sequence)
        {
            entry.sequence = sequence;
            entry.sequenceIndex = FindOrMeasureSequence(extraItem->interpolator.data, *sequence);
            SequenceInfo& info = m_data->sequences[entry.sequenceIndex];
            info.fading = false;
            entry.acceptance = info.Sample(fTime);
            entry.off = info.IsOff(fTime);
        }
        else if (const int index = FindSequence(extraItem->interpolator.data); index != -1)
        {
            const SequenceInfo& info = m_data->sequences[index];
            entry.off = info.detachedOff;
            if (info.hasDetachedPose)
            {
                entry.sequenceIndex = index;
                entry.acceptance = info.detachedPose.acceptance;
            }
        }
    }
    m_anyHandOff |= !entry.acceptance.IsRightOnly();
}

void WeaponHandBlend::Context::SetTranslateWeight(const void* key, float fWeight)
{
    if (Entry* entry = FindEntry(key))
        entry->translateWeight = fWeight;
}

void WeaponHandBlend::Context::SetRotateWeight(const void* key, float fWeight)
{
    if (Entry* entry = FindEntry(key))
        entry->rotateWeight = fWeight;
}

void WeaponHandBlend::Context::BlendSingle(kBlendInterpItem* extraItem, float fTime, NiQuatTransform& kValue)
{
    if (!m_weapon)
        return;
    AddItem(nullptr, extraItem, fTime, kValue);
    SetTranslateWeight(nullptr, 1.0f);
    SetRotateWeight(nullptr, 1.0f);
    Blend(kValue);
}

void WeaponHandBlend::Context::CacheDetachedPose(const kBlendInterpItem& extraItem, const NiControllerSequence& sequence, float fTime)
{
    if (!m_weapon || !extraItem.interpolator || extraItem.isAdditive)
        return;
    // Sequences detach here, outside of blending, so this is where released sequences are dropped
    std::erase_if(m_data->sequences, IsReleased);
    // An item without weight that was never blended here while attached (bone LOD skips the Weapon node) is in no model
    // of the arms yet, so the sequence is not worth measuring
    if (extraItem.IsSmoothedWeightZero() && FindSequence(extraItem.interpolator.data) == -1)
        return;

    const int index = FindOrMeasureSequence(extraItem.interpolator.data, sequence);
    SequenceInfo& info = m_data->sequences[index];
    info.hasDetachedPose = false;
    info.fading = false;
    info.detachedOff = info.IsOff(fTime);
    const Acceptance acceptance = info.Sample(fTime);
    // Cache every item's arms, including those holding the weapon in the right hand only: HoldLeftHand blends them by
    // the arms' own weights for as long as they fade out there. Standing in a blended arm for them would move the hand
    // when a sequence detaches from the Weapon node while the arms still blend it.
    if (!EnsureChains())
        return;
    const HandChains& chains = m_data->chains;
    ItemPose& pose = info.detachedPose;
    SampleLocals(info, chains, fTime, pose.locals);
    // A hand track that ends before the sequence does is sampled past its last key
    const NiTransform sequenceLeft = Compose(pose.locals.left, chains.leftCount);
    if (!IsFinite(Compose(pose.locals.right, chains.rightCount)) || !IsFinite(sequenceLeft) || sequenceLeft.m_fScale <= 0.0f)
        return;
    pose.acceptance = acceptance;
    pose.hasRelativeToLeft = info.SampleLeftSpace(fTime, pose.relativeToLeft);
    if (!info.weaponInterpolator->Update(WeaponParentSwitch::GetUpdateTime(&extraItem, m_weapon, fTime), m_weapon, pose.weapon))
        pose.weapon.MakeInvalid();
    info.hasDetachedPose = true;
    info.fading = true;
}

namespace
{
    // Places the weapon by the references the blended items agree on (Blend), relative to the blended right hand (its
    // parent). corrected is the share of the blend that did not opt out.
    void MoveWeapon(NiQuatTransform& kValue, const TransformBlender* relatives, const float* referenceWeights, float corrected,
                    const NiTransform& rightBlended, const NiTransform& leftBlended)
    {
        const bool bTranslate = kValue.IsTranslateValid();
        const bool bRotate = kValue.IsRotateValid();
        const NiTransform rightBlendedInverse = Inverse(rightBlended);
        TransformBlender result;
        for (int reference = 0; reference < kReferenceCount; ++reference)
        {
            const TransformBlender& relative = relatives[reference];
            const float weight = referenceWeights[reference];
            if (weight <= kMinReferenceWeight || (bTranslate && !relative.HasTranslate()) || (bRotate && !relative.HasRotate()))
                continue;
            NiTransform candidate = relative.Get();
            if (reference == kLeft)
                candidate = rightBlendedInverse * leftBlended * candidate;
            else if (reference == kBody)
                candidate = rightBlendedInverse * candidate;
            result.Add(candidate, weight, weight);
        }
        if (!result.HasTranslate())
            return;
        const NiTransform blended = result.Get();
        if (!IsFinite(blended))
            return;
        if (bTranslate)
            kValue.SetTranslate(kValue.GetTranslate() + (blended.m_Translate - kValue.GetTranslate()) * corrected);
        if (bRotate)
        {
            NiQuaternion rotate;
            rotate.FromRotation(blended.m_Rotate);
            if (corrected < 1.0f)
                rotate = Nlerp(kValue.GetRotate(), rotate, corrected);
            kValue.SetRotate(rotate);
        }
    }

    // Returns base with the channels the blender has replaced by its blend
    NiTransform Merge(const TransformBlender& blender, NiTransform base)
    {
        if (!blender.HasTranslate() && !blender.HasRotate())
            return base;
        const NiTransform blended = blender.Get();
        if (blender.HasTranslate())
        {
            base.m_Translate = blended.m_Translate;
            base.m_fScale = blended.m_fScale;
        }
        if (blender.HasRotate())
            base.m_Rotate = blended.m_Rotate;
        return base;
    }

    // Returns a hand chain (HandChains::left or right) relative to its top, as blending the items' own chains joint by
    // joint gives it, each joint with the weights its own blend gave their sequences: what the arm's blend makes of
    // these items. The Weapon node's weights are not used because the arms also blend other animations (e.g. in third
    // person 1hp the aim and the walk key the arms but not the weapon) and take up other weight jumps (blend smoothing),
    // so their weights for the same sequences can differ substantially.
    NiTransform BlendItemsChain(const HandChains& chains, const ChainLocals& blendedLocals, bool bLeft)
    {
        const auto side = [bLeft](const ChainLocals& locals) { return bLeft ? locals.left : locals.right; };
        const unsigned int count = bLeft ? chains.leftCount : chains.rightCount;
        NiTransform result = IdentityTransform();
        for (unsigned int i = 0; i < count; ++i)
        {
            TransformBlender joint;
            for (const Entry& entry : t_entries)
            {
                if (!entry.locals)
                    continue;
                const float weight = bLeft ? entry.leftChainWeights[i] : entry.rightChainWeights[i];
                joint.Add(side(*entry.locals)[i], weight, weight);
            }
            result = result * Merge(joint, side(blendedLocals)[i]);
        }
        return result;
    }

    // Each arm is blended joint by joint, so between two poses that both hold the weapon with the left hand the hand
    // drifts off the weapon (e.g. aim -> aimIS, worst at the middle of the blend). The error is measured on the items
    // alone:
    // - the items' arms blended joint by joint as the engine does (BlendItemsChain), combined with the weapon as the
    //   Weapon node's blend does, put the hand somewhere relative to the weapon;
    // - the items' own places of the hand on the weapon, blended by the left hand's weights, give where it belongs.
    // The step from the first to the second, scaled by holdShare (the share of the left hand's blend holding the
    // weapon), is applied to the arm's hand, so whatever else moves the arm keeps doing so: additive animations, and
    // animations the Weapon node does not have (e.g. third person sneak attacks without a Weapon track). The arm then
    // reaches for the corrected hand (ReachWithArm). Runs while the Weapon node is evaluated, after this pass evaluated
    // the arms (RequireChains), so what it gives the arm is not overwritten by the arm's own blend and the arm's blend
    // weights are this pass's.
    void HoldLeftHand(const NiAVObject* weapon, WeaponHandBlend::NodeData& data, float holdShare, const ChainLocals& blendedLocals,
                      const NiTransform& leftBlended)
    {
        const HandChains& chains = data.chains;
        TransformBlender handOnWeapon;
        TransformBlender weaponLocal;
        for (const Entry& entry : t_entries)
        {
            weaponLocal.Add(ToTransform(entry.transform, weapon), entry.translateWeight, entry.rotateWeight);
            if (!entry.locals || entry.off || entry.acceptance.left <= kMinAcceptance)
                continue;
            const float weight = entry.leftChainWeights[chains.leftCount - 1] * entry.acceptance.left;
            handOnWeapon.Add(entry.handOnWeapon, weight, weight);
        }
        if (!handOnWeapon.HasTranslate() || !handOnWeapon.HasRotate())
            return;
        const NiTransform blendedOnWeapon = Inverse(BlendItemsChain(chains, blendedLocals, false) * Merge(weaponLocal, CurrentLocal(weapon)))
            * BlendItemsChain(chains, blendedLocals, true);
        const NiTransform step = Inverse(blendedOnWeapon) * handOnWeapon.Get();
        if (!IsFinite(step))
            return;
        const float share = (std::min)(holdShare, 1.0f);
        NiTransform partial = step;
        partial.m_Translate = step.m_Translate * share;
        partial.m_fScale = 1.0f;
        NiQuaternion rotate;
        rotate.FromRotation(step.m_Rotate);
        ToUnitRotation(Nlerp(NiQuaternion(1.0f, 0.0f, 0.0f, 0.0f), rotate, share), partial.m_Rotate);
        ReachWithArm(chains.left, blendedLocals.left, chains.leftCount, leftBlended * partial, data.leftReach);
    }
}

void WeaponHandBlend::Context::Blend(NiQuatTransform& kValue)
{
    if (!m_weapon)
        return;
    UndoReach(*m_data);
    if (!m_collecting)
        return; // no item evaluated, so t_entries belongs to another evaluation
    if (g_pluginSettings.keepLeftHandOnWeapon)
        m_anyHandOff |= AddFadingSequences(*m_data);
    if (!m_anyHandOff || !EnsureChains())
        return;
    const bool bTranslate = kValue.IsTranslateValid();
    const bool bRotate = kValue.IsRotateValid();
    if (!bTranslate && !bRotate)
        return;

    // How much of the blend's weight accepts each hand, and how much opted out
    float total = 0.0f;
    float off = 0.0f;
    Acceptance agreement{ 0.0f, 0.0f };
    for (const Entry& entry : t_entries)
    {
        const float weight = bTranslate ? entry.translateWeight : entry.rotateWeight;
        total += weight;
        if (entry.off)
            off += weight;
        agreement.right += weight * entry.acceptance.right;
        agreement.left += weight * entry.acceptance.left;
    }
    if (total <= 0.0f)
        return;
    // The opted out share keeps the regular blend, so the correction fades in and out with those animations' weight
    const float corrected = 1.0f - off / total;
    if (corrected <= kMinReferenceWeight)
        return;

    // The right hand wins where all agree on it, then the left hand. The body takes whatever both leave.
    float referenceWeights[kReferenceCount];
    referenceWeights[kRight] = static_cast<float>(std::pow(agreement.right / total, kAgreementExponent));
    referenceWeights[kLeft] = (1.0f - referenceWeights[kRight]) * static_cast<float>(std::pow(agreement.left / total, kAgreementExponent));
    referenceWeights[kBody] = (std::max)(0.0f, 1.0f - referenceWeights[kRight] - referenceWeights[kLeft]);
    const bool bMoveWeapon = referenceWeights[kRight] < 1.0f - kMinReferenceWeight;
    // Left hand: where the weapon stays on the right hand, the share of the left hand's blend whose animations hold the
    // weapon pulls the hand to where they hold it. This is not skipped while one animation has all of the Weapon node,
    // because the arms can belong to another (third person: a gait keys the arms over aim and under aimIS). Skipping
    // it would switch the correction on and off with the aim up/down weights and at the ends of the Weapon node's blends.
    const HandChains& chains = m_data->chains;
    float holdShare = 0.0f;
    ChainBlends chainBlends;
    if (g_pluginSettings.keepLeftHandOnWeapon && chains.leftCount >= 3 && referenceWeights[kRight] > kMinReferenceWeight
        && m_data->chainsCurrent && FindChainBlends(chains, chainBlends))
    {
        float holding = 0.0f;
        unsigned int weighted = 0;
        for (Entry& entry : t_entries)
        {
            if (entry.sequenceIndex != -1)
            {
                SequenceInfo& info = m_data->sequences[entry.sequenceIndex];
                for (unsigned int i = 0; i < chains.rightCount; ++i)
                    entry.rightChainWeights[i] = ChainWeight(chainBlends.right[i], info.rightInterpolators[i]);
                for (unsigned int i = 0; i < chains.leftCount; ++i)
                    entry.leftChainWeights[i] = ChainWeight(chainBlends.left[i], info.leftInterpolators[i]);
                // Its items have left the arms' blends, which a detached sequence does not join again
                if (entry.fading && !IsChainWeighted(entry))
                    info.fading = false;
                if (!entry.off && entry.acceptance.left > kMinAcceptance)
                    holding += entry.leftChainWeights[chains.leftCount - 1] * entry.acceptance.left;
            }
            weighted += IsWeighted(entry) || IsChainWeighted(entry);
        }
        // One animation alone places the weapon and the arms (as far as it animates them), so the step is 0
        if (weighted > 1)
            holdShare = referenceWeights[kRight] * holding;
    }
    const bool bHoldLeftHand = holdShare > kMinReferenceWeight;
    if (!bMoveWeapon && !bHoldLeftHand)
        return; // the regular blend

    ChainLocals blendedLocals;
    CurrentLocals(chains, blendedLocals);
    const NiTransform rightBlended = Compose(blendedLocals.right, chains.rightCount);
    const NiTransform leftBlended = Compose(blendedLocals.left, chains.leftCount);
    if (rightBlended.m_fScale <= 0.0f || leftBlended.m_fScale <= 0.0f)
        return;
    const bool bNeedBody = bMoveWeapon && referenceWeights[kBody] > kMinReferenceWeight;

    // Each item's weapon relative to each reference, in the item's own pose
    TransformBlender relatives[kReferenceCount];
    t_sampled.resize(t_entries.size());
    for (size_t i = 0; i < t_entries.size(); ++i)
    {
        Entry& entry = t_entries[i];
        if (!IsWeighted(entry) && !(bHoldLeftHand && IsChainWeighted(entry)))
            continue;
        const NiTransform weaponLocal = ToTransform(entry.transform, m_weapon);
        const Acceptance& acceptance = entry.acceptance;
        relatives[kRight].Add(weaponLocal, entry.translateWeight * acceptance.right, entry.rotateWeight * acceptance.right);
        // The left hand and body references need the item's hands, the left hand correction needs every item's arms
        const bool bPlacesWeapon = !acceptance.IsRightOnly() || bNeedBody;
        if (!bPlacesWeapon && !bHoldLeftHand)
            continue;

        // Items without arms of their own (pose interpolators) use the blended arms
        entry.locals = &blendedLocals;
        NiTransform sequenceRight = rightBlended;
        NiTransform sequenceLeft = leftBlended;
        NiTransform relativeToLeft;
        bool keyedLeft = false;
        if (entry.sequenceIndex != -1)
        {
            const SequenceInfo& info = m_data->sequences[entry.sequenceIndex];
            if (entry.sequence)
            {
                ChainLocals& sampled = t_sampled[i];
                SampleLocals(info, chains, entry.time, sampled);
                const NiTransform sampledRight = Compose(sampled.right, chains.rightCount);
                const NiTransform sampledLeft = Compose(sampled.left, chains.leftCount);
                // A hand track that ends before the sequence does is sampled past its last key
                if (IsFinite(sampledRight) && IsFinite(sampledLeft) && sampledLeft.m_fScale > 0.0f)
                {
                    entry.locals = &sampled;
                    sequenceRight = sampledRight;
                    sequenceLeft = sampledLeft;
                }
                keyedLeft = acceptance.left > 0.0f && info.SampleLeftSpace(entry.time, relativeToLeft);
            }
            else
            {
                const ItemPose& pose = info.detachedPose;
                entry.locals = &pose.locals;
                sequenceRight = Compose(pose.locals.right, chains.rightCount);
                sequenceLeft = Compose(pose.locals.left, chains.leftCount);
                keyedLeft = pose.hasRelativeToLeft;
                relativeToLeft = pose.relativeToLeft;
            }
        }

        const NiTransform relativeToBody = sequenceRight * weaponLocal;
        entry.handOnWeapon = Inverse(relativeToBody) * sequenceLeft;
        if (!bPlacesWeapon)
            continue;
        if (!keyedLeft)
            relativeToLeft = Inverse(sequenceLeft) * relativeToBody;
        relatives[kLeft].Add(relativeToLeft, entry.translateWeight * acceptance.left, entry.rotateWeight * acceptance.left);
        relatives[kBody].Add(relativeToBody, entry.translateWeight, entry.rotateWeight);
    }

    if (bMoveWeapon)
        MoveWeapon(kValue, relatives, referenceWeights, corrected, rightBlended, leftBlended);
    if (bHoldLeftHand)
        HoldLeftHand(m_weapon, *m_data, holdShare, blendedLocals, leftBlended);
}
