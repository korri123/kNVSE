#include "game_types.h"

#include "commands_animation.h"
#include "GameOSDepend.h"
#include "GameProcess.h"
#include "NiObjects.h"
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
	animGroupId110 = static_cast<UInt32>(GetAnimData()->GetNextAttackGroupID());
	this->baseProcess->SetQueuedIdleFlag(kIdleFlag_FireWeapon);
	GameFuncs::HandleQueuedAnimFlags(this); //Actor::HandleQueuedIdleFlags
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

BSAnimGroupSequence* BSAnimGroupSequence::Get3rdPersonCounterpart() const
{
	auto* animData = g_thePlayer->baseProcess->GetAnimData();
	if (auto* base = animData->mapAnimSequenceBase->Lookup(this->animGroup->groupID))
		return base->GetSequenceByIndex(0);
	return nullptr;
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

TESAnimGroup::AnimGroupInfo* GetGroupInfo(UInt8 groupId)
{
	return &g_animGroupInfos[groupId];
}

TESAnimGroup::AnimGroupInfo* TESAnimGroup::GetGroupInfo() const
{
	return ::GetGroupInfo(groupID);
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
