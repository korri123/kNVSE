#include "TempEaseSequence.h"
#include "NiObjects.h"
#include "string_view_util.h"

using SequenceList = std::vector<NiControllerSequencePtr>;
thread_local std::unordered_map<NiControllerManager*, SequenceList> g_tempEaseSequences;

#ifdef _DEBUG
#define CONTINUE_OR_DBG DebugBreak()
#else
#define CONTINUE continue
#endif

NiControllerSequence* FindOrCreateTempEaseSequence(NiControllerSequence* baseSequence)
{
    auto& seqList = g_tempEaseSequences[baseSequence->m_pkOwner];
    for (auto& seq : seqList)
    {
        if (seq->m_eState == NiControllerSequence::INACTIVE)
        {
            for (int i = 0; i < seq->m_uiArraySize; ++i)
                seq->RemoveInterpolator(i);
            return seq;
        }
    }
    auto* seq = NiControllerSequence::Create("__TempEaseSequence__", baseSequence->m_uiArraySize, baseSequence->m_uiArrayGrowBy);
    seqList.emplace_back(seq);
    return seq;
}

NiControllerSequence* TempEaseSequence::Create(NiControllerSequence* baseSequence)
{
    auto* tempSequence = FindOrCreateTempEaseSequence(baseSequence);
    
    const auto* idleSequence = baseSequence->m_pkOwner->FindSequence([](const NiControllerSequence* seq)
    {
        if (NOT_TYPE(seq, BSAnimGroupSequence))
            return false;
        auto* anim = static_cast<const BSAnimGroupSequence*>(seq);
        if (!anim->animGroup)
            return false;
        return anim->animGroup->GetSequenceType() == kSequence_Idle;
    });
    if (!idleSequence)
        return nullptr;

    static NiFixedString kTempBlendSequenceName("__TempBlendSequence__");

    const auto* tmpBlendSequence = baseSequence->m_pkOwner->FindSequence([](const NiControllerSequence* seq)
    {
        return seq->m_kName == kTempBlendSequenceName;
    });
        
    unsigned int idx = -1;
    for (auto& kItem : baseSequence->GetControlledBlocks())
    {
        ++idx;
            
        if (!kItem.m_spInterpolator || !kItem.m_spInterpCtlr)
            continue;

        NiQuatTransform kValue;
        const auto& idTag = baseSequence->GetIDTags()[idx];

        const auto pkInterpTarget = kItem.m_spInterpCtlr->GetTargetNode(idTag);
        if (!pkInterpTarget)
            CONTINUE_OR_DBG;

        auto* item = kItem.m_pkBlendInterp->GetItemByInterpolator(kItem.m_spInterpolator);
        if (!item)
            CONTINUE_OR_DBG;

        auto* idleInterp = idleSequence->GetControlledBlock(idTag.m_kAVObjectName);
        if (!idleInterp)
        {
            if (tmpBlendSequence)
                idleInterp = tmpBlendSequence->GetControlledBlock(idTag.m_kAVObjectName);
        }
        NiPointer blendInterp = NiBlendTransformInterpolator::Create();

        if (idleInterp)
        {
            auto* idleItem = idleInterp->m_pkBlendInterp->GetItemByInterpolator(idleInterp->m_spInterpolator);
            if (!idleItem)
                CONTINUE_OR_DBG;
            blendInterp->AddInterpInfo(idleInterp->m_spInterpolator, idleItem->m_fWeight, idleItem->m_cPriority, idleItem->m_fEaseSpinner);
        }
        
        blendInterp->AddInterpInfo(kItem.m_spInterpolator, item->m_fWeight, item->m_cPriority, item->m_fEaseSpinner);

        if (!blendInterp->Update(item->m_fUpdateTime, pkInterpTarget, kValue))
            continue;
            
        auto* newInterpolator = NiTransformInterpolator::Create(kValue);
        const auto newIdx = tempSequence->AddInterpolator(newInterpolator, idTag, kItem.m_ucPriority);

        auto& interpItem = tempSequence->m_pkInterpArray[newIdx];
        interpItem.m_pkBlendInterp = kItem.m_pkBlendInterp;
        interpItem.m_spInterpCtlr = kItem.m_spInterpCtlr;
    }

    // missing interpolators
    idx = -1;
    for (auto& kItem : idleSequence->GetControlledBlocks())
    {
        ++idx;
        if (!kItem.m_spInterpolator || !kItem.m_spInterpCtlr)
            continue;
        auto& idTag = idleSequence->GetIDTags()[idx];
        if (!tempSequence->GetControlledBlock(idTag.m_kAVObjectName))
        {
            NiQuatTransform kValue;
            auto* target = kItem.m_spInterpCtlr->GetTargetNode(idTag);
            if (!target)
                CONTINUE_OR_DBG;
            auto* item = kItem.m_pkBlendInterp->GetItemByInterpolator(kItem.m_spInterpolator);
            if (!item)
                CONTINUE_OR_DBG;
            if (!kItem.m_spInterpolator->Update(item->m_fUpdateTime, target, kValue))
                continue;

            auto* newInterpolator = NiTransformInterpolator::Create(kValue);
            const auto newIdx = tempSequence->AddInterpolator(newInterpolator, idTag, kItem.m_ucPriority + 1);
            auto& interpArrayItem = tempSequence->m_pkInterpArray[newIdx];
            interpArrayItem.m_pkBlendInterp = kItem.m_pkBlendInterp;
            interpArrayItem.m_spInterpCtlr = kItem.m_spInterpCtlr;
        }
    }

    baseSequence->m_pkOwner->AddSequence(tempSequence, nullptr, false);
    return tempSequence;
}
