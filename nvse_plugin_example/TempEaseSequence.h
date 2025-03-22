#pragram once

#include "NiNodes.h"
#include "string_view_util.h"

namespace TempEaseSequence
{
    inline NiControllerSequencePtr CreateTempEaseSequence(NiControllerSequence* baseSequence)
    {
        const sv::stack_string<0x400> name("%s_TMP_EASE", sv::get_file_name(baseSequence->m_kName.CStr()).data());
        NiControllerSequencePtr tempSequence = NiControllerSequence::Create(name.c_str(), baseSequence->m_uiArraySize, baseSequence->m_uiArrayGrowBy);

        unsigned int idx = 0;
        for (auto& interpolator : baseSequence->GetControlledBlocks())
        {
            if (!interpolator.m_spInterpolator || !interpolator.m_spInterpCtlr || !interpolator.m_spInterpCtlr->m_pkTarget)
                continue;
            NiQuatTransform kValue;
            const auto fTime = baseSequence->m_fLastScaledTime;
            const auto pkInterpTarget = interpolator.m_spInterpCtlr->m_pkTarget;
            interpolator.m_spInterpolator->Update(fTime, pkInterpTarget, kValue);
            auto* newInterpolator = NiTransformInterpolator::Create(kValue);
            const auto& idTag = baseSequence->GetIDTags()[idx];
            tempSequence->AddInterpolator(newInterpolator, idTag, 0xFF);
            ++idx;
        }

        baseSequence->m_pkOwner->AddSequence(tempSequence, name.c_str(), false);
        return tempSequence;
    }
}
