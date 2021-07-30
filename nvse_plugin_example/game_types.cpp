#include "game_types.h"

#include "commands_animation.h"
#include "GameOSDepend.h"
#include "GameProcess.h"

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
	ThisStdCall(0x8BA600, this); //Actor::HandleQueuedIdleFlags
}

TESObjectWEAP* Actor::GetWeaponForm() const
{
	auto* weaponInfo = this->baseProcess->GetWeaponInfo();
	if (!weaponInfo)
		return nullptr;
	return weaponInfo->weapon;
}

OSInputGlobals** g_inputGlobals = reinterpret_cast<OSInputGlobals**>(0x11F35CC);


UInt32 GetControl(UInt32 whichControl, Decoding::ControlType type)
{
	auto* globs = *g_inputGlobals;

	if (whichControl >= globs->kMaxControlBinds)
		return 0xFF;

	UInt32	result;

	switch (type)
	{
	case Decoding::kControlType_Keyboard:
		result = globs->keyBinds[whichControl];
		break;
	case Decoding::kControlType_Mouse:
		result = globs->mouseBinds[whichControl];
		if (result != 0xFF) result += 0x100;
		break;
	case Decoding::kControlType_Joystick:
		result = globs->joystickBinds[whichControl];
		break;
	default:
		result = 0xFF;
		break;
	}

	return result;
}
