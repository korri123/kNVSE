#include "accum_blend_fix.h"

#include <optional>

#include "hooks.h"
#include "NiNodes.h"
#include "SafeWrite.h"

namespace
{
	// NiBlendAccumTransformInterpolator::AddInterpInfo. When a second animation joins, the playing one keeps its last
	// value, with the reference frame for its deltas set as CalculateValue would (Gamebryo 2.3 behavior).
	UInt8 __fastcall AccumAddInterpInfoHook(NiBlendAccumTransformInterpolator* blend, void*, NiInterpolator* interpolator, float fWeight, char cPriority, float fEaseSpinner)
	{
		const UInt8 ucSingleIdx = blend->m_ucSingleIdx;
		std::optional<NiBlendAccumTransformInterpolator::AccumArrayItem> single;
		if (g_pluginSettings.fixAccumRootBlend && blend->m_ucInterpCount == 1 && ucSingleIdx != NiBlendInterpolator::INVALID_INDEX &&
			!blend->m_kAccumulatedTransformValue.IsTransformInvalid() && !blend->m_pkAccumArray[ucSingleIdx].m_kLastValue.IsTransformInvalid())
			single = blend->m_pkAccumArray[ucSingleIdx];
		// Calls the engine's AddInterpInfo by address, since the virtual call would come back here through the vtable
		const auto ucIndex = ThisStdCall<UInt8>(0xA392B0, blend, interpolator, fWeight, cPriority, fEaseSpinner);
		if (single && ucIndex != NiBlendInterpolator::INVALID_INDEX && blend->m_ucInterpCount == 2)
		{
			// The accumulated transform has taken the single's last value
			auto& item = blend->m_pkAccumArray[ucSingleIdx];
			item.m_fLastTime = single->m_fLastTime;
			item.m_kLastValue = single->m_kLastValue;
			const auto& accumulated = blend->m_kAccumulatedTransformValue;
			const NiQuaternion accumRotate = accumulated.IsRotateValid() ? accumulated.m_kRotate : NiQuaternion::IDENTITY;
			const NiQuaternion invLast = single->m_kLastValue.IsRotateValid() ? single->m_kLastValue.m_kRotate.Inverse() : NiQuaternion::IDENTITY;
			(accumRotate * invLast).ToRotation(item.m_kRefFrame);
		}
		return ucIndex;
	}

	// NiBlendAccumTransformInterpolator::RemoveInterpInfo. When one of two animations leaves, the remaining one
	// continues from its last value: the accumulated transform (the blend's output so far) takes its inverse, as
	// StoreSingleValue does on reset, but from the last update instead of the current one.
	void __fastcall AccumRemoveInterpInfoHook(NiBlendAccumTransformInterpolator* blend, void*, UInt8 ucIndex)
	{
		UInt8 ucRemaining = NiBlendInterpolator::INVALID_INDEX;
		std::optional<NiBlendAccumTransformInterpolator::AccumArrayItem> remaining;
		if (g_pluginSettings.fixAccumRootBlend && blend->m_ucInterpCount == 2 && !blend->m_kAccumulatedTransformValue.IsTransformInvalid())
		{
			for (UInt8 i = 0; i < blend->m_ucArraySize; ++i)
			{
				if (i != ucIndex && blend->m_pkInterpArray[i].m_spInterpolator)
				{
					ucRemaining = i;
					break;
				}
			}
			if (ucRemaining != NiBlendInterpolator::INVALID_INDEX && !blend->m_pkAccumArray[ucRemaining].m_kLastValue.IsTransformInvalid())
				remaining = blend->m_pkAccumArray[ucRemaining];
		}
		// Calls the engine's RemoveInterpInfo by address, as above
		ThisStdCall(0xA393C0, blend, ucIndex);
		if (remaining && blend->m_ucInterpCount == 1 && blend->m_ucSingleIdx == ucRemaining && blend->m_bReset)
		{
			auto& item = blend->m_pkAccumArray[ucRemaining];
			item.m_fLastTime = remaining->m_fLastTime;
			item.m_kLastValue = remaining->m_kLastValue;
			blend->m_kAccumulatedTransformValue = blend->m_kAccumulatedTransformValue.HierApply(remaining->m_kLastValue.HierInvert());
			blend->m_bReset = false;
		}
	}

	// NiBlendAccumTransformInterpolator::CalculateValue, called from UpdateTransform. An animation joining the blend has
	// no motion on its first update, but Gamebryo 2.2 gives it a zero delta, which counts at its weight and slows the
	// actor. The delta is made invalid instead, so BlendValues leaves it out and gives its weight to the others, as for
	// any channel an animation lacks.
	void __fastcall AccumCalculateValueHook(NiBlendAccumTransformInterpolator* blend, void*, UInt8 ucIndex, float fTime, NiObjectNET* pkTarget)
	{
		const bool bFirst = g_pluginSettings.fixAccumRootBlend && blend->m_pkAccumArray[ucIndex].m_fLastTime == -NI_INFINITY;
		blend->CalculateValue(ucIndex, fTime, pkTarget);
		if (bFirst)
			blend->m_pkAccumArray[ucIndex].m_kDeltaValue.MakeInvalid();
	}
}

void AccumBlendFix::WriteHooks()
{
	// NiBlendAccumTransformInterpolator vtable
	SafeWrite32(0x1096860, reinterpret_cast<UInt32>(AccumAddInterpInfoHook));
	SafeWrite32(0x1096864, reinterpret_cast<UInt32>(AccumRemoveInterpInfoHook));
	// NiBlendAccumTransformInterpolator::UpdateTransform
	WriteRelCall(0xA3A627, AccumCalculateValueHook);
}
