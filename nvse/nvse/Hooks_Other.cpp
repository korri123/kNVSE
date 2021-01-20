#include "Hooks_Other.h"

#include "Commands_Animation.h"
#include "GameAPI.h"
#include "Commands_UI.h"
#include "ScriptUtils.h"
#include "SafeWrite.h"
#include "Commands_UI.h"
#include "Hooks_Gameplay.h"
#include "Hooks_Animation.h"

#if RUNTIME
namespace OtherHooks
{
	__declspec(naked) void TilesDestroyedHook()
	{
		__asm
		{
			mov g_tilesDestroyed, 1
			// original asm
			pop ecx
			mov esp, ebp
			pop ebp
			ret
		}
	}

	__declspec(naked) void TilesCreatedHook()
	{
		// Eddoursol reported a problem where stagnant deleted tiles got cached
		__asm
		{
			mov g_tilesDestroyed, 1
			pop ecx
			mov esp, ebp
			pop ebp
			ret
		}
	}

	void CleanUpNVSEVars(ScriptEventList* eventList)
	{
		// Prevent leakage of variables that's ScriptEventList gets deleted (e.g. in Effect scripts)
		auto* scriptVars = g_nvseVarGarbageCollectionMap.GetPtr(eventList);
		if (!scriptVars)
			return;
		auto* node = eventList->m_vars;
		while (node)
		{
			if (node->var)
			{
				const auto id = node->var->id;
				const auto type = scriptVars->Get(id);
				switch (type)
				{
				case NVSEVarType::kVarType_String:
					g_StringMap.MarkTemporary(static_cast<int>(node->var->data), true);
					break;
				case NVSEVarType::kVarType_Array:
					//g_ArrayMap.MarkTemporary(static_cast<int>(node->var->data), true);
					g_ArrayMap.RemoveReference(&node->var->data, eventList->m_script->GetModIndex());
					break;
				default:
					break;
				}
			}
			node = node->next;
		}
		g_nvseVarGarbageCollectionMap.Erase(eventList);
	}

	void DeleteEventList(ScriptEventList* eventList)
	{
		CleanUpNVSEVars(eventList);
		ThisStdCall(0x5A8BC0, eventList);
		GameHeapFree(eventList);
	}
	
	ScriptEventList* __fastcall ScriptEventListsDestroyedHook(ScriptEventList *eventList, int EDX, bool doFree)
	{
		DeleteEventList(eventList);
		return eventList;
	}

	bool __fastcall OverrideFirstPersonAnimation(UInt32 animGroupId)
	{
		auto* weaponInfo = (*g_thePlayer)->baseProcess->GetWeaponInfo();
		if (weaponInfo && weaponInfo->weapon)
		{
			auto& map = GetWeaponAnimMap(AnimType::kAnimType_AttackFirstPerson);
			const auto iter = map.find(weaponInfo->weapon->refID);
			if (iter != map.end())
			{
				auto* sequence = iter->second;
				auto* animData = (*g_thePlayer)->firstPersonAnimData;
				GameFuncs::MorphToSequence(animData, sequence, animGroupId, -1);
				return true;
			}
		}
		
		return false;
	}

	__declspec(naked) void FirstPersonAttackAnimHook()
	{
		static auto didOverrideReturnAddress = 0x8946FE;
		static auto didNotOverrideReturnAddress = 0x89464F;
		static auto fn_GetGroupId = 0x47D3B0;
		__asm
		{
			call fn_GetGroupId
			push eax
			movzx ecx, ax
			call OverrideFirstPersonAnimation
			test al, al
			pop eax
			jnz didOverride
			jmp didNotOverrideReturnAddress
		didOverride:
			jmp didOverrideReturnAddress
		}
	}

	AnimType DetermineAnimType(AnimData* animData, UInt8 animGroupMinor, UInt32 i)
	{
		AnimType type = AnimType::kAnimType_Null;
		if (animGroupMinor >= TESAnimGroup::kAnimGroup_AttackLeft && animGroupMinor <= TESAnimGroup::kAnimGroup_AttackThrow8ISDown)
		{
			// third person attack anims
			switch (i)
			{
			case 0: // normal
				type = AnimType::kAnimType_AttackThirdPerson;
				break;
			case 1: // up
				type = AnimType::kAnimType_AttackThirdPersonUp;
				break;
			case 2: // down
				type = AnimType::kAnimType_AttackThirdPersonDown;
				break;
			default:
				return AnimType::kAnimType_Null;
			}
		}
		else if (animData == (*g_thePlayer)->firstPersonAnimData && i == 0)
		{
			// first person aim anims
			switch (animGroupMinor)
			{
			case TESAnimGroup::kAnimGroup_Aim:
				type = AnimType::kAnimType_AimFirstPerson;
				break;
			case TESAnimGroup::kAnimGroup_AimIS:
				type = AnimType::kAnimType_AimFirstPersonIS;
				break;
			default:
				return AnimType::kAnimType_Null;
			}
		}
		else
		{
			// third person aim anims
			switch (i)
			{
			case 0:
				if (animGroupMinor == TESAnimGroup::kAnimGroup_Aim)
					type = AnimType::kAnimType_AimThirdPerson;
				else if (animGroupMinor == TESAnimGroup::kAnimGroup_AimIS)
					type = AnimType::kAnimType_AimThirdPersonIS;
				break;
			case 1:
				if (animGroupMinor == TESAnimGroup::kAnimGroup_AimUp)
					type = AnimType::kAnimType_AimThirdPersonUp;
				else if (animGroupMinor == TESAnimGroup::kAnimGroup_AimISUp)
					type = AnimType::kAnimType_AimThirdPersonISUp;
				break;
			case 2:
				if (animGroupMinor == TESAnimGroup::kAnimGroup_AimDown)
					type = AnimType::kAnimType_AimThirdPersonDown;
				else if (animGroupMinor == TESAnimGroup::kAnimGroup_AimISDown)
					type = AnimType::kAnimType_AimThirdPersonISDown;
				break;
			default:
				return AnimType::kAnimType_Null;
			}
		}
		return type;
	}

	bool __fastcall OverrideThirdPersonAndAimAnimations(Actor* actor, AnimData* animData, UInt32 animGroupId, UInt32 i)
	{
		if (!animData || !actor)
			return false;
		const UInt8 animGroupMinor = animGroupId;
		const auto type = DetermineAnimType(animData, animGroupMinor, i);
		if (type != AnimType::kAnimType_Null && actor)
		{
			auto& map = GetWeaponAnimMap(type);
			auto* weapon = actor->baseProcess->GetWeaponInfo()->weapon;
			const auto iter = map.find(weapon->refID);
			if (iter != map.end())
			{
				auto* sequence = iter->second;
				GameFuncs::MorphToSequence(animData, sequence, animGroupId, -1);
				return true;
			}
		}
		return false;
	}
	
	__declspec(naked) void ThirdPersonAttackAndAnimHook()
	{
		static auto returnAddress = 0x8B2AB4;
		static auto fn_PlayAnimation = 0x494740;
		__asm
		{
			push ecx
			mov edx, [ebp-0x10] // i
			push edx
			mov edx, [ebp-0x1C] // groupId
			push edx
			mov edx, [ebp+0xC] // animdata
			mov ecx, [ebp-0x24] // actor
			call OverrideThirdPersonAndAimAnimations
			test al, al
			pop ecx
			jnz didOverride
			call fn_PlayAnimation
		didOverride:
			jmp returnAddress
		}
	}

	bool OverrideEquipAnimations(AnimData* animData, UInt16 animGroupId, bool firstPerson)
	{
		const UInt8 groupIdMinor = animGroupId;
		auto type = AnimType::kAnimType_Null;
		if (groupIdMinor == TESAnimGroup::kAnimGroup_Equip)
		{
			type = firstPerson ? AnimType::kAnimType_EquipFirstPerson : AnimType::kAnimType_EquipThirdPerson;
		}
		else if (groupIdMinor == TESAnimGroup::kAnimGroup_Unequip)
		{
			type = firstPerson ? AnimType::kAnimType_UnequipFirstPerson : AnimType::kAnimType_UnequipThirdPerson;
		}
		if (type != AnimType::kAnimType_Null && animData && animData->actor)
		{
			auto* actor = animData->actor;
			auto* weaponInfo = actor->baseProcess->GetWeaponInfo();
			if (weaponInfo->weapon)
			{
				auto& map = GetWeaponAnimMap(type);
				const auto iter = map.find(weaponInfo->weapon->refID);
				if (iter != map.end())
				{
					auto* sequence = iter->second;
					auto* realAnimData = firstPerson ? (*g_thePlayer)->firstPersonAnimData : animData;
					GameFuncs::MorphToSequence(realAnimData, sequence, animGroupId, -1);
					return true;
				}
			}
		}
		return false;
	}

	__declspec(naked) void ThirdPersonEquipHook()
	{
		static auto returnAddress = 0x897392;
		static auto fn_PlayAnimation = 0x494740;
		__asm
		{
			push ecx
			// our func
			push 0
			movzx edx, [ebp-0x24] // groupid
			mov ecx, [ebp-0x4] // animdata

			call OverrideEquipAnimations
			test al, al
			pop ecx
			jnz didOverride
			call fn_PlayAnimation
		didOverride:
			jmp returnAddress
		}
	}

	__declspec(naked) void FirstPersonEquipHook()
	{
		static auto returnAddress = 0x8973EE;
		__asm
		{
			push 1
			movzx edx, [ebp - 0x24] // groupid
			mov ecx, [ebp - 0x4] // animdata
			call OverrideEquipAnimations
			test al, al
			jnz didOverride
			
			// original asm
			push 1
			movzx ecx, [ebp-0x24]
			push ecx
			mov edx, [ebp-0x10C]
			mov eax, [edx]
			mov ecx, [ebp-0x10C]
			mov edx, [eax+0x480]
			call edx
		didOverride:
			jmp returnAddress
		}
	}
	
	AnimType GetAttackAimWeaponAnimType(UInt8 animGroupMinor, bool firstPerson)
	{
		UInt8 base = 0;
		AnimType baseAnimType = AnimType::kAnimType_Null;
		if (animGroupMinor >= TESAnimGroup::kAnimGroup_AttackLeft && animGroupMinor <= TESAnimGroup::kAnimGroup_AttackThrow8ISDown)
		{
			base = static_cast<UInt8>(TESAnimGroup::kAnimGroup_AttackLeft);
			baseAnimType = AnimType::kAnimType_AttackThirdPerson;
			if (firstPerson)
				return AnimType::kAnimType_AttackFirstPerson;
		}
		else if (animGroupMinor >= TESAnimGroup::kAnimGroup_Aim && animGroupMinor <= TESAnimGroup::kAnimGroup_AimDown)
		{
			base = static_cast<UInt8>(TESAnimGroup::kAnimGroup_Aim);
			baseAnimType = AnimType::kAnimType_AimThirdPerson;
			if (firstPerson)
				return AnimType::kAnimType_AimFirstPerson;
		}
		else if (animGroupMinor >= TESAnimGroup::kAnimGroup_AimIS && animGroupMinor <= TESAnimGroup::kAnimGroup_AimISDown)
		{
			base = static_cast<UInt8>(TESAnimGroup::kAnimGroup_AimIS);
			baseAnimType = AnimType::kAnimType_AimThirdPersonIS;
			if (firstPerson)
				return AnimType::kAnimType_AimFirstPersonIS;
		}
		if (base)
		{
			const auto type = (animGroupMinor - base) % 3;
			return static_cast<AnimType>(static_cast<int>(baseAnimType) + type);
		}
		return AnimType::kAnimType_Null;
	}

	AnimType GetAnimType(AnimData* animData, UInt32 animGroupId)
	{
		const UInt8 animGroupMinor = animGroupId;
		const auto firstPerson = animData == (*g_thePlayer)->firstPersonAnimData;

		// attack, aim, aimIS animations
		const auto aimAttackType = GetAttackAimWeaponAnimType(animGroupMinor, firstPerson);
		if (aimAttackType != AnimType::kAnimType_Null)
			return aimAttackType;

		// equip animations
		if (animGroupMinor == TESAnimGroup::kAnimGroup_Equip)
			return firstPerson ? AnimType::kAnimType_EquipFirstPerson : AnimType::kAnimType_EquipThirdPerson;
		
		if (animGroupMinor == TESAnimGroup::kAnimGroup_Unequip)
			return firstPerson ? AnimType::kAnimType_UnequipFirstPerson : AnimType::kAnimType_UnequipThirdPerson;

		// reload animations
		if (animGroupMinor >= TESAnimGroup::kAnimGroup_ReloadA && animGroupMinor <= TESAnimGroup::kAnimGroup_ReloadZ)
		{
			return firstPerson ? AnimType::kAnimType_ReloadFirstPerson : AnimType::kAnimType_ReloadThirdPerson;
		}

		if (animGroupMinor >= TESAnimGroup::kAnimGroup_ReloadWStart && animGroupMinor <= TESAnimGroup::kAnimGroup_ReloadZStart)
		{
			return firstPerson ? AnimType::kAnimType_ReloadStartFirstPerson : AnimType::kAnimType_ReloadStartThirdPerson;
		}

		// jam animations
		if (animGroupMinor >= TESAnimGroup::kAnimGroup_JamA && animGroupMinor <= TESAnimGroup::kAnimGroup_JamZ)
		{
			return firstPerson ? AnimType::kAnimType_JamFirstPerson : AnimType::kAnimType_JamThirdPerson;
		}
		
		return AnimType::kAnimType_Null;
	}

	void __fastcall HandleAnimationChange(AnimData* animData, UInt32 animGroupId, BSAnimGroupSequence** toMorph)
	{
		if (animData && animData->actor)
		{
			const auto animGroupMinor = static_cast<UInt8>(animGroupId);
			auto* weaponInfo = animData->actor->baseProcess->GetWeaponInfo();
			const auto firstPerson = animData == (*g_thePlayer)->firstPersonAnimData;
			if (weaponInfo && weaponInfo->weapon)
			{
				auto* anim = GetWeaponAnimation(weaponInfo->weapon->refID, animGroupMinor, firstPerson);
				if (anim)
				{
					*toMorph = anim;
					return;
				}
			}
			auto* actorAnim = GetActorAnimation(animData->actor->refID, animGroupMinor, firstPerson);
			if (actorAnim)
			{
				*toMorph = actorAnim;
			}
		}
	}

	__declspec(naked) void AnimationHook()
	{
		static auto fn_AnimDataContainsAnimSequence = 0x498EA0;
		static auto returnAddress = 0x4949D5;
		__asm
		{
			push ecx
			lea ecx, [ebp + 0x8] // BSAnimGroupSequence* toMorph
			push ecx
			mov edx, [ebp + 0xC] // animGroupId
			mov ecx, [ebp - 0x5C] // animData
			call HandleAnimationChange
			pop ecx
			call fn_AnimDataContainsAnimSequence
			jmp returnAddress
		}
	}

	void Hooks_Other_Init()
	{
		WriteRelJump(0x9FF5FB, UInt32(TilesDestroyedHook));
		WriteRelJump(0x709910, UInt32(TilesCreatedHook));
		WriteRelJump(0x41AF70, UInt32(ScriptEventListsDestroyedHook));

		//WriteRelJump(0x894667, UInt32(WeaponAnimationHook));
		//WriteRelJump(0x8B2AAF, UInt32(WeaponAnimationHook2));
		//WriteRelJump(0x495E2A, UInt32(FPSAnimHook));
		//WriteRelJump(0x89464A, UInt32(FirstPersonAttackAnimHook));
		//WriteRelJump(0x8B2AAF, UInt32(ThirdPersonAttackAndAnimHook));


		WriteRelJump(0x4949D0, UInt32(AnimationHook));
	}
}
#endif