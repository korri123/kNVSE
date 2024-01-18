#pragma once
#include <unordered_set>

#include "commands_animation.h"

struct AnimData;
class BSAnimGroupSequence;
enum AnimGroupID : UInt8;

extern bool g_startedAnimation;
extern BSAnimGroupSequence* g_lastLoopSequence;
void ApplyHooks();
extern bool g_doNotSwapAnims;

BSAnimGroupSequence* GetAnimByGroupID(AnimData* animData, AnimGroupID ani);
void ApplyDestFrame(NiControllerSequence* sequence, float destFrame);

extern bool g_fixSpineBlendBug;
extern bool g_fixAttackISTransition;
extern bool g_fixBlendSamePriority;
extern bool g_fixLoopingReloadStart;
extern bool g_disableFirstPersonTurningAnims;
