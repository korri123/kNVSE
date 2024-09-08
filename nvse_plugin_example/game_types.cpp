#include "game_types.h"

#include "commands_animation.h"
#include "GameData.h"
#include "GameOSDepend.h"
#include "GameProcess.h"
#include "NiObjects.h"
#include <span>

#include "hooks.h"

bool AnimSequenceBase::Contains(BSAnimGroupSequence* anim)
{
	if (this->IsSingle())
	{
		auto* single = static_cast<AnimSequenceSingle*>(this);
		return single->anim == anim;
	}
	const auto* multi = static_cast<AnimSequenceMultiple*>(this);
	return std::any_of(multi->anims->begin(), multi->anims->end(), [&](auto* mAnim) { return mAnim == anim; });		
}

bool AnimSequenceBase::Contains(const char* path)
{
	if (this->IsSingle())
	{
		auto* single = static_cast<AnimSequenceSingle*>(this);
		return single->anim->m_kName.CStr() == path;
	}
	const auto* multi = static_cast<AnimSequenceMultiple*>(this);
	return std::any_of(multi->anims->begin(), multi->anims->end(), [&](auto* mAnim) { return mAnim->m_kName.CStr() == path; });
}

AnimGroupID AnimData::GetNextAttackGroupID() const
{
	const auto type = ThisStdCall<char>(0x495E40, this, 0);
	switch (type)
	{
	case '3':
		return kAnimGroup_Attack3;
	case '4':
		return kAnimGroup_Attack4;
	case '5':
		return kAnimGroup_Attack5;
	case '6':
		return kAnimGroup_Attack6;
	case '7':
		return kAnimGroup_Attack7;
	case '8':
		return kAnimGroup_Attack8;
	case 'l':
		return kAnimGroup_AttackLeft;
	default:
		TESObjectWEAP* weap;
		if (this->actor->baseProcess->GetWeaponInfo() && (weap = this->actor->baseProcess->GetWeaponInfo()->weapon))
		{
			if (weap->attackAnim != 0xFF)
				return static_cast<AnimGroupID>(weap->attackAnim);
		}
		return kAnimGroup_AttackRight;
	}
}


void Actor::FireWeapon()
{
	nextAttackAnimGroupId110 = static_cast<UInt32>(GetAnimData()->GetNextAttackGroupID());
	this->baseProcess->SetQueuedIdleFlag(kIdleFlag_FireWeapon);
	GameFuncs::HandleQueuedAnimFlags(this); //Actor::HandleQueuedIdleFlags
}

void Actor::EjectFromWeapon(TESObjectWEAP* weapon)
{
	if (weapon && !weapon->IsMeleeWeapon())
	{
		// eject
		baseProcess->SetQueuedIdleFlag(kIdleFlag_AttackEjectEaseInFollowThrough);
		GameFuncs::HandleQueuedAnimFlags(this);
	}
}

TESObjectWEAP* Actor::GetWeaponForm() const
{
	auto* weaponInfo = this->baseProcess->GetWeaponInfo();
	if (!weaponInfo)
		return nullptr;
	return weaponInfo->weapon;
}

OSInputGlobals** g_inputGlobals = reinterpret_cast<OSInputGlobals**>(0x11F35CC);


bool TESObjectWEAP::IsMeleeWeapon() const
{
	return this->eWeaponType >= 0 && this->eWeaponType <= 2;
}

std::span<NiControllerSequence::InterpArrayItem> NiControllerSequence::GetControlledBlocks() const
{
	return { m_pkInterpArray, m_uiArraySize };
}

std::span<NiControllerSequence::IDTag> NiControllerSequence::GetIDTags() const
{
	return { m_pkIDTagArray, m_uiArraySize };
}

NiControllerSequence::InterpArrayItem* NiControllerSequence::GetControlledBlock(const char* name) const
{
	std::span idTags(m_pkIDTagArray, m_uiArraySize);
	const auto it = std::ranges::find_if(idTags, [&](const IDTag& tag) { return strcmp(tag.m_kAVObjectName.CStr(), name) == 0; });
	if (it != idTags.end())
	{
		return m_pkInterpArray + std::distance(idTags.begin(), it);
	}
	return nullptr;
}

BSAnimGroupSequence* BSAnimGroupSequence::Get3rdPersonCounterpart() const
{
	auto* animData = g_thePlayer->baseProcess->GetAnimData();
	return GetAnimByFullGroupID(animData, this->animGroup->groupID);
}


bool TESAnimGroup::IsAttackIS()
{
	const auto idMinor = static_cast<AnimGroupID>(groupID & 0xFF);
	switch (idMinor)
	{
	case kAnimGroup_AttackLeftIS: break;
	case kAnimGroup_AttackRightIS: break;
	case kAnimGroup_Attack3IS: break;
	case kAnimGroup_Attack4IS: break;
	case kAnimGroup_Attack5IS: break;
	case kAnimGroup_Attack6IS: break;
	case kAnimGroup_Attack7IS: break;
	case kAnimGroup_Attack8IS: break;
	case kAnimGroup_AttackLoopIS: break;
	case kAnimGroup_AttackSpinIS: break;
	case kAnimGroup_AttackSpin2IS: break;
	case kAnimGroup_PlaceMineIS: break;
	case kAnimGroup_PlaceMine2IS: break;
	case kAnimGroup_AttackThrowIS: break;
	case kAnimGroup_AttackThrow2IS: break;
	case kAnimGroup_AttackThrow3IS: break;
	case kAnimGroup_AttackThrow4IS: break;
	case kAnimGroup_AttackThrow5IS: break;
	case kAnimGroup_Attack9IS: break;
	case kAnimGroup_AttackThrow6IS: break;
	case kAnimGroup_AttackThrow7IS: break;
	case kAnimGroup_AttackThrow8IS: break;
	default: return false;
	}
	return true;
}

bool TESAnimGroup::IsAttackNonIS()
{
	const auto idMinor = static_cast<AnimGroupID>(groupID & 0xFF);
	switch (idMinor)
	{
	case kAnimGroup_AttackLeft: break;
	case kAnimGroup_AttackRight: break;
	case kAnimGroup_Attack3: break;
	case kAnimGroup_Attack4: break;
	case kAnimGroup_Attack5: break;
	case kAnimGroup_Attack6: break;
	case kAnimGroup_Attack7: break;
	case kAnimGroup_Attack8: break;
	case kAnimGroup_AttackLoop: break;
	case kAnimGroup_AttackSpin: break;
	case kAnimGroup_AttackSpin2: break;
	case kAnimGroup_AttackPower: break;
	case kAnimGroup_AttackForwardPower: break;
	case kAnimGroup_AttackBackPower: break;
	case kAnimGroup_AttackLeftPower: break;
	case kAnimGroup_AttackRightPower: break;
	case kAnimGroup_AttackCustom1Power: break;
	case kAnimGroup_AttackCustom2Power: break;
	case kAnimGroup_AttackCustom3Power: break;
	case kAnimGroup_AttackCustom4Power: break;
	case kAnimGroup_AttackCustom5Power: break;
	case kAnimGroup_PlaceMine: break;
	case kAnimGroup_PlaceMine2: break;
	case kAnimGroup_AttackThrow: break;
	case kAnimGroup_AttackThrow2: break;
	case kAnimGroup_AttackThrow3: break;
	case kAnimGroup_AttackThrow4: break;
	case kAnimGroup_AttackThrow5: break;
	case kAnimGroup_Attack9: break;
	case kAnimGroup_AttackThrow6: break;
	case kAnimGroup_AttackThrow7: break;
	case kAnimGroup_AttackThrow8: break;

	default: return false;
	}
	return true;
}

AnimGroupInfo* GetGroupInfo(AnimGroupID groupId)
{
	return &g_animGroupInfos[groupId];
}

eAnimSequence GetSequenceType(AnimGroupID groupId)
{
	return static_cast<eAnimSequence>(GetGroupInfo(groupId)->sequenceType);
}

eAnimSequence GetSequenceType(UInt8 groupId)
{
	return GetSequenceType(static_cast<AnimGroupID>(groupId));
}

AnimGroupInfo* TESAnimGroup::GetGroupInfo() const
{
	return ::GetGroupInfo(GetBaseGroupID());
}

AnimGroupID TESAnimGroup::GetBaseGroupID() const
{
	return static_cast<AnimGroupID>(groupID);
}

enum
{
	kAddr_AddExtraData = 0x40FF60,
	kAddr_RemoveExtraType = 0x410140,
	kAddr_LoadModel = 0x447080,
	kAddr_ApplyAmmoEffects = 0x59A030,
	kAddr_MoveToMarker = 0x5CCB20,
	kAddr_ApplyPerkModifiers = 0x5E58F0,
	kAddr_ReturnThis = 0x6815C0,
	kAddr_PurgeTerminalModel = 0x7FFE00,
	kAddr_EquipItem = 0x88C650,
	kAddr_UnequipItem = 0x88C790,
	kAddr_ReturnTrue = 0x8D0360,
	kAddr_TileGetFloat = 0xA011B0,
	kAddr_TileSetFloat = 0xA012D0,
	kAddr_TileSetString = 0xA01350,
	kAddr_InitFontInfo = 0xA12020,
};

#define CALL_EAX(addr) __asm mov eax, addr __asm call eax
#define JMP_EAX(addr)  __asm mov eax, addr __asm jmp eax
#define JMP_EDX(addr)  __asm mov edx, addr __asm jmp edx

__declspec(naked) NiAVObject* __fastcall NiNode::GetBlockByName(const char *nameStr)	//	str of NiString
{
	__asm
	{
		movzx	eax, word ptr [ecx+0xA6]
		test	eax, eax
		jz		done
		push	esi
		push	edi
		mov		esi, [ecx+0xA0]
		mov		edi, eax
		ALIGN 16
	iterHead:
		dec		edi
		js		iterEnd
		mov		eax, [esi]
		add		esi, 4
		test	eax, eax
		jz		iterHead
		cmp		[eax+8], edx
		jz		found
		mov		ecx, [eax]
		cmp		dword ptr [ecx+0xC], kAddr_ReturnThis
		jnz		iterHead
		mov		ecx, eax
		call	NiNode::GetBlockByName
		test	eax, eax
		jz		iterHead
	found:
		pop		edi
		pop		esi
		retn
		ALIGN 16
	iterEnd:
		xor		eax, eax
		pop		edi
		pop		esi
	done:
		retn
	}
}

__declspec(naked) NiAVObject* __fastcall NiNode::GetBlock(const char *blockName)
{
	__asm
	{
		cmp		[edx], 0
		jz		retnNULL
		push	ecx
		push	edx
		CALL_EAX(0xA5B690)
		pop		ecx
		pop		ecx
		test	eax, eax
		jz		done
		lock dec dword ptr [eax-8]
		jz		retnNULL
		cmp		[ecx+8], eax
		jz		found
		mov		edx, eax
		call	NiNode::GetBlockByName
		retn
	found:
		mov		eax, ecx
		retn
	retnNULL:
		xor		eax, eax
	done:
		retn
	}
}

__declspec(naked) NiNode* __fastcall NiNode::GetNode(const char *nodeName)
{
	__asm
	{
		call	NiNode::GetBlock
		test	eax, eax
		jz		done
		xor		edx, edx
		mov		ecx, [eax]
		cmp		dword ptr [ecx+0xC], kAddr_ReturnThis
		cmovnz	eax, edx
	done:
		retn
	}
}

__declspec(naked) NiAVObject* __fastcall GetNifBlock(TESObjectREFR *thisObj, UInt32 pcNode, const char *blockName)
{
	__asm
	{
		test	dl, dl
		jz		notPlayer
		cmp		dword ptr [ecx+0xC], 0x14
		jnz		notPlayer
		test	dl, 1
		jz		get1stP
		mov		eax, [ecx+0x64]
		test	eax, eax
		jz		done
		mov		eax, [eax+0x14]
		jmp		gotRoot
	get1stP:
		mov		eax, [ecx+0x694]
		jmp		gotRoot
	notPlayer:
		mov		eax, [ecx]
		call	dword ptr [eax+0x1D0]
	gotRoot:
		test	eax, eax
		jz		done
		mov		edx, [esp+4]
		cmp		[edx], 0
		jz		done
		mov		ecx, eax
		call	NiNode::GetBlock
	done:
		retn	4
	}
}

float TESObjectWEAP::GetModBonuses(UInt32 effectID)
{
	float result = 0;
	for (UInt32 idx = 0; idx < 3; idx++)
		if (effectMods[idx] == effectID)
			result += value1Mod[idx];
	return result;
}

bool Actor::IsAnimActionReload() const
{
	const auto currentAnimAction = static_cast<AnimAction>(baseProcess->GetCurrentAnimAction());
	const static std::unordered_set s_reloads = { kAnimAction_Reload, kAnimAction_ReloadLoop, kAnimAction_ReloadLoopStart, kAnimAction_ReloadLoopEnd };
	return s_reloads.contains(currentAnimAction);
}

__declspec(naked) TESForm* __stdcall LookupFormByRefID(UInt32 refID)
{
	__asm
	{
		mov		ecx, ds:[0x11C54C0]
		mov		eax, [esp+4]
		xor		edx, edx
		div		dword ptr [ecx+4]
		mov		eax, [ecx+8]
		mov		eax, [eax+edx*4]
		test	eax, eax
		jz		done
		mov		edx, [esp+4]
		ALIGN 16
	iterHead:
		cmp		[eax+4], edx
		jz		found
		mov		eax, [eax]
		test	eax, eax
		jnz		iterHead
		retn	4
	found:
		mov		eax, [eax+8]
	done:
		retn	4
	}
}

Script* Script::CompileFromText(const std::string& scriptSource, const std::string& name)
{
	ScriptBuffer buffer;
	DataHandler::Get()->SetAssignFormIDs(false);
	auto condition = MakeUnique<Script, 0x5AA0F0, 0x5AA1A0>();
	DataHandler::Get()->SetAssignFormIDs(true);
	buffer.scriptName.Set(("kNVSEScript_" + name).c_str());
	buffer.scriptText = scriptSource.data();
	buffer.partialScript = true;
	buffer.currentScript = condition.get();
	const auto* ctx = ConsoleManager::GetSingleton()->scriptContext;
	const auto result = ThisStdCall<bool>(0x5AEB90, ctx, condition.get(), &buffer);
	condition->text = nullptr;
	buffer.scriptText = nullptr;
	if (!result)
	{
		ERROR_LOG("Failed to compile script");
		return nullptr;
	}
	return condition.release();
}

bool TESForm::IsTypeActor()
{
	return IS_ID(this, Character) || IS_ID(this, Creature);
}

void FormatScriptText(std::string& str)
{
	UInt32 pos = 0;

	while (((pos = str.find('%', pos)) != -1) && pos < str.length() - 1)
	{
		char toInsert = 0;
		switch (str[pos + 1])
		{
		case '%':
			pos += 2;
			continue;
		case 'r':
		case 'R':
			toInsert = '\n';
			break;
		case 'q':
		case 'Q':
			toInsert = '"';
			break;
		default:
			pos += 1;
			continue;
		}

		str.insert(pos, 1, toInsert); // insert char at current pos
		str.erase(pos + 1, 2);		  // erase format specifier
		pos += 1;
	}
}

UInt32* g_weaponTypeToAnim = reinterpret_cast<UInt32*>(0x118A838);

UInt16 GetActorRealAnimGroup(Actor* actor, UInt8 groupID)
{
	UInt8 animHandType = 0;
	if (auto* form = actor->GetWeaponForm(); form && actor->baseProcess->isWeaponOut)
		animHandType = g_weaponTypeToAnim[form->eWeaponType];
	else if (actor->baseProcess->isWeaponOut)
		animHandType = 1; // arms
	auto moveFlags = actor->actorMover->GetMovementFlags();
	UInt8 moveType = 0;
	if ((moveFlags & 0x800) != 0)
		moveType = kAnimMoveType_Swimming;
	else if ((moveFlags & 0x2000) != 0)
		moveType = kAnimMoveType_Flying;
	else if ((moveFlags & 0x400) != 0)
		moveType = kAnimMoveType_Sneaking;
	const auto isPowerArmor = ThisStdCall<bool>(0x8BA3E0, actor) || ThisStdCall<bool>(0x8BA410, actor);
	return (moveType << 12) + (isPowerArmor << 15) + (animHandType << 8) + groupID;
}

const char* MoveFlagToString(UInt32 flag)
{
	auto flag8 = static_cast<MovementFlags>(flag & 0xFF);
	switch(flag8)
	{
	case kMoveFlag_Forward: return "Forward";
	case kMoveFlag_Backward: return "Backward";
	case kMoveFlag_Left: return "Left";
	case kMoveFlag_Right: return "Right";
	case kMoveFlag_TurnLeft: return "TurnLeft";
	case kMoveFlag_TurnRight: return "TurnRight";
	case kMoveFlag_NonController: return "NonController";
	case kMoveFlag_Walking: return "Walking";
	case kMoveFlag_Running: return "Running";
	case kMoveFlag_Sneaking: return "Sneaking";
	case kMoveFlag_Swimming: return "Swimming";
	case kMoveFlag_Jump: return "Jump";
	case kMoveFlag_Flying: return "Flying";
	case kMoveFlag_Fall: return "Fall";
	case kMoveFlag_Slide: return "Slide";
	}
	return "";
}

bool BaseProcess::WeaponInfo::HasWeaponMod(WeaponModFlags mod) const
{
	if (auto* xData = GetExtraData(); xData)
	{
		const auto* modFlags = static_cast<ExtraWeaponModFlags*>(xData->GetByType(kExtraData_WeaponModFlags));
		if (modFlags && modFlags->flags)
			return (modFlags->flags & mod) != 0;
	}
	return false;
}


bool Actor::HasWeaponWithMod(WeaponModFlags mod) const
{
	if (auto* weaponInfo = this->baseProcess->GetWeaponInfo(); weaponInfo && weaponInfo->weapon)
		return weaponInfo->HasWeaponMod(mod);
	return false;
}

AnimHandTypes AnimGroup::GetHandType(UInt16 groupId)
{
	return static_cast<AnimHandTypes>((groupId & 0xF00) >> 8);
}

UInt16 TESAnimGroup::GetMoveType() const
{
	return AnimGroup::GetMoveType(groupID);
}

// based on 0x495740
bool AnimGroup::FallbacksTo(AnimData* animData, FullAnimGroupID sourceGroupId, FullAnimGroupID destGroupId)
{
	if (sourceGroupId == destGroupId)
		return true;

	const AnimGroupID sourceBaseGroupId = GetBaseGroupID(sourceGroupId);

	// Check for special upper bit flags
	if ((sourceGroupId & 0x8000) != 0)
	{
		if (FallbacksTo(animData, sourceGroupId & ~0x8000, destGroupId))
			return true;
		if (IsAttackIS(sourceBaseGroupId) && destGroupId == sourceGroupId - 3)
			return true;
	}
	const auto sequenceType = GetSequenceType(sourceBaseGroupId);
	const bool isWeaponSequence = sequenceType >= kSequence_Weapon && sequenceType <= kSequence_WeaponDown;

	// Check for player-specific weapon animations
	if (animData == g_thePlayer->firstPersonAnimData &&
		(sourceGroupId & 0x7000) != 0 &&
		(isWeaponSequence && FallbacksTo(animData, sourceGroupId & ~0xF000, destGroupId)))
		return true;

	// Check for attack group fallback
	if (IsAttackIS(sourceBaseGroupId))
		return FallbacksTo(animData, sourceGroupId - 3, destGroupId);

	// Check sequence type
	if (isWeaponSequence)
		return false;

	constexpr auto mask1HM = kAnimHandType_1HM << 8;
	constexpr auto mask1HP = kAnimHandType_1HP << 8;

	// Check hand type fallbacks
	if ((sourceGroupId & 0xF00) != 0)
	{
		const AnimHandTypes handType = GetHandType(sourceGroupId);
		const UInt16 fallbackGroupId = sourceGroupId & 0xF0FF;

		if (handType == kAnimHandType_2HM)
		{
			if (destGroupId == (fallbackGroupId | mask1HM))
				return true;
		}
		else if (handType != kAnimHandType_1HP && handType != kAnimHandType_1HM)
		{
			if (destGroupId == (fallbackGroupId | mask1HP))
				return true;
		}

		if (destGroupId == fallbackGroupId)
			return true;
	}

	// Check movement group fallbacks
	UInt16 movementGroupId = 0xFFFF;
	const UInt16 masked = sourceGroupId & ~0x80FF;
	switch (sourceBaseGroupId)
	{
	case kAnimGroup_FastForward:
		movementGroupId = masked | kAnimGroup_Forward;
		break;
	case kAnimGroup_FastBackward:
		movementGroupId = masked | kAnimGroup_Backward;
		break;
	case kAnimGroup_FastLeft:
		movementGroupId = masked | kAnimGroup_Left;
		break;
	case kAnimGroup_FastRight:
		movementGroupId = masked | kAnimGroup_Right;
		break;
	default:
		break;
	}

	if (movementGroupId != 0xFFFF &&
		FallbacksTo(animData, movementGroupId, destGroupId))
	{
		return true;
	}

	// Check upper bits fallback
	if ((sourceGroupId & 0x7000) != 0 &&
		FallbacksTo(animData, sourceGroupId & 0xFFF, destGroupId))
	{
		return true;
	}

	// Final fallback check
	return destGroupId == (sourceGroupId & 0x7F00);
}
