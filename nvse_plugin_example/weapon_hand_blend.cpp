#include "weapon_hand_blend.h"

#include <algorithm>
#include <cmath>
#include <string_view>

#include "blend_smoothing.h"
#include "hooks.h"
#include "NiObjects.h"
#include "string_view_util.h"
#include "utility.h"

using WeaponHandBlend::Acceptance;
using WeaponHandBlend::HandChains;
using WeaponHandBlend::SequenceInfo;

namespace
{
    // Classification sampling
    constexpr float kSampleRate = 30.0f;
    constexpr unsigned int kMaxSamples = 512;
    // Half width of the window over which reference-relative motion is accumulated (seconds)
    constexpr float kMotionWindowHalf = 0.1f;
    // Half width of the box filter applied to the acceptance curves (seconds)
    constexpr float kSmoothingHalf = 0.05f;
    // Converts a rotation difference (radians) into game units so it can be summed with translation
    constexpr float kLeverArm = 10.0f;
    // Below this accumulated motion over a window, the weapon is rigid relative to a reference
    constexpr float kMinWindowMotion = 0.75f;
    // A hand is accepted fully while the weapon moves no more relative to it than relative to the steadiest reference (or
    // kMinWindowMotion), and not at all once it moves kLooseRatio times as much
    constexpr float kLooseRatio = 2.0f;
    // When neither hand moves relative to the weapon, one this far from the right hand is not held by it
    constexpr float kFarFromRightHand = 15.0f;
    constexpr float kMinAcceptance = 0.001f;
    // Sharpens agreement: a hand accepted by only part of the blend's weight quickly yields to the next reference
    constexpr int kAgreementExponent = 8;
    constexpr float kMinReferenceWeight = 1e-4f;
    // The left hand touches the weapon while within this distance outside the weapon model's bounding sphere (game units)
    constexpr float kContactMargin = 4.0f;
    // A smaller bound means no weapon model is attached; contact is then not tested
    constexpr float kMinWeaponRadius = 1.0f;
    // Key gaps longer than this (below 20 fps) let a counter-animated weapon swim visibly between keys
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

    NiTransform IdentityTransform()
    {
        NiTransform result;
        result.m_Rotate = NiMatrix33{ { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } } };
        result.m_Translate = NiPoint3::ZERO;
        result.m_fScale = 1.0f;
        return result;
    }

    NiMatrix33 Transpose(const NiMatrix33& matrix)
    {
        NiMatrix33 result;
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col)
                result.m_pEntry[row][col] = matrix.m_pEntry[col][row];
        return result;
    }

    // Rotation matrices in bone locals are orthonormal, so the inverse rotation is the transpose
    NiTransform Inverse(const NiTransform& transform)
    {
        NiTransform result;
        result.m_Rotate = Transpose(transform.m_Rotate);
        result.m_fScale = 1.0f / transform.m_fScale;
        result.m_Translate = result.m_Rotate * -transform.m_Translate * result.m_fScale;
        return result;
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

    // Key interpolation does not always produce unit quaternions; a non-unit quaternion turns into a matrix that is not
    // orthonormal, which breaks the transpose-as-inverse and makes identical rotations measure as different.
    void ToUnitRotation(NiQuaternion quaternion, NiMatrix33& rotation)
    {
        quaternion.Normalize();
        quaternion.ToRotation(rotation);
    }

    NiTransform CurrentLocal(const NiAVObject* node)
    {
        NiTransform result = node->m_kLocal;
        NiQuaternion rotation;
        rotation.FromRotation(result.m_Rotate);
        ToUnitRotation(rotation, result.m_Rotate);
        return result;
    }

    // An interpolator's value as a transform; channels it does not provide come from the node's current local transform.
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

    // Local transform of a node as animated by one of a sequence's interpolators; without one (the sequence does not
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
            // the interpolator caches its last evaluation by time; sampling at other times must not leave it stale
            interpolator->m_fLastTime = -NI_INFINITY;
        }
        return ToTransform(value, node);
    }

    NiTransform SampleChain(NiAVObject* const* nodes, NiInterpolator* const* interpolators, unsigned int count, float fTime,
                            bool bInvalidateCache)
    {
        NiTransform result = IdentityTransform();
        for (unsigned int i = 0; i < count; ++i)
            result = result * SampleLocal(interpolators[i], nodes[i], fTime, bInvalidateCache);
        return result;
    }

    NiTransform BlendedChain(NiAVObject* const* chain, unsigned int count)
    {
        NiTransform result = IdentityTransform();
        for (unsigned int i = 0; i < count; ++i)
            result = result * CurrentLocal(chain[i]);
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

    // 1 while the left hand touches the weapon model's bounding sphere, falling to 0 at kContactMargin outside of it; 1 when
    // there is no model to test against
    float Contact(const NiTransform& weaponRelativeToLeft, const WeaponHandBlend::WeaponBound& bound)
    {
        if (bound.radius < kMinWeaponRadius)
            return 1.0f;
        const NiPoint3 hand = Inverse(weaponRelativeToLeft).m_Translate; // the left hand in Weapon node space
        const float outside = (hand - bound.center).Length() - bound.radius;
        if (!std::isfinite(outside))
            return 1.0f;
        return std::clamp(1.0f - outside / kContactMargin, 0.0f, 1.0f);
    }

    // "weaponSpace: right|left|body|auto|off" text keys set the space from their time until the next one; auto (and the
    // time before the first key) leaves it to the measurement; off keeps the regular blend for this animation's share
    enum class Space : unsigned char { Auto, Right, Left, Body, Off };

    struct SpaceKey
    {
        float time;
        Space space;
    };

    // Scratch buffers for Measure, reused per thread: measuring runs on whichever thread blends the actor
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
        constexpr std::string_view kPrefix = "weaponSpace:";
        for (const NiTextKey& key : sequence.m_spTextKeys->GetKeys())
        {
            const char* text = key.m_kText.CStr();
            if (!text || !StartsWith(text, kPrefix.data()))
                continue;
            std::string_view value(text + kPrefix.size());
            const auto first = value.find_first_not_of(" \t");
            if (first == std::string_view::npos)
                continue;
            value = value.substr(first, value.find_last_not_of(" \t") - first + 1);
            Space space;
            if (sv::equals_ci(value, "right"))
                space = Space::Right;
            else if (sv::equals_ci(value, "left"))
                space = Space::Left;
            else if (sv::equals_ci(value, "body"))
                space = Space::Body;
            else if (sv::equals_ci(value, "auto"))
                space = Space::Auto;
            else if (sv::equals_ci(value, "off"))
                space = Space::Off;
            else
            {
                DebugPrint(FormatString("WeaponHandBlend: %s has an unknown text key \"%s\" (expected weaponSpace: right, left, body, auto or off)",
                    sequence.m_kName.CStr(), text));
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

    void AddKeyTimes(NiInterpolator* interpolator)
    {
        auto* transform = interpolator ? NI_DYNAMIC_CAST(NiTransformInterpolator, interpolator) : nullptr;
        if (!transform)
            return;
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
                // an Euler key's own time is never set; its three float tracks hold the timing
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
    }

    // The weapon relative to the left hand at every key time of the weapon and hand tracks. Between keys each track
    // interpolates on its own, so a weapon keyed relative to the right hand and a left hand keyed down its own chain drift
    // apart; interpolating their relation instead keeps a counter-animated weapon on the left hand. Exact at the keys.
    void BuildLeftSpaceKeys(SequenceInfo& info, NiAVObject* weapon, const HandChains& chains, float beginTime, float endTime)
    {
        auto& times = t_keyTimes;
        times.clear();
        AddKeyTimes(info.weaponInterpolator.data);
        for (unsigned int i = 0; i < chains.rightCount; ++i)
            AddKeyTimes(info.rightInterpolators[i]);
        for (unsigned int i = 0; i < chains.leftCount; ++i)
            AddKeyTimes(info.leftInterpolators[i]);
        times.push_back(beginTime);
        times.push_back(endTime);
        std::erase_if(times, [&](float time) { return !(time >= beginTime && time <= endTime); });
        std::ranges::sort(times);
        times.erase(std::unique(times.begin(), times.end(), [](float a, float b) { return b - a < kSameKeyTime; }), times.end());

        // keys this dense already keep the tracks together
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

    // Measures how firmly the weapon is held by each hand. A counter-animated weapon keeps a near-constant offset from the
    // left hand while its local transform (relative to the right hand) changes; a weapon held in the right hand shows the
    // opposite, and one held with both hands neither. Motion relative to the body (the hands' common ancestor) is measured
    // too, so that a weapon moving on its own is not attributed to the hand it moves least relative to. A still pose is
    // rigid relative to everything: there the left hand only counts as holding the weapon if it touches the weapon model.
    void Measure(SequenceInfo& info, const NiControllerSequence& sequence, NiAVObject* weapon, const HandChains& chains,
                 const WeaponHandBlend::WeaponBound& bound)
    {
        info.acceptance.clear();
        info.leftSpaceKeys.clear();
        // read before anything else: an animation without left hand tracks can opt out as well
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
        // begin + step * i overshoots the end key time by a rounding error, and the engine's rotation key search reads past
        // the key array for times beyond a track's last key
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

        // prefix sums of per-step motion relative to each reference
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
            // a non-finite sample (a track that ends before the sequence does) contributes no motion
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
            // rigid relative to both hands, so motion cannot tell them apart: a weapon far from the right hand is held by the
            // left hand if that touches it; one touching neither (floating, like a levitation gag) stays with the right
            // hand, which keeps the regular blend
            if (right < kMinWindowMotion && left < kMinWindowMotion && relative[kRight][i].m_Translate.Length() > kFarFromRightHand)
                raw[i].right = 1.0f - contact[i];
            // a left hand that only stays still next to a weapon the right hand holds is not holding it
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
            case Space::Off: // measured as usual; Blend fades the result out by the animation's weight
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

    // Weighted average of transforms, translation and rotation weighted separately like the regular blend
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
        // the item's sequence while attached, null for detached items, pose interpolators and items without extra data
        const NiControllerSequence* sequence = nullptr;
        // NodeData::sequences index: the attached item's sequence, or a detached item's cached pose; -1 if neither
        int sequenceIndex = -1;
        float time = 0.0f;
        NiQuatTransform transform;
        Acceptance acceptance;
        float translateWeight = 0.0f;
        float rotateWeight = 0.0f;
        bool off = false; // "weaponSpace: off"
    };
    // evaluations of the Weapon node do not nest, so one buffer per blending thread is enough
    thread_local std::vector<Entry> t_entries;

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
    // cubic Hermite with Catmull-Rom tangents (finite differences over the neighbouring keys, uneven spacing allowed)
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
        // reports when the game moves the Weapon node to another parent (prn: keys)
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
        // measured with this weapon model, or there is no model now to measure the contact against
        return index;
    }
    SequenceInfo& info = m_data->sequences[index];
    info.weaponModel = model;
    // a skeleton without a left hand chain keeps the right hand only entry so the search is not repeated every frame
    if (EnsureChains())
        Measure(info, sequence, m_weapon, m_data->chains, GetWeaponBound());
    return index;
}

void WeaponHandBlend::Context::SampleHands(const SequenceInfo& info, float fTime, bool bInvalidateCache, NiTransform& right,
                                           NiTransform& left) const
{
    const HandChains& chains = m_data->chains;
    right = SampleChain(chains.right, info.rightInterpolators, chains.rightCount, fTime, bInvalidateCache);
    left = SampleChain(chains.left, info.leftInterpolators, chains.leftCount, fTime, bInvalidateCache);
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
            const SequenceInfo& info = m_data->sequences[entry.sequenceIndex];
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
    // Sequences detach here, outside of blending, so this is where released sequences are dropped: an entry whose Weapon
    // interpolator is referenced by nobody else belongs to a sequence that is gone.
    std::erase_if(m_data->sequences, [](const SequenceInfo& info) { return info.weaponInterpolator.data->m_uiRefCount == 1; });

    const int index = FindOrMeasureSequence(extraItem.interpolator.data, sequence);
    SequenceInfo& info = m_data->sequences[index];
    info.hasDetachedPose = false;
    info.detachedOff = info.IsOff(fTime);
    const Acceptance acceptance = info.Sample(fTime);
    if (acceptance.IsRightOnly() || !EnsureChains())
        return;
    NiTransform sequenceRight;
    NiTransform sequenceLeft;
    SampleHands(info, fTime, false, sequenceRight, sequenceLeft);
    // a hand track that ends before the sequence does is sampled past its last key
    if (!IsFinite(sequenceRight) || !IsFinite(sequenceLeft) || sequenceLeft.m_fScale <= 0.0f)
        return;
    NiTransform relativeToLeft;
    const bool hasRelativeToLeft = info.SampleLeftSpace(fTime, relativeToLeft);
    info.detachedPose = { acceptance, sequenceRight, sequenceLeft, relativeToLeft, hasRelativeToLeft };
    info.hasDetachedPose = true;
}

void WeaponHandBlend::Context::Blend(NiQuatTransform& kValue)
{
    if (!m_weapon || !m_anyHandOff || !EnsureChains())
        return;
    const bool bTranslate = kValue.IsTranslateValid();
    const bool bRotate = kValue.IsRotateValid();
    if (!bTranslate && !bRotate)
        return;

    // how much of the blend's weight accepts each hand, and how much opted out
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
    // the opted out share keeps the regular blend, so the correction fades in and out with those animations' weight
    const float corrected = 1.0f - off / total;
    if (corrected <= kMinReferenceWeight)
        return;

    // the right hand wins where all agree on it, then the left hand; the body takes whatever both leave
    float referenceWeights[kReferenceCount];
    referenceWeights[kRight] = static_cast<float>(std::pow(agreement.right / total, kAgreementExponent));
    referenceWeights[kLeft] = (1.0f - referenceWeights[kRight]) * static_cast<float>(std::pow(agreement.left / total, kAgreementExponent));
    referenceWeights[kBody] = (std::max)(0.0f, 1.0f - referenceWeights[kRight] - referenceWeights[kLeft]);
    if (referenceWeights[kRight] >= 1.0f - kMinReferenceWeight)
        return; // the regular blend

    const HandChains& chains = m_data->chains;
    const NiTransform rightBlended = BlendedChain(chains.right, chains.rightCount);
    const NiTransform leftBlended = BlendedChain(chains.left, chains.leftCount);
    if (rightBlended.m_fScale <= 0.0f || leftBlended.m_fScale <= 0.0f)
        return;
    const bool bNeedBody = referenceWeights[kBody] > kMinReferenceWeight;

    // each item's weapon relative to each reference, in the item's own pose
    TransformBlender relatives[kReferenceCount];
    for (const Entry& entry : t_entries)
    {
        const NiTransform weaponLocal = ToTransform(entry.transform, m_weapon);
        const Acceptance& acceptance = entry.acceptance;
        relatives[kRight].Add(weaponLocal, entry.translateWeight * acceptance.right, entry.rotateWeight * acceptance.right);
        if (acceptance.IsRightOnly() && !bNeedBody)
            continue;

        // items without hands of their own (detached right hand only items, pose interpolators) use the blended hands
        NiTransform sequenceRight = rightBlended;
        NiTransform sequenceLeft = leftBlended;
        NiTransform relativeToLeft;
        bool keyedLeft = false;
        if (entry.sequenceIndex != -1)
        {
            const SequenceInfo& info = m_data->sequences[entry.sequenceIndex];
            if (entry.sequence)
            {
                NiTransform sampledRight;
                NiTransform sampledLeft;
                SampleHands(info, entry.time, false, sampledRight, sampledLeft);
                // a hand track that ends before the sequence does is sampled past its last key
                if (IsFinite(sampledRight) && IsFinite(sampledLeft) && sampledLeft.m_fScale > 0.0f)
                {
                    sequenceRight = sampledRight;
                    sequenceLeft = sampledLeft;
                }
                keyedLeft = acceptance.left > 0.0f && info.SampleLeftSpace(entry.time, relativeToLeft);
            }
            else
            {
                sequenceRight = info.detachedPose.sequenceRight;
                sequenceLeft = info.detachedPose.sequenceLeft;
                keyedLeft = info.detachedPose.hasRelativeToLeft;
                relativeToLeft = info.detachedPose.relativeToLeft;
            }
        }

        const NiTransform relativeToBody = sequenceRight * weaponLocal;
        if (!keyedLeft)
            relativeToLeft = Inverse(sequenceLeft) * relativeToBody;
        relatives[kLeft].Add(relativeToLeft, entry.translateWeight * acceptance.left, entry.rotateWeight * acceptance.left);
        relatives[kBody].Add(relativeToBody, entry.translateWeight, entry.rotateWeight);
    }

    // where each reference puts the weapon, relative to the blended right hand (its parent)
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
