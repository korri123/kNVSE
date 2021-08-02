#pragma once

#include <span>

#include "CommandTable.h"
#include "GameForms.h"
#include "GameObjects.h"
#include "GameProcess.h"
#include "game_types.h"
#include "ParamInfos.h"
#include "utility.h"

extern std::span<TESAnimGroup::AnimGroupInfo> g_animGroupInfos;

enum QueuedIdleFlags
{
  kIdleFlag_FireWeapon = 0x1,
  kIdleFlag_Reload = 0x2,
  kIdleFlag_CrippledLimb = 0x10,
  kIdleFlag_Death = 0x20,
  kIdleFlag_ForcedIdle = 0x80,
  kIdleFlag_HandGrip = 0x100,
  kIdleFlag_Activate = 0x400,
  kIdleFlag_StandingLayingDownChange = 0x800,
  kIdleFlag_EquipOrUnequip = 0x4000,
  kIdleFlag_AimWeapon = 0x10000,
  kIdleFlag_AttackEjectEaseInFollowThrough = 0x20000,
  kIdleFlag_SomethingAnimatingReloadLoop = 0x40000,
};

struct BurstFireData
{
	AnimData* animData;
	BSAnimGroupSequence* anim;
	std::size_t index;
	std::vector<NiTextKey*> hitKeys;
	float timePassed;
	bool shouldEject = false;
};

struct CallScriptKeyData
{
	std::vector<NiTextKey*> hitKeys;
	float timePassed = 0;
};

enum class POVSwitchState
{
	NotSet, POV3rd, POV1st
};
struct AnimTime
{
	float time = 0;
	bool finishedEndKey = false;
	bool respectEndKey = false;
	bool callScript = false;
	std::vector<std::pair<Script*, float>> scripts;
	AnimData* animData = nullptr;
	UInt32 scriptStage = 0;
	BSAnimGroupSequence* anim3rdCounterpart = nullptr;
	POVSwitchState povState = POVSwitchState::NotSet;
	UInt32 numThirdPersonKeys = 0;
	float *thirdPersonKeys = nullptr;
};

extern std::map<BSAnimGroupSequence*, AnimTime> g_timeTrackedAnims;

enum class AnimKeySetting
{
	NotChecked, NotSet, Set 
};

struct AnimPath
{
	std::string path;
	AnimKeySetting hasBurstFire{};
	AnimKeySetting hasRespectEndKey{};
	AnimKeySetting hasInterruptLoop{};
	AnimKeySetting hasNoBlend{};
	AnimKeySetting hasCallScript{};
	bool partialReload = false;

};

struct SavedAnims
{
	int order = -1;
	std::vector<AnimPath> anims;
	Script* conditionScript = nullptr;
};

enum class AnimCustom
{
	None, Male, Female, Mod1, Mod2, Mod3, Hurt, Human, Max
};

enum AnimKeyTypes
{
	kAnimKeyType_ClampSequence = 0x0,
	kAnimKeyType_LoopingSequenceOrAim = 0x1,
	kAnimKeyType_SpecialIdle = 0x2,
	kAnimKeyType_Equip = 0x3,
	kAnimKeyType_Unequip = 0x4,
	kAnimKeyType_Attack = 0x5,
	kAnimKeyType_PowerAttackOrPipboy = 0x6,
	kAnimKeyType_AttackThrow = 0x7,
	kAnimKeyType_PlaceMine = 0x8,
	kAnimKeyType_SpinAttack = 0x9,
	kAnimKeyType_LoopingReload = 0xA,
	kAnimKeyType_MAX = 0xB,
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
	std::vector<SavedAnims> humanAnims;

	std::vector<SavedAnims>& GetCustom(const AnimCustom custom)
	{
		switch (custom) { case AnimCustom::None: return anims;
		case AnimCustom::Male: return maleAnims;
		case AnimCustom::Female: return femaleAnims;
		case AnimCustom::Mod1: return mod1Anims;
		case AnimCustom::Mod2: return mod2Anims;
		case AnimCustom::Mod3: return mod3Anims;
		case AnimCustom::Hurt: return hurtAnims;
		case AnimCustom::Human: return humanAnims;
		}
		return anims;
	}

};

struct BurstState
{
	int index = 0;
	BurstState() = default;
};

struct JSONAnimContext
{
	Script* script;

	void Reset()
	{
		memset(this, 0, sizeof JSONAnimContext);
	}

	JSONAnimContext() { Reset(); }
};

extern JSONAnimContext g_jsonContext;

extern std::unordered_map<BSAnimGroupSequence*, BurstState> burstFireAnims;

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

enum AnimAction
{
	kAnimAction_None = 0xFFFFFFFF,
	kAnimAction_Equip_Weapon = 0x0,
	kAnimAction_Unequip_Weapon = 0x1,
	kAnimAction_Attack = 0x2,
	kAnimAction_Attack_Eject = 0x3,
	kAnimAction_Attack_Follow_Through = 0x4,
	kAnimAction_Attack_Throw = 0x5,
	kAnimAction_Attack_Throw_Attach = 0x6,
	kAnimAction_Block = 0x7,
	kAnimAction_Recoil = 0x8,
	kAnimAction_Reload = 0x9,
	kAnimAction_Stagger = 0xA,
	kAnimAction_Dodge = 0xB,
	kAnimAction_Wait_For_Lower_Body_Anim = 0xC,
	kAnimAction_Wait_For_Special_Idle = 0xD,
	kAnimAction_Force_Script_Anim = 0xE,
	kAnimAction_ReloadLoopStart = 0xF,
	kAnimAction_ReloadLoopEnd = 0x10,
	kAnimAction_ReloadLoop = 0x11,
};

#define THISCALL(address, returnType, ...) reinterpret_cast<returnType(__thiscall*)(__VA_ARGS__)>(address)

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
	inline auto* NiTPointerMap_Init = reinterpret_cast<GameAnimMap * (__thiscall*)(GameAnimMap*, int numBuckets)>(0x49C050);

	// Multiple "Hit" per anim
	inline auto* AnimData_GetSequenceOffsetPlusTimePassed = reinterpret_cast<float (__thiscall*)(AnimData*, BSAnimGroupSequence*)>(0x493800);
	inline auto* TESAnimGroup_GetTimeForAction = reinterpret_cast<double (__thiscall*)(TESAnimGroup*, UInt32)>(0x5F3780);
	inline auto* Actor_SetAnimActionAndSequence = reinterpret_cast<void (__thiscall*)(Actor*, Decoding::AnimAction, BSAnimGroupSequence*)>(0x8A73E0);
	inline auto* AnimData_GetAnimSequenceElement = reinterpret_cast<BSAnimGroupSequence* (__thiscall*)(AnimData*, eAnimSequence a2)>(0x491040);
	
	inline auto GetControlState = _VL((Decoding::ControlCode code, Decoding::IsDXKeyState state), ThisStdCall<int>(0xA24660, *g_inputGlobals, code, state));
	inline auto SetControlHeld = _VL((int key), ThisStdCall<int>(0xA24280, *g_inputGlobals, key));

	inline auto IsDoingAttackAnimation = THISCALL(0x894900, bool, Actor* actor);
	inline auto HandleQueuedAnimFlags = THISCALL(0x8BA600, void, Actor* actor);

	inline auto ActivateSequence = THISCALL(0xA2E280, bool, NiControllerManager* manager, NiControllerSequence* source, NiControllerSequence* destination, float blend, int priority, bool startOver, float morphWeight, NiControllerSequence* pkTimeSyncSeq);
	inline auto MorphSequence = THISCALL(0xA351D0, bool, NiControllerSequence *source, NiControllerSequence *pkDestSequence, float fDuration, int iPriority, float fSourceWeight, float fDestWeight);
}

enum SequenceState1
{
	kSeqState_Start = 0x0,
	kSeqState_HitOrDetach = 0x1,
	kSeqState_EjectOrUnequipEnd = 0x2,
	kSeqState_Unk3 = 0x3,
	kSeqState_End = 0x4,
};

enum PlayerAnimDataType
{
  kPlayerAnimData_3rd = 0x0,
  kPlayerAnimData_1st = 0x1,
};


BSAnimGroupSequence* GetActorAnimation(UInt32 animGroupId, bool firstPerson, AnimData* animData, const char* prevPath);

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
	{ "play immediately", kParamType_Integer, 1 },
	{"condition", kParamType_AnyForm, 1},
};

static ParamInfo kParams_PlayAnimationPath[] =
{
	{"path", kParamType_String, 0},
	{"anim type", kParamType_Integer, 0},
	{"first person", kParamType_Integer, 0}
};

static ParamInfo kParams_SetAnimationPathCondition[] =
{
	{"path", kParamType_String, 0},
	{"condition", kParamType_AnyForm, 0},
};

DEFINE_COMMAND_PLUGIN(ForcePlayIdle, "", true, 2, kParams_OneForm_OneOptionalInt)
DEFINE_COMMAND_PLUGIN(SetWeaponAnimationPath, "", false, sizeof kParams_SetWeaponAnimationPath / sizeof(ParamInfo), kParams_SetWeaponAnimationPath)
DEFINE_COMMAND_PLUGIN(SetActorAnimationPath, "", false, sizeof kParams_SetActorAnimationPath / sizeof(ParamInfo), kParams_SetActorAnimationPath)
DEFINE_COMMAND_PLUGIN(PlayAnimationPath, "", true, sizeof kParams_PlayAnimationPath / sizeof(ParamInfo), kParams_PlayAnimationPath)
DEFINE_COMMAND_PLUGIN(kNVSEReset, "", false, 0, nullptr)
DEFINE_COMMAND_PLUGIN(ForceStopIdle, "", true, 1, kParams_OneOptionalInt)


void OverrideActorAnimation(const Actor* actor, const std::string& path, bool firstPerson, bool enable, bool append, int* outGroupId = nullptr, Script* conditionScript = nullptr);
void OverrideWeaponAnimation(const TESObjectWEAP* weapon, const std::string& path, bool firstPerson, bool enable, bool append);
void OverrideModIndexAnimation(UInt8 modIdx, const std::string& path, bool firstPerson, bool enable, bool append);
void OverrideRaceAnimation(const TESRace* race, const std::string& path, bool firstPerson, bool enable, bool append);
void OverrideFormAnimation(const TESForm* form, const std::string& path, bool firstPerson, bool enable, bool append);

float GetTimePassed(AnimData* animData, UInt8 animGroupID);

bool IsCustomAnim(BSAnimGroupSequence* sequence);

BSAnimGroupSequence* GetGameAnimation(AnimData* animData, UInt16 groupID);
float GetAnimMult(AnimData* animData, UInt8 animGroupID);
bool IsAnimGroupReload(UInt8 animGroupId);