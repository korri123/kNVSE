#include "nihooks.h"

#include "commands_animation.h"
#include "NiNodes.h"
#include "SafeWrite.h"

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

    unsigned char uc;

    if (_this->m_fHighSumOfWeights == -NI_INFINITY)
    {
        _this->m_fHighSumOfWeights = 0.0f;
        _this->m_fNextHighSumOfWeights = 0.0f;
        _this->m_fHighEaseSpinner = 0.0f;
        float fHighEaseSpinnerWeightedSum = 0.0f;  // Accumulator for weighted ease spinner sum

        for (uc = 0; uc < _this->m_ucArraySize; uc++)
        {
            auto& kItem = _this->m_pkInterpArray[uc];
            if (kItem.m_spInterpolator != NULL)
            {
                float fRealWeight = kItem.m_fWeight * kItem.m_fEaseSpinner;
                if (kItem.m_cPriority == _this->m_cHighPriority)
                {
                    _this->m_fHighSumOfWeights += fRealWeight;
                    fHighEaseSpinnerWeightedSum += kItem.m_fEaseSpinner * kItem.m_fWeight;
                }
                else if (kItem.m_cPriority == _this->m_cNextHighPriority)
                {
                    _this->m_fNextHighSumOfWeights += fRealWeight;
                }
            }
        }

        // Calculate the weighted average of the high ease spinner, if the total weight is not zero
        if (_this->m_fHighSumOfWeights > 0.0f)
        {
            _this->m_fHighEaseSpinner = fHighEaseSpinnerWeightedSum / _this->m_fHighSumOfWeights;
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

void FixPriorityBug()
{
    WriteRelJump(0xA37260, NiBlendInterpolator_ComputeNormalizedWeights);
}