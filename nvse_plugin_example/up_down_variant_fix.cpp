#include "up_down_variant_fix.h"

#include <cstdio>
#include <cstring>
#include <string_view>

#include "GameProcess.h"
#include "NiNodes.h"
#include "SafeWrite.h"

namespace
{
	// AnimSequenceMultiple::GetCorrespondingSequence (0x48F500): the KF of this group that goes with the playing weapon
	// sequence, by file name. eLayer: the section of this group (5 weapon up, 6 weapon down, others: the same name)
	BSAnimGroupSequence* __fastcall GetCorrespondingSequence(AnimSequenceMultiple* group, void*, BSAnimGroupSequence* sequence, int eLayer)
	{
		const char* name = sequence ? sequence->m_kName.CStr() : nullptr;
		const char* fileName = name ? strrchr(name, '\\') : nullptr;
		if (!fileName || !group->anims)
			return nullptr;
		const std::string_view file(fileName);
		const char* suffix = eLayer == kSequence_WeaponUp ? "Up" : eLayer == kSequence_WeaponDown ? "Down" : "";
		// before the first '_' (2haaim_b.kf -> 2haaimUp_b.kf, as the game does), else before the extension
		auto insertAt = file.find('_');
		if (insertAt == std::string_view::npos)
			insertAt = file.rfind('.');
		if (insertAt == std::string_view::npos)
			insertAt = file.size();
		char wanted[MAX_PATH];
		const int length = sprintf_s(wanted, "%.*s%s%.*s", static_cast<int>(insertAt), file.data(), suffix,
			static_cast<int>(file.size() - insertAt), file.data() + insertAt);
		if (length < 0)
			return nullptr;
		for (auto* candidate : *group->anims)
		{
			const char* candidateName = candidate ? candidate->m_kName.CStr() : nullptr;
			const char* candidateFile = candidateName ? strrchr(candidateName, '\\') : nullptr;
			if (candidateFile && _stricmp(candidateFile, wanted) == 0)
				return candidate;
		}
		return nullptr;
	}
}

void UpDownVariantFix::WriteHooks()
{
	WriteRelJump(0x48F500, &GetCorrespondingSequence);
}
