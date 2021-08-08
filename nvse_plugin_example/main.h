#pragma once
#include <functional>
#include <deque>

extern std::deque<std::function<void()>> g_executionQueue;

#define IS_TRANSITION_FIX 0

bool IsGodMode();
