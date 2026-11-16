#include "bone_evaluation.h"

#include <cstring>

#include "NiObjects.h"

namespace
{
    thread_local BoneEvaluation::Pass* t_current = nullptr;
}

BoneEvaluation::Pass::Pass(NiMultiTargetTransformController& controller, float fTime, Rule rule)
    : m_controller(controller), m_fTime(fTime), m_rule(rule), m_outer(t_current)
{
    const unsigned int words = (controller.m_usNumInterps + 31u) / 32u;
    if (controller.m_usNumInterps <= kInlineSlots)
        m_done = m_inlineDone;
    else
    {
        m_heapDone = std::make_unique<UInt32[]>(words);
        m_done = m_heapDone.get();
    }
    std::memset(m_done, 0, words * sizeof(UInt32));
    t_current = this;
}

BoneEvaluation::Pass::~Pass()
{
    t_current = m_outer;
}

void BoneEvaluation::Pass::Run()
{
    for (unsigned short us = 0; us < m_controller.m_usNumInterps; us++)
    {
        if (!IsDone(us) && Updates(us))
            Evaluate(us);
    }
}

bool BoneEvaluation::Pass::Updates(unsigned short slot) const
{
    const auto* target = m_controller.m_ppkTargets[slot];
    if (!target)
        return false;
    const bool bTargetSelective = target->GetSelectiveUpdate();
    switch (m_rule)
    {
    case Rule::AllButTarget:
    case Rule::AllButTargetSelective:
    {
        const bool bSelective = m_rule == Rule::AllButTargetSelective;
        if (slot == m_controller.m_usNumInterps - 1)
            return bSelective || bTargetSelective;
        return bSelective != bTargetSelective;
    }
    case Rule::Selected:
        return bTargetSelective;
    }
    return false;
}

bool BoneEvaluation::Pass::IsDone(unsigned short slot) const
{
    return (m_done[slot / 32u] >> (slot % 32u)) & 1u;
}

void BoneEvaluation::Pass::Evaluate(unsigned short slot)
{
    // Marked first: a bone that requires itself through others reads its local transform as it is
    m_done[slot / 32u] |= 1u << (slot % 32u);
    m_controller.UpdateTarget(slot, m_fTime);
}

bool BoneEvaluation::Require(const NiAVObject* node)
{
    Pass* pass = t_current;
    if (!pass || !node)
        return false;
    const auto& controller = pass->m_controller;
    for (unsigned short us = 0; us < controller.m_usNumInterps; us++)
    {
        if (controller.m_ppkTargets[us] != node)
            continue;
        if (pass->IsDone(us))
            return true;
        if (!pass->Updates(us))
            return false;
        pass->Evaluate(us);
        return true;
    }
    return true;
}
