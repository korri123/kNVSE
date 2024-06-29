#pragma once
#include <optional>

#include "GameObjects.h"

namespace Events
{
    void RegisterEvents();
}

namespace InterceptPlayAnimGroup
{
    std::optional<AnimGroupID> Dispatch(Actor* actor, UInt32 groupId);
}
