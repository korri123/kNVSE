#include "knvse_events.h"

#include "hooks.h"
#include "main.h"
#include "SafeWrite.h"

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
        thread_local static BSAnimGroupSequence* s_result;
        thread_local static bool s_bSkip;
        thread_local static AnimData* s_animData;
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
        }, nullptr, animData->actor, anim->animGroup->groupID & 0xFF, anim->m_kName.CStr());
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

namespace OnActorUpdateAnimation
{
    const char* eventName = "kNVSE:OnActorUpdate";
    
    void Dispatch(Actor* actor)
    {
        g_eventManagerInterface->DispatchEvent(eventName, actor);
    }
}

#define REGISTER_EVENT(event) g_eventManagerInterface->RegisterEvent(event::eventName, std::size(event::params), event::params, EventFlags::kFlags_None)
#define REGISTER_EVENT_NO_PARAMS(event) g_eventManagerInterface->RegisterEvent(event::eventName, 0, nullptr, EventFlags::kFlag_FlushOnLoad)

struct MoveEvent
{
    const char* name;
    const char* endName;
    MovementFlags flag;
};

static constexpr auto moveEvents = std::initializer_list<MoveEvent>{
    {"kNVSE:OnSneak", "kNVSE:OnSneakEnd", kMoveFlag_Sneaking},
    {"kNVSE:OnWalk", "kNVSE:OnWalkEnd", kMoveFlag_Walking},
    {"kNVSE:OnSwim", "kNVSE:OnSwimEnd", kMoveFlag_Swimming},
    {"kNVSE:OnRun", "kNVSE:OnRunEnd", kMoveFlag_Running},
    {"kNVSE:OnJump", "kNVSE:OnJumpEnd", kMoveFlag_Jump},
    {"kNVSE:OnFall", "kNVSE:OnFallEnd", kMoveFlag_Fall},
    {"kNVSE:OnFly", "kNVSE:OnFlyEnd", kMoveFlag_Flying},
    {"kNVSE:OnSlide", "kNVSE:OnSlideEnd", kMoveFlag_Slide},
    {"kNVSE:OnMoveForward", "kNVSE:OnMoveForwardEnd", kMoveFlag_Forward},
    {"kNVSE:OnMoveBackward", "kNVSE:OnMoveBackwardEnd", kMoveFlag_Backward},
    {"kNVSE:OnMoveLeft", "kNVSE:OnMoveLeftEnd", kMoveFlag_Left},
    {"kNVSE:OnMoveRight", "kNVSE:OnMoveRightEnd", kMoveFlag_Right},
    {"kNVSE:OnTurnLeft", "kNVSE:OnTurnLeftEnd", kMoveFlag_TurnLeft},
    {"kNVSE:OnTurnRight", "kNVSE:OnTurnRightEnd", kMoveFlag_TurnRight},
};

void OnMoveFlagsChange(Actor* actor, UInt32 previousFlags, UInt32 newFlags)
{
    if (previousFlags == newFlags)
        return;
    g_eventManagerInterface->DispatchEvent("kNVSE:OnMoveFlagsChange", actor, previousFlags, newFlags);

    const auto checkAndDispatchState = [&](UInt32 flag, const char* startEvent, const char* endEvent)
    {
        if ((previousFlags & flag) == 0 && (newFlags & flag) != 0) 
            g_eventManagerInterface->DispatchEvent(startEvent, actor);
        else if ((previousFlags & flag) != 0 && (newFlags & flag) == 0)
            g_eventManagerInterface->DispatchEvent(endEvent, actor);
    };

    for (const auto& moveEvent : moveEvents)
        checkAndDispatchState(moveEvent.flag, moveEvent.name, moveEvent.endName);
}

void InitMoveEvents()
{
    
    for (const auto& moveEvent : moveEvents)
    {
        g_eventManagerInterface->RegisterEvent(moveEvent.name, 0, nullptr, EventFlags::kFlag_FlushOnLoad);
        g_eventManagerInterface->RegisterEvent(moveEvent.endName, 0, nullptr, EventFlags::kFlag_FlushOnLoad);
    }

    static EventParamType onMoveFlagsChangeParams[] = {
        EventParamType::eParamType_Int, EventParamType::eParamType_Int
    }; 
    g_eventManagerInterface->RegisterEvent("kNVSE:OnMoveFlagsChange", 2, onMoveFlagsChangeParams, EventFlags::kFlag_FlushOnLoad);

    WriteRelCall(0x8B3A0F, INLINE_HOOK(void, __fastcall, ActorMover* actorMover, void*, UInt32 flags)
    {
        const auto previousFlags = actorMover->phMovementFlags1;
        actorMover->SetMovementFlag(flags);
        const auto newFlags = actorMover->phMovementFlags1;
        if (!actorMover->actor)
            return;
        OnMoveFlagsChange(actorMover->actor, previousFlags, newFlags);
    }));

    // player
    SafeWrite32(0x1092988, INLINE_HOOK(void, __fastcall, PlayerMover* actorMover, void*, UInt32 flags)
    {
        const auto previousFlags = actorMover->GetMovementFlags();
        ThisStdCall(0x9EA3E0, actorMover, flags);
        const auto newFlags = actorMover->GetMovementFlags();
        if (!actorMover->actor)
            return;
        OnMoveFlagsChange(actorMover->actor, previousFlags, newFlags);
    }));
}

void Events::RegisterEvents()
{
    REGISTER_EVENT(InterceptPlayAnimGroup);
    REGISTER_EVENT(InterceptStopSequence);
    REGISTER_EVENT_NO_PARAMS(OnActorUpdateAnimation);

    InitMoveEvents();
}

