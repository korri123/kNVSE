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
    BSAnimGroupSequence* Dispatch(AnimData* animData, BSAnimGroupSequence* anim, bool& bSkip);
}

namespace InterceptStopSequence
{
    std::optional<bool> Dispatch(Actor* actor, eAnimSequence sequenceType);
}

namespace InterceptSetPlayerMoveFlags
{
    std::optional<UInt32> Dispatch(UInt32 flags);
}

namespace InterceptTurnAnims
{
    std::optional<bool> Dispatch();
}
