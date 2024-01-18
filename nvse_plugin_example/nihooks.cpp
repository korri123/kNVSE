#include "nihooks.h"

#include "commands_animation.h"
#include "hooks.h"
#include "NiNodes.h"
#include "SafeWrite.h"
#include "utility.h"

#if _DEBUG
extern std::unordered_map<NiBlendInterpolator*, std::unordered_set<NiControllerSequence*>> g_debugInterpMap;
extern std::unordered_map<NiInterpolator*, const char*> g_debugInterpNames;
extern std::unordered_map<NiInterpolator*, const char*> g_debugInterpSequences;
#endif

enum
{
    MANAGER_CONTROLLED_MASK = 0X0001,
    ONLY_USE_HIGHEST_WEIGHT_MASK = 0X0002,
    COMPUTE_NORMALIZED_WEIGHTS_MASK = 0x0004
};

constexpr auto NI_INFINITY = FLT_MAX;

bool GetBit(NiBlendInterpolator *_this, unsigned int uMask)
{
    return (_this->m_uFlags & uMask) != 0;
}

bool GetComputeNormalizedWeights(NiBlendInterpolator *_this)
{
    return GetBit(_this, COMPUTE_NORMALIZED_WEIGHTS_MASK);
}

void SetBit(NiBlendInterpolator *_this, bool bSet, unsigned int uMask)
{
    if (bSet)
        _this->m_uFlags |= uMask;
    else
        _this->m_uFlags &= ~uMask;
}

void SetComputeNormalizedWeights(NiBlendInterpolator *_this, bool bComputeNormalizedWeights)
{
    SetBit(_this, bComputeNormalizedWeights, COMPUTE_NORMALIZED_WEIGHTS_MASK);
}

bool GetOnlyUseHighestWeight(NiBlendInterpolator *_this)
{
    return GetBit(_this, ONLY_USE_HIGHEST_WEIGHT_MASK);
}

const unsigned char INVALID_INDEX = UCHAR_MAX;

void ComputeNormalizedEvenWeights(
    NiBlendInterpolator& _this,
    NiBlendInterpolator::InterpArrayItem& apItem1,
    NiBlendInterpolator::InterpArrayItem& apItem2
)
{
    // Calculate the real weight of each item.
    float fRealWeight1 = apItem1.m_fWeight * apItem1.m_fEaseSpinner;
    float fRealWeight2 = apItem2.m_fWeight * apItem2.m_fEaseSpinner;
    if (fRealWeight1 == 0.0f && fRealWeight2 == 0.0f)
    {
        apItem1.m_fNormalizedWeight = 0.0f;
        apItem2.m_fNormalizedWeight = 0.0f;
        return;
    }

    // Compute normalized weights.
    if (apItem1.m_cPriority > apItem2.m_cPriority)
    {
        if (apItem1.m_fEaseSpinner == 1.0f)
        {
            apItem1.m_fNormalizedWeight = 1.0f;
            apItem2.m_fNormalizedWeight = 0.0f;
            return;
        }

        float fOneMinusEaseSpinner = 1.0f - apItem1.m_fEaseSpinner;
        float fOneOverSumOfWeights = 1.0f / (apItem1.m_fEaseSpinner *
            fRealWeight1 + fOneMinusEaseSpinner * fRealWeight2);
        apItem1.m_fNormalizedWeight = apItem1.m_fEaseSpinner * fRealWeight1
            * fOneOverSumOfWeights;
        apItem2.m_fNormalizedWeight = fOneMinusEaseSpinner * fRealWeight2 *
            fOneOverSumOfWeights;
    }
    else if (apItem1.m_cPriority < apItem2.m_cPriority)
    {
        if (apItem2.m_fEaseSpinner == 1.0f)
        {
            apItem1.m_fNormalizedWeight = 0.0f;
            apItem2.m_fNormalizedWeight = 1.0f;
            return;
        }

        float fOneMinusEaseSpinner = 1.0f - apItem2.m_fEaseSpinner;
        float fOneOverSumOfWeights = 1.0f / (apItem2.m_fEaseSpinner *
            fRealWeight2 + fOneMinusEaseSpinner * fRealWeight1);
        apItem1.m_fNormalizedWeight = fOneMinusEaseSpinner * fRealWeight1 *
            fOneOverSumOfWeights;
        apItem2.m_fNormalizedWeight = apItem2.m_fEaseSpinner * fRealWeight2
            * fOneOverSumOfWeights;
    }
    else
    {
        float fOneOverSumOfWeights = 1.0f / (fRealWeight1 + fRealWeight2);
        apItem1.m_fNormalizedWeight = fRealWeight1 * fOneOverSumOfWeights;
        apItem2.m_fNormalizedWeight = fRealWeight2 * fOneOverSumOfWeights;
    }

    // Only use the highest weight, if so desired.
    if (GetOnlyUseHighestWeight(&_this))
    {
        if (apItem1.m_fNormalizedWeight >= apItem2.m_fNormalizedWeight)
        {
            apItem1.m_fNormalizedWeight = 1.0f;
            apItem2.m_fNormalizedWeight = 0.0f;
        }
        else
        {
            apItem1.m_fNormalizedWeight = 0.0f;
            apItem2.m_fNormalizedWeight = 1.0f;
        }
        return;
    }

    // Exclude weights below threshold.
    if (_this.m_fWeightThreshold > 0.0f)
    {
        bool bReduced1 = false;
        if (apItem1.m_fNormalizedWeight < _this.m_fWeightThreshold)
        {
            apItem1.m_fNormalizedWeight = 0.0f;
            bReduced1 = true;
        }

        bool bReduced2 = false;
        if (apItem2.m_fNormalizedWeight < _this.m_fWeightThreshold)
        {
            apItem2.m_fNormalizedWeight = 0.0f;
            bReduced1 = true;
        }

        if (bReduced1 && bReduced2)
        {
            return;
        }
        else if (bReduced1)
        {
            apItem2.m_fNormalizedWeight = 1.0f;
            return;
        }
        else if (bReduced2)
        {
            apItem1.m_fNormalizedWeight = 1.0f;
            return;
        }
    }
}

std::unordered_set<NiInterpolator*> g_easingInterps;

bool ComputeTwoBlendingHighPriorityInterpolators(NiBlendInterpolator* _this)
{
    const auto isBlendingHighInterpItem = [&](const NiBlendInterpolator::InterpArrayItem& item)
    {
        return item.m_spInterpolator != nullptr && item.m_cPriority == _this->m_cHighPriority
            && item.m_fEaseSpinner != 0.0f && item.m_fEaseSpinner != 1.0f && g_easingInterps.contains(item.m_spInterpolator.data);
    };
    std::span interpItems(_this->m_pkInterpArray, _this->m_ucArraySize);
    const auto numHighPriorityInterps = ra::count_if(interpItems, isBlendingHighInterpItem);
    if (numHighPriorityInterps == 2)
    {
        NiBlendInterpolator::InterpArrayItem* highPriorityInterps[2] {nullptr};
        for (auto& item : interpItems)
        {
            if (isBlendingHighInterpItem(item))
            {
                if (highPriorityInterps[0] == nullptr)
                    highPriorityInterps[0] = &item;
                else
                {
                    highPriorityInterps[1] = &item;
                    break;
                }
            }
        }
        ComputeNormalizedEvenWeights(*_this, *highPriorityInterps[0], *highPriorityInterps[1]);
        for (auto& item : interpItems)
        {
            if (&item != highPriorityInterps[0] && &item != highPriorityInterps[1])
                item.m_fNormalizedWeight = 0.0f;
        }
        return true;
    }
    return false;
}


void __fastcall NiBlendInterpolator_ComputeNormalizedWeights(NiBlendInterpolator *_this)
{
    if (!GetComputeNormalizedWeights(_this))
    {
        return;
    }

    SetComputeNormalizedWeights(_this, false);

    if (_this->m_ucInterpCount == 1)
    {
        _this->m_pkInterpArray[_this->m_ucSingleIdx].m_fNormalizedWeight = 1.0f;
        return;
    }
    else if (_this->m_ucInterpCount == 2)
    {
        ThisStdCall(0xA36BD0, _this); // NiBlendInterpolator::ComputeNormalizedWeightsFor2
        return;
    }

#if 0
    // hacky way to fix flickering when two interpolators share the same priority
    if (ComputeTwoBlendingHighPriorityInterpolators(_this))
        return;
#endif
    unsigned char uc;

    if (_this->m_fHighSumOfWeights == -NI_INFINITY)
    {
        // Compute sum of weights for highest and next highest priorities,
        // along with highest ease spinner for the highest priority.
        _this->m_fHighSumOfWeights = 0.0f;
        _this->m_fNextHighSumOfWeights = 0.0f;
        _this->m_fHighEaseSpinner = 0.0f;
        for (uc = 0; uc < _this->m_ucArraySize; uc++)
        {
            auto& kItem = _this->m_pkInterpArray[uc];
            if (kItem.m_spInterpolator != NULL)
            {
                float fRealWeight = kItem.m_fWeight * kItem.m_fEaseSpinner;
                if (kItem.m_cPriority == _this->m_cHighPriority)
                {
                    _this->m_fHighSumOfWeights += fRealWeight;
                    if (kItem.m_fEaseSpinner > _this->m_fHighEaseSpinner)
                    {
                        _this->m_fHighEaseSpinner = kItem.m_fEaseSpinner;
                    }
                }
                else if (kItem.m_cPriority == _this->m_cNextHighPriority)
                {
                    _this->m_fNextHighSumOfWeights += fRealWeight;
                }
            }
        }
    }


    float fOneMinusHighEaseSpinner = 1.0f - _this->m_fHighEaseSpinner;
    float fTotalSumOfWeights = _this->m_fHighEaseSpinner * _this->m_fHighSumOfWeights +
                               fOneMinusHighEaseSpinner * _this->m_fNextHighSumOfWeights;
    float fOneOverTotalSumOfWeights =
        (fTotalSumOfWeights > 0.0f) ? (1.0f / fTotalSumOfWeights) : 0.0f;



    // Compute normalized weights.
    for (uc = 0; uc < _this->m_ucArraySize; uc++)
    {
        auto &kItem = _this->m_pkInterpArray[uc];
        if (kItem.m_spInterpolator != NULL)
        {
            if (kItem.m_cPriority == _this->m_cHighPriority)
            {
                kItem.m_fNormalizedWeight = _this->m_fHighEaseSpinner *
                                            kItem.m_fWeight * kItem.m_fEaseSpinner *
                                            fOneOverTotalSumOfWeights;
            }
            else if (kItem.m_cPriority == _this->m_cNextHighPriority)
            {
                kItem.m_fNormalizedWeight = fOneMinusHighEaseSpinner *
                                            kItem.m_fWeight * kItem.m_fEaseSpinner *
                                            fOneOverTotalSumOfWeights;
            }
            else
            {
                kItem.m_fNormalizedWeight = 0.0f;
            }
        }
    }

    // Exclude weights below threshold, computing new sum in the process.
    float fSumOfNormalizedWeights = 1.0f;
    if (_this->m_fWeightThreshold > 0.0f)
    {
        fSumOfNormalizedWeights = 0.0f;
        for (uc = 0; uc < _this->m_ucArraySize; uc++)
        {
            auto &kItem = _this->m_pkInterpArray[uc];
            if (kItem.m_spInterpolator != NULL &&
                kItem.m_fNormalizedWeight != 0.0f)
            {
                if (kItem.m_fNormalizedWeight < _this->m_fWeightThreshold)
                {
                    kItem.m_fNormalizedWeight = 0.0f;
                }
                fSumOfNormalizedWeights += kItem.m_fNormalizedWeight;
            }
        }
    }

    // Renormalize weights if any were excluded earlier.
    if (fSumOfNormalizedWeights != 1.0f)
    {
        // Renormalize weights.
        float fOneOverSumOfNormalizedWeights =
            (fSumOfNormalizedWeights > 0.0f) ? (1.0f / fSumOfNormalizedWeights) : 0.0f;

        for (uc = 0; uc < _this->m_ucArraySize; uc++)
        {
            auto &kItem = _this->m_pkInterpArray[uc];
            if (kItem.m_fNormalizedWeight != 0.0f)
            {
                kItem.m_fNormalizedWeight = kItem.m_fNormalizedWeight *
                                            fOneOverSumOfNormalizedWeights;
            }
        }
    }

    // Only use the highest weight, if so directed.
    if (GetOnlyUseHighestWeight(_this))
    {
        float fHighest = -1.0f;
        unsigned char ucHighIndex = INVALID_INDEX;
        for (uc = 0; uc < _this->m_ucArraySize; uc++)
        {
            if (_this->m_pkInterpArray[uc].m_fNormalizedWeight > fHighest)
            {
                ucHighIndex = uc;
                fHighest = _this->m_pkInterpArray[uc].m_fNormalizedWeight;
            }
            _this->m_pkInterpArray[uc].m_fNormalizedWeight = 0.0f;
        }

        // Set the highest index to 1.0
        _this->m_pkInterpArray[ucHighIndex].m_fNormalizedWeight = 1.0f;
    }
}

bool IsInterpHighPriority(NiControllerSequence::ControlledBlock& interp)
{
    return interp.interpolator && interp.blendInterpolator && interp.priority != 0xFF
        && interp.priority == interp.blendInterpolator->m_cHighPriority;
}

void __fastcall NiControllerSequence_AttachInterpolatorsHook(NiControllerSequence* _this, void*,  char cPriority)
{
#if 0
    auto* addressOfReturnAddress = _AddressOfReturnAddress();
    const auto fEaseInTime = NI_GET_CALLER_VAR(float, 0x20, false);
    ThisStdCall(0xA30900, _this, cPriority);
    std::span interpItems(_this->controlledBlocks, _this->numControlledBlocks);
    for (auto& interp: interpItems)
    {
        if (!IsInterpHighPriority(interp))
            continue;
        g_easingInterps.insert(interp.interpolator);
    }
#else
    ThisStdCall(0xA30900, _this, cPriority);
#endif
}

void __fastcall NiControllerSequence_DetachInterpolatorsHook(NiControllerSequence* _this, void*)
{
    ThisStdCall(0xA30540, _this);
    std::span interpItems(_this->controlledBlocks, _this->numControlledBlocks);
    for (auto& interp: interpItems)
    {
        if (!IsInterpHighPriority(interp))
            continue;
        g_easingInterps.erase(interp.interpolator);
    }
}

void FixConflictingPriorities(NiControllerSequence* pkSource, NiControllerSequence* pkDest)
{
    std::span sourceInterpItems(pkSource->controlledBlocks, pkSource->numControlledBlocks);
    std::unordered_map<NiBlendInterpolator*, NiControllerSequence::ControlledBlock*> sourceInterpMap;
    for (auto& interp : sourceInterpItems)
    {
        if (!interp.interpolator || !interp.blendInterpolator || interp.priority == 0xFF)
            continue;
        sourceInterpMap.emplace(interp.blendInterpolator, &interp);
    }
    const std::span destInterpItems(pkDest->controlledBlocks, pkDest->numControlledBlocks);
    for (auto& interp : destInterpItems)
    {
        auto blendInterpolator = interp.blendInterpolator;
        if (!interp.interpolator || !blendInterpolator || interp.priority == 0xFF)
            continue;
        if (auto it = sourceInterpMap.find(blendInterpolator); it != sourceInterpMap.end())
        {
            const auto sourceInterp = *it->second;
            if (interp.priority != sourceInterp.priority || interp.priority != blendInterpolator->m_cHighPriority)
                continue;
            std::span blendInterpItems(blendInterpolator->m_pkInterpArray, blendInterpolator->m_ucArraySize);
            auto destInterpItem = std::ranges::find_if(blendInterpItems, [&](const NiBlendInterpolator::InterpArrayItem& item)
            {
                return item.m_spInterpolator == interp.interpolator;
            });
            if (destInterpItem == blendInterpItems.end())
                continue;
            destInterpItem->m_cPriority++;
            blendInterpolator->m_cHighPriority = destInterpItem->m_cPriority;
            blendInterpolator->m_cNextHighPriority = interp.priority;
        }
    }
}

bool __fastcall CrossFadeHook(NiControllerManager*, void*, NiControllerSequence* pkSourceSequence, NiControllerSequence* pkDestSequence,
    float fEaseInTime, char iPriority, bool bStartOver, float fWeight, NiControllerSequence* pkTimeSyncSeq)
{
    if (pkSourceSequence->state == NiControllerSequence::kAnimState_Inactive
        || pkDestSequence->state != NiControllerSequence::kAnimState_Inactive)
        return false;
    pkSourceSequence->Deactivate(fEaseInTime, false);
    const auto result = ThisStdCall<bool>(0xA34F20, pkDestSequence, iPriority, bStartOver, fWeight, fEaseInTime, pkTimeSyncSeq, false);
    if (result)
        FixConflictingPriorities(pkSourceSequence, pkDestSequence);
    return result;
}

void FixPriorityBug()
{
    if (g_fixBlendSamePriority)
    {
        //WriteRelJump(0xA37260, NiBlendInterpolator_ComputeNormalizedWeights);
        //WriteRelCall(0xA34F71, NiControllerSequence_AttachInterpolatorsHook);
        //WriteRelCall(0xA350C5, NiControllerSequence_DetachInterpolatorsHook);
        WriteRelJump(0xA2E280, CrossFadeHook);
    }
}