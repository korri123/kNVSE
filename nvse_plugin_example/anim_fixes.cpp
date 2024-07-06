#include "anim_fixes.h"

#include "class_vtbls.h"
#include "utility.h"

void LogAnimError(const BSAnimGroupSequence* anim, const std::string& msg)
{
	DebugPrint("kNVSE Animation Error: " + std::string(anim->sequenceName) + "\n\t" + msg);
}

void FixInconsistentEndTime(BSAnimGroupSequence* anim)
{
	
	const auto* endKey = anim->textKeyData->FindFirstByName("end");
	if (endKey && anim->endKeyTime > endKey->m_fTime)
	{
		const auto endKeyTime = endKey->m_fTime;
		LogAnimError(anim, FormatString("Stop time is greater than end key time %f > %f", anim->endKeyTime, endKeyTime));
		anim->endKeyTime = endKeyTime;
		for (const auto& block : anim->GetControlledBlocks())
		{
			auto* interpolator = block.interpolator;
			if (interpolator && IS_TYPE(interpolator, NiTransformInterpolator))
			{
				const auto& data = *interpolator->m_spData;

				// this heap of mess is required since they keys are different sizes depending on the type
				const auto updateEndKeyTime = [&](auto getKeyFunction)
				{
					auto keys = (data.*getKeyFunction)();
					if (!keys.empty())
						keys.back().m_fTime = endKeyTime;
				};

				switch (data.m_ePosType)
				{
				case BEZKEY:
					updateEndKeyTime(&NiTransformData::GetPosKeys<NiBezPosKey>);
					break;
				case TCBKEY:
					updateEndKeyTime(&NiTransformData::GetPosKeys<NiTCBPosKey>);
					break;
				default:
					updateEndKeyTime(&NiTransformData::GetPosKeys<NiPosKey>);
					break;
				}
				
				switch (data.m_eRotType)
				{
				case BEZKEY:
					updateEndKeyTime(&NiTransformData::GetRotKeys<NiBezRotKey>);
					break;
				case TCBKEY:
					updateEndKeyTime(&NiTransformData::GetRotKeys<NiTCBRotKey>);
					break;
				case EULERKEY:
					updateEndKeyTime(&NiTransformData::GetRotKeys<NiEulerRotKey>);
					break;
				default:
					updateEndKeyTime(&NiTransformData::GetRotKeys<NiRotKey>);
					break;
				}
				switch (data.m_eScaleType)
				{
				case BEZKEY:
					updateEndKeyTime(&NiTransformData::GetScaleKeys<NiBezPosKey>);
					break;
				case TCBKEY:
					updateEndKeyTime(&NiTransformData::GetScaleKeys<NiTCBPosKey>);
					break;
				default:
					updateEndKeyTime(&NiTransformData::GetScaleKeys<NiFloatKey>);
					break;
				}
			}
		}
	}
}

void FixAnimIfBroken(BSAnimGroupSequence* anim)
{
	FixInconsistentEndTime(anim);
}
