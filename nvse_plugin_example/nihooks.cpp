#include "nihooks.h"

#include "commands_animation.h"
#include "NiNodes.h"
#include "SafeWrite.h"

#if 0
bool doOnce = false;
BSAnimGroupSequence* g_anim = nullptr;
NiBlendInterpolator* g_armInterp = nullptr;
#endif

__declspec(noinline) void __fastcall NiBlendInterpolator_ComputeNormalizedWeightsFor2(NiBlendInterpolator* _this)
{
#if 0
    if (!g_armInterp)
    {
        if (!g_anim)
        {
            auto res = GetActorAnimation(0x514, true, g_thePlayer->firstPersonAnimData);
            if (res)
                g_anim = LoadAnimationPath(*res, g_thePlayer->firstPersonAnimData, 0x514);

        }
        if (g_anim)
        {
            std::span idTags{ g_anim->IDTagArray, g_anim->numControlledBlocks };
            auto iter = ra::find_if(idTags, _L(NiControllerSequence::IDTag& t, _stricmp(t.m_kAVObjectName.CStr(), "Bip01 L UpperArm") == 0));
            if (iter != idTags.end())
            {
                auto idx = iter - idTags.begin();
                g_armInterp = g_anim->controlledBlocks[idx].blendInterpolator;
            }
        }
    }
    if (auto* anim = g_thePlayer->firstPersonAnimData->animSequence[kSequence_Weapon])
    {
        if (anim->animGroup->GetBaseGroupID() == kAnimGroup_AimIS && anim->state != NiControllerSequence::kAnimState_Animating)
        {
            if (_this == g_armInterp)
            {
                _MESSAGE("%g %g", pkItem1->m_fNormalizedWeight, pkItem2->m_fNormalizedWeight);
            }
        }
    }

#endif
    

    NiBlendInterpolator::InterpArrayItem* pkItem1 = NULL;
    NiBlendInterpolator::InterpArrayItem* pkItem2 = NULL;

    // Get pointers to the two items.
    for (unsigned char uc = 0; uc < _this->m_ucArraySize; uc++)
    {
        if (_this->m_pkInterpArray[uc].m_spInterpolator)
        {
            if (!pkItem1)
            {
                pkItem1 = &_this->m_pkInterpArray[uc];
                continue;
            }
            else if (!pkItem2)
            {
                pkItem2 = &_this->m_pkInterpArray[uc];
                break;
            }
        }
    }

    // Calculate the real weight of each item.
    float fRealWeight1 = pkItem1->m_fWeight * pkItem1->m_fEaseSpinner;
    float fRealWeight2 = pkItem2->m_fWeight * pkItem2->m_fEaseSpinner;
    if (fRealWeight1 == 0.0f && fRealWeight2 == 0.0f)
    {
        pkItem1->m_fNormalizedWeight = 0.0f;
        pkItem2->m_fNormalizedWeight = 0.0f;
        return;
    }

    // Compute normalized weights.
    if (pkItem1->m_cPriority > pkItem2->m_cPriority)
    {
        if (pkItem1->m_fEaseSpinner == 1.0f)
        {
            pkItem1->m_fNormalizedWeight = 1.0f;
            pkItem2->m_fNormalizedWeight = 0.0f;
            return;
        }

        float fOneMinusEaseSpinner = 1.0f - pkItem1->m_fEaseSpinner;
        float fOneOverSumOfWeights = 1.0f / (pkItem1->m_fEaseSpinner *
            fRealWeight1 + fOneMinusEaseSpinner * fRealWeight2);
        pkItem1->m_fNormalizedWeight = pkItem1->m_fEaseSpinner * fRealWeight1
            * fOneOverSumOfWeights;
        pkItem2->m_fNormalizedWeight = fOneMinusEaseSpinner * fRealWeight2 *
            fOneOverSumOfWeights;
    }
    else if (pkItem1->m_cPriority < pkItem2->m_cPriority)
    {
        if (pkItem2->m_fEaseSpinner == 1.0f)
        {
            pkItem1->m_fNormalizedWeight = 0.0f;
            pkItem2->m_fNormalizedWeight = 1.0f;
            return;
        }

        float fOneMinusEaseSpinner = 1.0f - pkItem2->m_fEaseSpinner;
        float fOneOverSumOfWeights = 1.0f / (pkItem2->m_fEaseSpinner *
            fRealWeight2 + fOneMinusEaseSpinner * fRealWeight1);
        pkItem1->m_fNormalizedWeight = fOneMinusEaseSpinner * fRealWeight1 *
            fOneOverSumOfWeights;
        pkItem2->m_fNormalizedWeight = pkItem2->m_fEaseSpinner * fRealWeight2
            * fOneOverSumOfWeights;
    }
    else
    {
        float fOneOverSumOfWeights = 1.0f / (fRealWeight1 + fRealWeight2);
        pkItem1->m_fNormalizedWeight = fRealWeight1 * fOneOverSumOfWeights;
        pkItem2->m_fNormalizedWeight = fRealWeight2 * fOneOverSumOfWeights;
    }

    // Only use the highest weight, if so desired.
    if ((_this->m_uFlags & 2) != 0)
    {
        if (pkItem1->m_fNormalizedWeight >= pkItem2->m_fNormalizedWeight)
        {
            pkItem1->m_fNormalizedWeight = 1.0f;
            pkItem2->m_fNormalizedWeight = 0.0f;
        }
        else
        {
            pkItem1->m_fNormalizedWeight = 0.0f;
            pkItem2->m_fNormalizedWeight = 1.0f;
        }
        return;
    }

    // Exclude weights below threshold.
    if (_this->m_fWeightThreshold > 0.0f)
    {
        bool bReduced1 = false;
        if (pkItem1->m_fNormalizedWeight < _this->m_fWeightThreshold)
        {
            pkItem1->m_fNormalizedWeight = 0.0f;
            bReduced1 = true;
        }

        bool bReduced2 = false;
        if (pkItem2->m_fNormalizedWeight < _this->m_fWeightThreshold)
        {
            pkItem2->m_fNormalizedWeight = 0.0f;
            bReduced1 = true;
        }

        if (bReduced1 && bReduced2)
        {
            return;
        }
        else if (bReduced1)
        {
            pkItem2->m_fNormalizedWeight = 1.0f;
            return;
        }
        else if (bReduced2)
        {
            pkItem1->m_fNormalizedWeight = 1.0f;
            return;
        }
    }
}

constexpr auto NI_INFINITY = -FLT_MAX;

void __fastcall NiBlendInterpolator_ComputeNormalizedWeights(NiBlendInterpolator* _this)
{
    const auto m_uFlags = _this->m_uFlags;
    if ((_this->m_uFlags & 4) == 0)
        return;

    _this->m_uFlags = m_uFlags & 0xFB;
    if (_this->m_ucInterpCount == 1)
    {
        _this->m_pkInterpArray[_this->m_ucSingleIdx].m_fNormalizedWeight = 1.0f;
        return;
    }
    else if (_this->m_ucInterpCount == 2)
    {
        NiBlendInterpolator_ComputeNormalizedWeightsFor2(_this);
        return;
    }

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
        auto& kItem = _this->m_pkInterpArray[uc];
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
            auto& kItem = _this->m_pkInterpArray[uc];
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
            (fSumOfNormalizedWeights > 0.0f) ?
            (1.0f / fSumOfNormalizedWeights) : 0.0f;

        for (uc = 0; uc < _this->m_ucArraySize; uc++)
        {
            auto& kItem = _this->m_pkInterpArray[uc];
            if (kItem.m_fNormalizedWeight != 0.0f)
            {
                kItem.m_fNormalizedWeight = kItem.m_fNormalizedWeight *
                    fOneOverSumOfNormalizedWeights;
            }
        }
    }

    // Only use the highest weight, if so directed.
    if ((_this->m_uFlags & 2) != 0)
    {
        float fHighest = -1.0f;
        unsigned char ucHighIndex = 0xFF;
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

void ApplyNiHooks()
{
    //WriteRelJump(0xA36BD0, NiBlendInterpolator_ComputeNormalizedWeightsFor2);
    //WriteRelJump(0xA37260, NiBlendInterpolator_ComputeNormalizedWeights);
}