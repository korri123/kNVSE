#include "knvse_events.h"

#include "main.h"

extern NVSEEventManagerInterface* g_eventManagerInterface;

using EventParamType = NVSEEventManagerInterface::ParamType;
using EventFlags = NVSEEventManagerInterface::EventFlags;
using DispatchReturn = NVSEEventManagerInterface::DispatchReturn;

namespace InterceptPlayAnimGroup
{
    const char* eventName = "kNVSE:InterceptPlayAnimGroup";
    EventParamType params[] = {
        EventParamType::eParamType_Int
    };
    constexpr auto numParams = std::size(params);

    std::optional<AnimGroupID> Dispatch(Actor* actor, UInt32 groupId)
    {
        static std::optional<AnimGroupID> result;
        // need to initialize static variable to nullopt, otherwise it will be the old value when this function was last called
        result = std::nullopt;
        g_eventManagerInterface->DispatchEventAlt(eventName, [](NVSEArrayVarInterface::Element& callbackResult, void*)
        {
            if (callbackResult.GetType() != NVSEArrayVarInterface::Element::kType_Numeric)
                return true;
            result = static_cast<AnimGroupID>(callbackResult.GetNumber());
            return false;
        }, nullptr, actor, groupId & 0xFF);
        return result;
    }
}

namespace InterceptStopSequence
{
    const char* eventName = "kNVSE:InterceptStopSequence";
    EventParamType params[] = {
        EventParamType::eParamType_Int
    };
    constexpr auto numParams = std::size(params);

    std::optional<bool> Dispatch(Actor* actor, eAnimSequence sequenceType)
    {
        static std::optional<bool> result;
        result = std::nullopt;
        g_eventManagerInterface->DispatchEventAlt(eventName, [](NVSEArrayVarInterface::Element& callbackResult, void*)
        {
            if (callbackResult.GetType() != NVSEArrayVarInterface::Element::kType_Numeric)
                return true;
            result = callbackResult.GetNumber() != 0;
            return false;
        }, nullptr, actor, static_cast<UInt32>(sequenceType));
        return result;
    }
}

void Events::RegisterEvents()
{
    g_eventManagerInterface->RegisterEvent(InterceptPlayAnimGroup::eventName, InterceptPlayAnimGroup::numParams, InterceptPlayAnimGroup::params, EventFlags::kFlag_FlushOnLoad);
    g_eventManagerInterface->RegisterEvent(InterceptStopSequence::eventName, InterceptStopSequence::numParams, InterceptStopSequence::params, EventFlags::kFlag_FlushOnLoad);
}