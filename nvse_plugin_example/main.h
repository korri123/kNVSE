#pragma once
#include <functional>
#include <deque>
#include "commands_animation.h"

extern std::deque<std::function<void()>> g_executionQueue;

#define IS_TRANSITION_FIX 0

void Revert3rdPersonAnimTimes(AnimTime& animTime, BSAnimGroupSequence* anim);

bool IsGodMode();
