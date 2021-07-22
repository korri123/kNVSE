#pragma once

enum AnimGroupID : UInt8;

struct AnimationContext
{
	UInt8* basePointer;

	explicit AnimationContext(UInt8* basePointer)
		: basePointer(basePointer)
	{
	}

	AnimGroupID& GroupID() const
	{
		return *reinterpret_cast<AnimGroupID*>(basePointer + 0xC);
	}
};

extern AnimationContext g_animationHookContext;

void ApplyHooks();