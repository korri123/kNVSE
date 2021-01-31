#pragma once

#include "CommandTable.h"
#include "GameForms.h"
#include "GameObjects.h"
#include "GameProcess.h"
#include "ParamInfos.h"
struct SavedAnim
{
	KFModel* model;
	AnimSequenceSingle* single;
	UInt32 animGroup;

	SavedAnim(KFModel* model, AnimSequenceSingle* single, UInt32 animGroup)
		: model(model),
		single(single),
		animGroup(animGroup)
	{
	}
};

struct Anims
{
	std::vector<SavedAnim> anims;
	bool enabled = true;
};

enum class AnimType
{
	kAnimType_Null,
	kAnimType_AttackFirstPerson,
	kAnimType_AttackFirstPersonIS,
	kAnimType_AttackThirdPerson,
	kAnimType_AttackThirdPersonUp,
	kAnimType_AttackThirdPersonDown,
	kAnimType_AttackThirdPersonIS,
	kAnimType_AttackThirdPersonISUp,
	kAnimType_AttackThirdPersonISDown,
	kAnimType_AimFirstPerson,
	kAnimType_AimFirstPersonIS,
	kAnimType_AimThirdPerson,
	kAnimType_AimThirdPersonUp,
	kAnimType_AimThirdPersonDown,
	kAnimType_AimThirdPersonIS,
	kAnimType_AimThirdPersonISUp,
	kAnimType_AimThirdPersonISDown,
	kAnimType_ReloadFirstPerson,
	kAnimType_ReloadThirdPerson,
	kAnimType_ReloadStartFirstPerson,
	kAnimType_ReloadStartThirdPerson,
	kAnimType_EquipFirstPerson,
	kAnimType_EquipThirdPerson,
	kAnimType_UnequipFirstPerson,
	kAnimType_UnequipThirdPerson,
	kAnimType_JamFirstPerson,
	kAnimType_JamThirdPerson,
	kAnimType_Max,

};

class AnimGroupType
{
public:
	BSAnimGroupSequence* firstPerson = nullptr;
	BSAnimGroupSequence* thirdPerson = nullptr;
	BSAnimGroupSequence* thirdPersonUp = nullptr;
	BSAnimGroupSequence* thirdPersonDown = nullptr;
};

enum eAnimSequence
{
	kSequence_Idle = 0x0,
	kSequence_Movement = 0x1,
	kSequence_LeftArm = 0x2,
	kSequence_LeftHand = 0x3,
	kSequence_Weapon = 0x4,
	kSequence_WeaponUp = 0x5,
	kSequence_WeaponDown = 0x6,
	kSequence_SpecialIdle = 0x7,
	kSequence_Death = 0x14,
};

namespace GameFuncs
{
	inline auto* PlayIdle = reinterpret_cast<void(__thiscall*)(void*, TESIdleForm*, Actor*, int, int)>(0x497F20);
	inline auto* ConstructAnimIdle = reinterpret_cast<void* (__thiscall*)(AnimIdle*, TESIdleForm*, eAnimSequence, int, MobileObject*, bool,
		AnimData*)>(0x4965D0);
	inline auto* PlayAnimation = reinterpret_cast<void(__thiscall*)(AnimData*, UInt32, int flags, int loopRange, eAnimSequence)>(0x494740);
	inline auto* LoadKFModel = reinterpret_cast<KFModel * (__thiscall*)(ModelLoader*, const char*)>(0x4471C0);
	inline auto* BSAnimGroupSequence_Init = reinterpret_cast<void(__thiscall*)(BSAnimGroupSequence*, TESAnimGroup*, BSAnimGroupSequence*)>(0x4EE9F0);
	inline auto* KFModel_Init = reinterpret_cast<void(__thiscall*)(KFModel * alloc, const char* filePath, char* bsStream)>(0x43B640);
	inline auto* GetFilePtr = reinterpret_cast<BSFile * (__cdecl*)(const char* path, int const_0, int const_negative_1, int const_1)>(0xAFDF20); // add Meshes in front!
	inline auto* BSStream_SetFileAndName = reinterpret_cast<bool(__thiscall*)(char* bsStreamBuf, const char* fileName, BSFile*)>(0xC3A8A0);
	inline auto* BSStream_Init = reinterpret_cast<char* (__thiscall*)(char* bsStream)>(0x43CFD0);
	inline auto* GetAnims = reinterpret_cast<tList<char>*(__thiscall*)(TESObjectREFR*, int)>(0x566970);
	inline auto* LoadAnimation = reinterpret_cast<bool(__thiscall*)(AnimData*, KFModel*, bool)>(0x490500);
	inline auto* MorphToSequence = reinterpret_cast<BSAnimGroupSequence * (__thiscall*)(AnimData*, BSAnimGroupSequence*, int, int)>(0x4949A0);
}

BSAnimGroupSequence* GetWeaponAnimation(UInt32 refId, UInt32 animGroupId, bool firstPerson);
BSAnimGroupSequence* GetActorAnimation(UInt32 refId, UInt32 animGroupId, bool firstPerson);
SavedAnim* GetWeaponAnimationSingle(UInt32 refId, UInt32 animGroupId, bool firstPerson);
SavedAnim* GetActorAnimationSingle(UInt32 refId, UInt32 animGroupId, bool firstPerson);

static ParamInfo kParams_SetWeaponAnimationPath[5] =
{
	{	"weapon",	kParamType_AnyForm,	0	}, // weapon
	{	"anim group",	kParamType_Integer,	0	}, // animGroup
	{	"first person",	kParamType_Integer,	0	}, // firstPerson
	{	"enable",	kParamType_Integer,	0	}, // enable or disable
	{	"animation path",	kParamType_String,	1	},  // path
};

static ParamInfo kParams_SetActorAnimationPath[5] =
{
	{	"anim group",	kParamType_Integer,	0	}, // animGroup
	{	"anim group hands",	kParamType_Integer,	0	}, // animGroup hands
	{	"first person",	kParamType_Integer,	0	}, // firstPerson
	{	"enable",	kParamType_Integer,	0	}, // enable or disable
	{	"animation path",	kParamType_String,	1	},  // path
};

DEFINE_COMMAND_PLUGIN(ForcePlayIdle, "", true, 1, kParams_OneForm)
DEFINE_COMMAND_PLUGIN(SetWeaponAnimationPath, "", false, sizeof(kParams_SetWeaponAnimationPath) / sizeof(ParamInfo), kParams_SetWeaponAnimationPath)
DEFINE_COMMAND_PLUGIN(SetActorAnimationPath, "", true, sizeof(kParams_SetActorAnimationPath) / sizeof(ParamInfo), kParams_SetActorAnimationPath)

