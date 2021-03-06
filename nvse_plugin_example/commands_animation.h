#pragma once

#include "CommandTable.h"
#include "GameForms.h"
#include "GameObjects.h"
#include "GameProcess.h"
#include "ParamInfos.h"

struct SavedAnims
{
	int order = -1;
	std::vector<std::string> anims;
};

enum class AnimCustom
{
	None, Male, Female, Mod1, Mod2, Mod3, Hurt
};

using GameAnimMap = NiTPointerMap<AnimSequenceBase>;

struct AnimStacks
{
	std::vector<SavedAnims> anims;
	std::vector<SavedAnims> maleAnims;
	std::vector<SavedAnims> femaleAnims;
	std::vector<SavedAnims> mod1Anims;
	std::vector<SavedAnims> mod2Anims;
	std::vector<SavedAnims> mod3Anims;

	std::vector<SavedAnims> hurtAnims;

	std::vector<SavedAnims>& GetCustom(const AnimCustom custom)
	{
		switch (custom) { case AnimCustom::None: return anims;
		case AnimCustom::Male: return maleAnims;
		case AnimCustom::Female: return femaleAnims;
		case AnimCustom::Mod1: return mod1Anims;
		case AnimCustom::Mod2: return mod2Anims;
		case AnimCustom::Mod3: return mod3Anims;
		case AnimCustom::Hurt: return hurtAnims;
		default: ;
		}
		return anims;
	}

};

enum AnimHandTypes
{
	kAnim_H2H = 0,
	kAnim_1HM,
	kAnim_2HM,
	kAnim_1HP,
	kAnim_2HR,
	kAnim_2HA,
	kAnim_2HH,
	kAnim_2HL,
	kAnim_1GT,
	kAnim_1MD,
	kAnim_1LM,
	kAnim_Max,
};

enum eAnimSequence
{
	kSequence_None = -0x1,
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
	inline auto* PlayAnimGroup = reinterpret_cast<BSAnimGroupSequence * (__thiscall*)(AnimData*, int, int, int, int)>(0x494740);
	inline auto* NiTPointerMap_Lookup = reinterpret_cast<bool (__thiscall*)(void*, int, AnimSequenceBase**)>(0x49C390);
	inline auto* NiTPointerMap_RemoveKey = reinterpret_cast<bool(__thiscall*)(void*, UInt16)>(0x49C250);
	inline auto* NiTPointerMap_Init = reinterpret_cast<GameAnimMap *(__thiscall*)(GameAnimMap*, int numBuckets)>(0x49C050);
}

BSAnimGroupSequence* GetWeaponAnimation(TESObjectWEAP* weapon, UInt32 animGroupId, bool firstPerson, AnimData* animData);
BSAnimGroupSequence* GetActorAnimation(Actor* actor, UInt32 animGroupId, bool firstPerson, AnimData* animData, const char* prevPath);

static ParamInfo kParams_SetWeaponAnimationPath[] =
{
	{	"weapon",	kParamType_AnyForm,	0	}, // weapon
	{	"first person",	kParamType_Integer,	0	}, // firstPerson
	{	"enable",	kParamType_Integer,	0	}, // enable or disable
	{	"animation path",	kParamType_String,	0	},  // path
};

static ParamInfo kParams_SetActorAnimationPath[] =
{
	{	"first person",	kParamType_Integer,	0	}, // firstPerson
	{	"enable",	kParamType_Integer,	0	}, // enable or disable
	{	"animation path",	kParamType_String,	0	},  // path
	{ "play immediately", kParamType_Integer, 1 }
};

static ParamInfo kParams_PlayAnimationPath[] =
{
	{"path", kParamType_String, 0},
	{"anim type", kParamType_Integer, 0},
	{"first person", kParamType_Integer, 0}
};

DEFINE_COMMAND_PLUGIN(ForcePlayIdle, "", true, 1, kParams_OneForm)
DEFINE_COMMAND_PLUGIN(SetWeaponAnimationPath, "", false, sizeof kParams_SetWeaponAnimationPath / sizeof(ParamInfo), kParams_SetWeaponAnimationPath)
DEFINE_COMMAND_PLUGIN(SetActorAnimationPath, "", true, sizeof kParams_SetActorAnimationPath / sizeof(ParamInfo), kParams_SetActorAnimationPath)
DEFINE_COMMAND_PLUGIN(PlayAnimationPath, "", true, sizeof kParams_PlayAnimationPath / sizeof(ParamInfo), kParams_PlayAnimationPath)

void OverrideActorAnimation(const Actor* actor, const std::string& path, bool firstPerson, bool enable, bool append, int* outGroupId = nullptr);
void OverrideWeaponAnimation(const TESObjectWEAP* weapon, const std::string& path, bool firstPerson, bool enable, bool append);
void OverrideModIndexAnimation(UInt8 modIdx, const std::string& path, bool firstPerson, bool enable, bool append);
void OverrideRaceAnimation(const TESRace* race, const std::string& path, bool firstPerson, bool enable, bool append);

/*
   ~AnimStacks()
	{
		if (map)
		{
			map->~NiTPointerMap<AnimSequenceBase>();
			FormHeap_Free(map);
		}
	}
 */