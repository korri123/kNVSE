#include "knvse_events.h"

#include "hooks.h"
#include "main.h"

extern NVSEEventManagerInterface* g_eventManagerInterface;

using EventParamType = NVSEEventManagerInterface::ParamType;
using EventFlags = NVSEEventManagerInterface::EventFlags;
using DispatchReturn = NVSEEventManagerInterface::DispatchReturn;

namespace InterceptPlayAnimGroup
{
    const char* eventName = "kNVSE:InterceptPlayAnimGroup";
    EventParamType params[] = {
        EventParamType::eParamType_Int,
        EventParamType::eParamType_String
    };

    BSAnimGroupSequence* Dispatch(AnimData* animData, BSAnimGroupSequence* anim, bool& bSkip)
    {
        if (!anim || !anim->animGroup)
            return nullptr;
        static BSAnimGroupSequence* s_result;
        static bool s_bSkip;
        static AnimData* s_animData;
        // need to initialize static variable to null, otherwise it will be the old value when this function was last called
        s_result = nullptr;
        s_animData = animData;
        s_bSkip = false;
        g_eventManagerInterface->DispatchEventAlt(eventName, [](NVSEArrayVarInterface::Element& callbackResult, void*)
        {
            if (callbackResult.GetType() == NVSEArrayVarInterface::Element::kType_Numeric)
            {
                const auto groupId = static_cast<AnimGroupID>(callbackResult.GetNumber());
                if (groupId == static_cast<AnimGroupID>(0xFF))
                {
                    s_bSkip = true;
                    return false;
                }
                s_result = GetAnimByGroupID(s_animData, groupId);
                return false;
            }
            if (callbackResult.GetType() == NVSEArrayVarInterface::Element::kType_String)
            {
                if (auto* str = callbackResult.GetString())
                {
                    s_result = FindOrLoadAnim(s_animData, str);
                    if (s_result)
                        return false;
                }
            }
            return true;
        }, nullptr, animData->actor, anim->animGroup->groupID & 0xFF, anim->m_kName);
        bSkip = s_bSkip;
        return s_result;
    }
}

namespace InterceptStopSequence
{
    const char* eventName = "kNVSE:InterceptStopSequence";
    EventParamType params[] = {
        EventParamType::eParamType_Int
    };

    std::optional<bool> Dispatch(Actor* actor, eAnimSequence sequenceType)
    {
        static std::optional<bool> result;
        result = std::nullopt;
        g_eventManagerInterface->DispatchEventAlt(eventName, [](NVSEArrayVarInterface::Element& callbackResult, void*)
        {
            if (callbackResult.GetType() != NVSEArrayVarInterface::Element::kType_Numeric)
                return true;
            result = callbackResult.GetNumber() != 0.0;
            return false;
        }, nullptr, actor, static_cast<UInt32>(sequenceType));
        return result;
    }
}

#define REGISTER_EVENT(event) g_eventManagerInterface->RegisterEvent(event::eventName, std::size(event::params), event::params, EventFlags::kFlag_FlushOnLoad)
#define REGISTER_EVENT_NO_PARAMS(event) g_eventManagerInterface->RegisterEvent(event::eventName, 0, nullptr, EventFlags::kFlag_FlushOnLoad)

void Events::RegisterEvents()
{
    REGISTER_EVENT(InterceptPlayAnimGroup);
    REGISTER_EVENT(InterceptStopSequence);
}