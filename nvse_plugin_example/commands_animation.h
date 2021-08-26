#pragma once

#include <chrono>
#include <chrono>
#include <optional>
#include <span>
#include <unordered_set>

#include "CommandTable.h"
#include "GameForms.h"
#include "GameObjects.h"
#include "GameProcess.h"
#include "game_types.h"
#include "ParamInfos.h"
#include "utility.h"

struct SavedAnims;
extern std::span<TESAnimGroup::AnimGroupInfo> g_animGroupInfos;
using FormID = UInt32;

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
	bool firstPerson = false;
	BSAnimGroupSequence* anim;
	std::size_t index;
	std::vector<NiTextKey*> hitKeys;
	float timePassed;
	bool shouldEject = false;
	float lastNiTime = -FLT_MAX;
	UInt32 actorId = 0;
	std::vector<NiTextKey*> ejectKeys;
	std::size_t ejectIdx = 0;
	bool reloading = false;
};

extern std::list<BurstFireData> g_burstFireQueue;

struct CallScriptKeyData
{
	std::vector<NiTextKey*> hitKeys;
	float timePassed = 0;
};

enum class POVSwitchState
{
	NotSet, POV3rd, POV1st
};

template <typename T>
class TimedExecution
{
public:

	class Context
	{
	public:
		TimedExecution* execution;
		size_t index = 0;

		explicit Context(TimedExecution<T>* execution)
			: execution(execution)
		{
		}

		bool Exists() { return execution != nullptr; }

		Context() : execution(nullptr) {}

		template <typename F>
		void Update(float time, AnimData* animData, F&& f)
		{
			if (index >= execution->items.size())
				return;
			auto& [item, nextTime] = execution->items.at(index);
			if (time >= nextTime)
			{
				if (!IsPlayersOtherAnimData(animData))
					f(item);
				++index;
			}
		}
	};

	std::vector<std::pair<T, float>> items;
	bool init = false;

	template <typename F2>
	TimedExecution(const std::span<NiTextKey>& keys, F2&& f2)
	{
		for (auto& key : keys)
		{
			T t;
			if (f2(key.m_kText.CStr(), t))
				items.emplace_back(t, key.m_fTime);
		}
		init = true;
	}

	Context CreateContext()
	{
		return Context(this);
	}
};

struct AnimTime
{
	UInt32 actorId = 0;
	float lastNiTime = -FLT_MAX;
	bool finishedEndKey = false;
	bool respectEndKey = false;
	bool firstPerson = false;
	BSAnimGroupSequence* anim3rdCounterpart = nullptr;
	POVSwitchState povState = POVSwitchState::NotSet;
	TESObjectWEAP* actorWeapon = nullptr;
	TimedExecution<Script*>::Context scriptLines;
	TimedExecution<Script*>::Context scriptCalls;
	TimedExecution<Sound>::Context soundPaths;

	AnimData* GetAnimData(Actor* actor) const
	{
		if (firstPerson)
			return g_thePlayer->firstPersonAnimData;
		return actor->baseProcess->GetAnimData();
	}
};

struct SavedAnimsTime
{
	float time = 0;
	UInt16 groupId = 0;
	BSAnimGroupSequence* anim = nullptr;
	UInt32 actorId = 0;
	float lastNiTime = -FLT_MAX;
	bool firstPerson = false;
};

extern std::map<BSAnimGroupSequence*, AnimTime> g_timeTrackedAnims;
extern std::map<SavedAnims*, SavedAnimsTime> g_timeTrackedGroups;

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
	AnimKeySetting hasSoundPath{};
	AnimKeySetting hasBlendToReloadLoop{};
	AnimKeySetting hasScriptLine{};
	bool partialReload = false;
	std::unique_ptr<TimedExecution<Script*>> scriptLineKeys = nullptr;
	std::unique_ptr<TimedExecution<Script*>> scriptCallKeys = nullptr;
	std::unique_ptr<TimedExecution<Sound>> soundPaths = nullptr;

};

struct SavedAnims
{
	bool hasOrder = false;
	std::vector<AnimPath> anims;
	Script* conditionScript = nullptr;
	bool pollCondition = false;
	std::unique_ptr<AnimPath> emptyMagAnim = nullptr;
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
	bool pollCondition;

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

enum AnimAction : SInt16
{
	kAnimAction_None = -1,
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
	
	inline auto GetControlState = _VL((ControlCode code, Decoding::IsDXKeyState state), ThisStdCall<int>(0xA24660, *g_inputGlobals, code, state));
	inline auto SetControlHeld = _VL((int key), ThisStdCall<int>(0xA24280, *g_inputGlobals, key));

	inline auto IsDoingAttackAnimation = THISCALL(0x894900, bool, Actor* actor);
	inline auto HandleQueuedAnimFlags = THISCALL(0x8BA600, void, Actor* actor);

	inline auto ActivateSequence = THISCALL(0xA2E280, bool, NiControllerManager* manager, NiControllerSequence* source, NiControllerSequence* destination, float blend, int priority, bool startOver, float morphWeight, NiControllerSequence* pkTimeSyncSeq);
	inline auto MorphSequence = THISCALL(0xA351D0, bool, NiControllerSequence *source, NiControllerSequence *pkDestSequence, float fDuration, int iPriority, float fSourceWeight, float fDestWeight);
	inline auto BlendFromPose = THISCALL(0xA2F800, bool, NiControllerManager * mgr, NiControllerSequence * pkSequence, float fDestFrame, float fDuration, int iPriority, NiControllerSequence * pkSequenceToSynchronize);
	inline auto DeactivateSequence = THISCALL(0x47B220, int, NiControllerManager * mgr, NiControllerSequence * pkSequence, float fEaseOut);
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

class AnimationResult
{
public:
	SavedAnims* parent;
	SavedAnimsTime* animsTime;
	AnimationResult(SavedAnims* parent, SavedAnimsTime* animsTime)
		: parent(parent), animsTime(animsTime)
	{
	}
};

struct BSAnimationContext
{
	BSAnimGroupSequence* anim;
	AnimSequenceBase* base;

	BSAnimationContext(BSAnimGroupSequence* anim, AnimSequenceBase* base): anim(anim), base(base)
	{
	}
};

std::optional<BSAnimationContext> LoadCustomAnimation(const std::string& path, AnimData* animData);
BSAnimGroupSequence* LoadAnimationPath(const AnimationResult& result, AnimData* animData, UInt16 groupId);

std::optional<AnimationResult> GetActorAnimation(UInt32 animGroupId, bool firstPerson, AnimData* animData, const char* prevPath);

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
#if _DEBUG

DEFINE_COMMAND_PLUGIN(kNVSETest, "", false, 0, nullptr);

#endif

void OverrideModIndexAnimation(UInt8 modIdx, const std::string& path, bool firstPerson, bool enable, std::unordered_set<UInt16>& groupIdFillSet);
void OverrideFormAnimation(const TESForm* form, const std::string& path, bool firstPerson, bool enable, std::unordered_set<UInt16>& groupIdFillSet, Script* conditionScript = nullptr, bool pollCondition = false);

void HandleOnActorReload();

float GetTimePassed(AnimData* animData, UInt8 animGroupID);

bool IsCustomAnim(BSAnimGroupSequence* sequence);

BSAnimGroupSequence* GetGameAnimation(AnimData* animData, UInt16 groupID);
bool HandleExtraOperations(AnimData* animData, BSAnimGroupSequence* sequence, AnimPath& ctx);
float GetAnimMult(AnimData* animData, UInt8 animGroupID);
bool IsAnimGroupReload(UInt8 animGroupId);

bool WeaponHasNthMod(Decoding::ContChangesEntry* weaponInfo, TESObjectWEAP* weap, UInt32 mod);


int GetWeaponInfoClipSize(Actor* actor);

enum class ReloadSubscriber
{
	Partial, BurstFire, AimTransition
};

struct ReloadHandler
{
	std::unordered_map<ReloadSubscriber, bool> subscribers;
	UInt32 lastAmmoCount = 0;
	AnimAction lastAction = kAnimAction_None;
};

void SubscribeOnActorReload(Actor* actor, ReloadSubscriber subscriber);

bool DidActorReload(Actor* actor, ReloadSubscriber subscriber);

AnimPath* GetAnimPath(const AnimationResult& animResult, UInt16 groupId, AnimData* animData);