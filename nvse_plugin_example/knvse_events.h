#pragma once
#include <optional>

#include "commands_animation.h"
#include "GameObjects.h"

namespace Events
{
    void RegisterEvents();
}

namespace InterceptPlayAnimGroup
{
    std::optional<AnimGroupID> Dispatch(Actor* actor, UInt32 groupId);
}

namespace InterceptStopSequence
{
    std::optional<bool> Dispatch(Actor* actor, eAnimSequence sequenceType);
}
