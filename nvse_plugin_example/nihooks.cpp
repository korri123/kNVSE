#include "nihooks.h"

#include "commands_animation.h"
#include "NiNodes.h"
#include "SafeWrite.h"

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

void NormalizeSamePriorityWeights(NiBlendInterpolator* _this, unsigned char priority, float sumOfWeights) {
    unsigned char count = 0;
    // First, count how many animations have the same priority
    for (unsigned char uc = 0; uc < _this->m_ucArraySize; uc++) {
	    NiBlendInterpolator::InterpArrayItem& interpArrayItem = _this->m_pkInterpArray[uc];
        if (interpArrayItem.m_spInterpolator != NULL &&
            interpArrayItem.m_cPriority == priority) {
            count++;
        }
    }
    // Then, distribute weights equally among them
    float equalWeight = sumOfWeights / static_cast<float>(count);
    for (unsigned char uc = 0; uc < _this->m_ucArraySize; uc++) {
	    NiBlendInterpolator::InterpArrayItem& interpArrayItem = _this->m_pkInterpArray[uc];
        if (interpArrayItem.m_spInterpolator != NULL &&
            interpArrayItem.m_cPriority == priority) {
            interpArrayItem.m_fNormalizedWeight = equalWeight;
        }
    }
}


void __fastcall NiBlendInterpolator_ComputeNormalizedWeights(NiBlendInterpolator *_this)
{
#if _DEBUG
	auto& sequences = g_debugInterpMap[_this];
    std::vector <const char*> interpNames;
    std::vector <const char*> sequenceNames;
	for (int i = 0; i < _this->m_ucArraySize; ++i)
	{
		auto& item = _this->m_pkInterpArray[i];
        if (item.m_spInterpolator)
        {
	        interpNames.emplace_back(g_debugInterpNames[item.m_spInterpolator.data]);
            sequenceNames.emplace_back(g_debugInterpSequences[item.m_spInterpolator.data]);
        }
	}

#endif
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

unsigned char __fastcall AddInterpInfoHook(NiBlendInterpolator* interp, void*, NiInterpolator* spInterp, float fWeight, unsigned char cPriority, float fEaseSpinner)
{
#if _DEBUG 
    auto& sequences = g_debugInterpMap[interp];
    const auto* name = g_debugInterpNames[spInterp];

    std::vector <const char*> interpNames;
    std::vector <const char*> sequenceNames;
    auto* owner = g_debugInterpSequences[spInterp];
    for (int i = 0; i < interp->m_ucArraySize; ++i)
    {
        auto& item = interp->m_pkInterpArray[i];
        if (item.m_spInterpolator)
        {
            interpNames.emplace_back(g_debugInterpNames[item.m_spInterpolator.data]);
            sequenceNames.emplace_back(g_debugInterpSequences[item.m_spInterpolator.data]);
        }
    }


#endif
#if 1
    if (interp->m_ucInterpCount == 2)
    {
        for (int i = 0; i < interp->m_ucInterpCount; ++i)
        {
            auto& item = interp->m_pkInterpArray[i];
            if (!item.m_spInterpolator || item.m_spInterpolator == spInterp)
				continue;
            auto* itemName = g_debugInterpNames[item.m_spInterpolator];
            auto& sequences = g_debugInterpMap[interp];
            auto* owner = g_debugInterpSequences[item.m_spInterpolator.data];
            if (item.m_cPriority == cPriority)
            {
	            // ++cPriority;
                break;
            }
        }
    }
#endif
	const auto result = ThisStdCall<bool>(0xA36970, interp, spInterp, fWeight, cPriority, fEaseSpinner);
    return result;
}

void FixPriorityBug()
{
    //WriteRelJump(0xA37260, NiBlendInterpolator_ComputeNormalizedWeights);
    //WriteVirtualCall(0xA3094E, AddInterpInfoHook);
}