#pragma once

#include <chrono>
#include <chrono>
#include <filesystem>
#include <optional>
#include <set>
#include <span>
#include <unordered_set>

#include "CommandTable.h"
#include "GameForms.h"
#include "GameObjects.h"
#include "GameProcess.h"
#include "game_types.h"
#include "LambdaVariableContext.h"
#include "ParamInfos.h"
#include "utility.h"


struct SavedAnims;
extern std::span<TESAnimGroup::AnimGroupInfo> g_animGroupInfos;
using FormID = UInt32;
using GroupID = UInt16;

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
		float lastTime = 0;
	public:
		TimedExecution* execution = nullptr;
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
			if (!execution || execution->items.empty())
				return;
			const auto lastTime = this->lastTime;
			this->lastTime = time;
			if (index >= execution->items.size())
			{
				if (time - lastTime < -0.01f)
					// looping anim
					index = 0;
				else
					return;
			}
			auto& [item, nextTime] = execution->items.at(index);
			if (time >= nextTime)
			{
				//if (!IsPlayersOtherAnimData(animData))
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


	TimedExecution() = default;

	Context CreateContext()
	{
		return Context(this);
	}
};

struct AnimTime
{
	UInt32 actorId = 0;
	BSAnimGroupSequence* anim = nullptr;
	float lastNiTime = -FLT_MAX;
	bool finishedEndKey = false;
	bool respectEndKey = false;
	bool firstPerson = false;
	BSAnimGroupSequence* anim3rdCounterpart = nullptr;
	POVSwitchState povState = POVSwitchState::NotSet;
	TESObjectWEAP* actorWeapon = nullptr;
	TimedExecution<Script*>::Context scriptLines;
	TimedExecution<Script*>::Context scriptCalls;
	std::optional<TimedExecution<Sound>> soundPathsBase;
	TimedExecution<Sound>::Context soundPaths;
	bool allowAttack = false;

	using TimedCallbacks = TimedExecution<std::function<void()>>;
	std::optional<TimedCallbacks> callbacksBase;
	TimedCallbacks::Context callbacks;

	bool hasCustomAnimGroups = false;
	std::set<std::pair<Script*, std::string>> cleanUpScripts;

	AnimData* GetAnimData(Actor* actor) const
	{
		if (firstPerson)
			return g_thePlayer->firstPersonAnimData;
		return actor->baseProcess->GetAnimData();
	}

	bool IsPlayer() const
	{
		return actorId == g_thePlayer->refID;
	}

	explicit AnimTime(Actor* actor, BSAnimGroupSequence* anim)
		: actorId(actor->refID), anim(anim)
	{
	}

	~AnimTime();

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

struct SavedAnimsTime
{
	Script* conditionScript = nullptr;
	UInt16 groupId = 0;
	BSAnimGroupSequence* anim = nullptr;
	UInt32 actorId = 0;
	AnimData* animData = nullptr;

	friend auto operator<=>(const SavedAnimsTime& lhs, const SavedAnimsTime& rhs) = default;
};


extern std::map<BSAnimGroupSequence*, std::shared_ptr<AnimTime>> g_timeTrackedAnims;
extern std::map<std::pair<SavedAnims*, AnimData*>, std::shared_ptr<SavedAnimsTime>> g_timeTrackedGroups;


struct AnimPath
{
	std::string path;
	bool partialReload = false;
};

enum class AnimCustom
{
	None, Male, Female, Mod1, Mod2, Mod3, Hurt, Human, Max=Human
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
	std::vector<std::shared_ptr<SavedAnims>> anims;
	std::vector<std::shared_ptr<SavedAnims>> maleAnims;
	std::vector<std::shared_ptr<SavedAnims>> femaleAnims;
	std::vector<std::shared_ptr<SavedAnims>> mod1Anims;
	std::vector<std::shared_ptr<SavedAnims>> mod2Anims;
	std::vector<std::shared_ptr<SavedAnims>> mod3Anims;

	std::vector<std::shared_ptr<SavedAnims>> hurtAnims;
	std::vector<std::shared_ptr<SavedAnims>> humanAnims;

	std::vector<std::shared_ptr<SavedAnims>>& GetCustom(const AnimCustom custom)
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

using FormID = UInt32;
using FullAnimGroupID = UInt32;
// Per ref ID there is a stack of animation variants per group ID
class AnimOverrideStruct
{
public:
	std::unordered_map<FullAnimGroupID, AnimStacks> stacks;
};

using AnimOverrideMap = std::unordered_map<FormID, AnimOverrideStruct>;
AnimOverrideMap& GetAnimOverrideMap(bool firstPerson);

struct SavedAnims
{
	bool hasOrder = false;
	std::vector<std::unique_ptr<AnimPath>> anims;
	std::optional<LambdaVariableContext> conditionScript;
	bool pollCondition = false;
	std::unordered_set<BSAnimGroupSequence*> linkedSequences;

	SavedAnims() = default;
	~SavedAnims()
	{
		int i = 0;
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

enum AnimHandTypes
{
	kAnim_H2H = 0x0,
	kAnim_1HM = 0x1,
	kAnim_2HM = 0x2,
	kAnim_1HP = 0x3,
	kAnim_2HR = 0x4,
	kAnim_2HA = 0x5,
	kAnim_2HH = 0x6,
	kAnim_2HL = 0x7,
	kAnim_1GT = 0x8,
	kAnim_1MD = 0x9,
	kAnim_1LM = 0xA,
	kAnim_Max = 0xB,
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
#define _CDECL(address, returnType, ...) reinterpret_cast<returnType(__cdecl*)(__VA_ARGS__)>(address)

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
	inline auto* BSStream_Clear = THISCALL(0x43D090, void, void*);
	inline auto* GetAnims = reinterpret_cast<tList<char>*(__thiscall*)(TESObjectREFR*, int)>(0x566970);
	inline auto* LoadAnimation = reinterpret_cast<bool(__thiscall*)(AnimData*, KFModel*, bool)>(0x490500);
	inline auto* MorphToSequence = reinterpret_cast<BSAnimGroupSequence * (__thiscall*)(AnimData*, BSAnimGroupSequence*, int, int)>(0x4949A0);
	inline auto* PlayAnimGroup = reinterpret_cast<BSAnimGroupSequence * (__thiscall*)(AnimData*, int, int, int, int)>(0x494740);
	inline auto* NiTPointerMap_Lookup = reinterpret_cast<bool (__thiscall*)(void*, int, AnimSequenceBase**)>(0x49C390);
	inline auto* NiTPointerMap_RemoveKey = reinterpret_cast<bool(__thiscall*)(void*, UInt16)>(0x49C250);
	inline auto* NiTPointerMap_Init = reinterpret_cast<GameAnimMap * (__thiscall*)(GameAnimMap*, int numBuckets)>(0x49C050);
	inline auto* NiTPointerMap_Delete = reinterpret_cast<void (__thiscall*)(GameAnimMap*, bool free)>(0x49C080);

	// Multiple "Hit" per anim
	inline auto* AnimData_GetSequenceOffsetPlusTimePassed = reinterpret_cast<float (__thiscall*)(AnimData*, BSAnimGroupSequence*)>(0x493800);
	inline auto* TESAnimGroup_GetTimeForAction = reinterpret_cast<double (__thiscall*)(TESAnimGroup*, UInt32)>(0x5F3780);
	inline auto* Actor_SetAnimActionAndSequence = reinterpret_cast<void (__thiscall*)(Actor*, Decoding::AnimAction, BSAnimGroupSequence*)>(0x8A73E0);
	inline auto* AnimData_GetAnimSequenceElement = reinterpret_cast<BSAnimGroupSequence* (__thiscall*)(AnimData*, eAnimSequence a2)>(0x491040);
	
	inline auto GetControlState = _VL((ControlCode code, Decoding::IsDXKeyState state), ThisStdCall<int>(0xA24660, *g_inputGlobals, code, state));
	inline auto SetControlHeld = _VL((ControlCode code), ThisStdCall<int>(0xA24280, *g_inputGlobals, code));

	inline auto IsDoingAttackAnimation = THISCALL(0x894900, bool, Actor* actor);
	inline auto HandleQueuedAnimFlags = THISCALL(0x8BA600, void, Actor* actor);

	inline auto CrossFade = THISCALL(0xA2E280, bool, NiControllerManager* manager, NiControllerSequence* source, NiControllerSequence* destination, float blend, int priority, bool startOver, float morphWeight, NiControllerSequence* pkTimeSyncSeq);
	inline auto ActivateSequence = THISCALL(0x47AAB0, bool, NiControllerManager* manager, NiControllerSequence* source, int priority, bool bStartOver, float fWeight, float fEaseInTime, NiControllerSequence* pkTimeSyncSeq);

	inline auto MorphSequence = THISCALL(0xA351D0, bool, NiControllerSequence *source, NiControllerSequence *pkDestSequence, float fDuration, int iPriority, float fSourceWeight, float fDestWeight);
	inline auto BlendFromPose = THISCALL(0xA2F800, bool, NiControllerManager * mgr, NiControllerSequence * pkSequence, float fDestFrame, float fDuration, int iPriority, NiControllerSequence * pkSequenceToSynchronize);
	inline auto StartBlend = THISCALL(0xA350D0, bool, NiControllerSequence * pkSourceSequence, NiControllerSequence * pkDestSequence, float fDuration, float fDestFrame, int iPriority, float fSourceWeight,
		float fDestWeight, NiControllerSequence * pkSequenceToSynchronize);
	inline auto DeactivateSequence = THISCALL(0x47B220, int, NiControllerManager * mgr, NiControllerSequence * pkSequence, float fEaseOut);
	inline auto GetActorAnimGroupId = THISCALL(0x897910, UInt16, Actor* actor, UInt32 groupId, BaseProcess::WeaponInfo* weapInfo, bool aFalse, AnimData* animData);
	inline auto NiControllerManager_RemoveSequence = THISCALL(0xA2EC50, NiControllerSequence*, NiControllerManager * mgr, NiControllerSequence * anim);
	inline auto GetNearestGroupID = THISCALL(0x495740, UInt16, AnimData* animData, UInt16 groupID, bool noRecurse);
	inline auto NiControllerManager_LookupSequence = THISCALL(0x47A520, NiControllerSequence*, NiControllerManager* mgr, const char** animGroupName);
	inline auto Actor_Attack = THISCALL(0x8935F0, bool, Actor* actor, UInt32 animGroupId);
	inline auto AnimData_ResetSequenceState = THISCALL(0x496080, void, AnimData* animData, eAnimSequence sequenceId, float blendAmound);

	inline auto InitAnimGroup = _CDECL(0x5F3A20, TESAnimGroup*, BSAnimGroupSequence* anim, const char* path);
	inline auto BSFixedString_CreateFromPool = _CDECL(0xA5B690, const char*, const char* str);
	inline auto NiTextKeyExtraData_Destroy = THISCALL(0xA46D50, UInt32, NiTextKeyExtraData * textKeys);
	inline auto TESAnimGroup_Destroy = THISCALL(0x5F22A0, void, TESAnimGroup * animGroup, bool free);
	inline auto NiRefObject_Replace = THISCALL(0x66B0D0, void, void* target, void* src);
	inline auto NiRefObject_IncRefCount = THISCALL(0x40F6E0, void, void* target);
	inline auto NiRefObject_DecRefCount_FreeIfZero = THISCALL(0x401970, void, void* target);
}

BSAnimGroupSequence* GetAnimationByPath(const char* path);

void HandleOnAnimDataDelete(AnimData* animData);

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
	std::shared_ptr<SavedAnims> parent;
	SavedAnimsTime* animsTime;
	AnimationResult(std::shared_ptr<SavedAnims> parent, SavedAnimsTime* animsTime)
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
std::optional<BSAnimationContext> LoadCustomAnimation(SavedAnims& ctx, UInt16 groupId, AnimData* animData);
BSAnimGroupSequence* LoadAnimationPath(const AnimationResult& result, AnimData* animData, UInt16 groupId);

std::optional<AnimationResult> GetActorAnimation(UInt32 animGroupId, AnimData* animData);

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
	{ "poll condition", kParamType_Integer, 1 },
	{"condition", kParamType_AnyForm, 1},
};

static ParamInfo kParams_PlayAnimationPath[] =
{
	{"path", kParamType_String, 0},
	{"first person", kParamType_Integer, 1}
};

static ParamInfo kParams_SetAnimationPathCondition[] =
{
	{"path", kParamType_String, 0},
	{"condition", kParamType_AnyForm, 0},
};

static ParamInfo kParams_PlayGroupAlt[] =
{
	{"anim group", kParamType_AnimationGroup, 0},
	{"play immediately", kParamType_Integer, 1},
};

static ParamInfo kParams_CreateIdleAnimForm[] = 
{
	{"path", kParamType_String, 0},
	{"type", kParamType_Integer, 1},
};

static ParamInfo kParams_OneAnimGroup[] = {
	{	"anim group", kParamType_AnimationGroup, 0 },
};

DEFINE_COMMAND_PLUGIN(ForcePlayIdle, "", true, 2, kParams_OneForm_OneOptionalInt)
DEFINE_COMMAND_PLUGIN(SetWeaponAnimationPath, "", false, sizeof kParams_SetWeaponAnimationPath / sizeof(ParamInfo), kParams_SetWeaponAnimationPath)
DEFINE_COMMAND_PLUGIN(SetActorAnimationPath, "", false, sizeof kParams_SetActorAnimationPath / sizeof(ParamInfo), kParams_SetActorAnimationPath)
DEFINE_COMMAND_PLUGIN(PlayAnimationPath, "", true, sizeof kParams_PlayAnimationPath / sizeof(ParamInfo), kParams_PlayAnimationPath)
DEFINE_COMMAND_PLUGIN(kNVSEReset, "", false, 0, nullptr)
DEFINE_COMMAND_PLUGIN(ForceStopIdle, "", true, 1, kParams_OneOptionalInt)
DEFINE_COMMAND_PLUGIN(PlayGroupAlt, "", true, 2, kParams_PlayGroupAlt)
DEFINE_COMMAND_PLUGIN(CreateIdleAnimForm, "", false, 2, kParams_CreateIdleAnimForm);
DEFINE_COMMAND_PLUGIN(EjectWeapon, "", true, 0, nullptr);
#if _DEBUG

DEFINE_COMMAND_PLUGIN(kNVSETest, "", false, 0, nullptr);

#endif
void OverrideModIndexAnimation(UInt8 modIdx, std::string_view path, bool firstPerson, bool enable, std::unordered_set<UInt16>& groupIdFillSet, Script* conditionScript, bool pollCondition);
void OverrideFormAnimation(const TESForm* form, std::string_view path, bool firstPerson, bool enable, std::unordered_set<UInt16>& groupIdFillSet, Script* conditionScript = nullptr, bool pollCondition = false);

void HandleOnActorReload();

float GetTimePassed(AnimData* animData, UInt8 animGroupID);

bool IsCustomAnim(BSAnimGroupSequence* sequence);

BSAnimGroupSequence* GetGameAnimation(AnimData* animData, UInt16 groupID);
bool HandleExtraOperations(AnimData* animData, BSAnimGroupSequence* anim);
float GetAnimMult(const AnimData* animData, UInt8 animGroupID);
bool IsAnimGroupReload(UInt8 animGroupId);
bool IsAnimGroupMovement(UInt8 animGroupId);

bool WeaponHasNthMod(Decoding::ContChangesEntry* weaponInfo, TESObjectWEAP* weap, UInt32 mod);


int GetWeaponInfoClipSize(Actor* actor);

enum class ReloadSubscriber
{
	Partial, BurstFire, AimTransition
};

struct ReloadHandler
{
	std::unordered_map<ReloadSubscriber, bool> subscribers;
};

void SubscribeOnActorReload(Actor* actor, ReloadSubscriber subscriber);

bool DidActorReload(Actor* actor, ReloadSubscriber subscriber);

AnimPath* GetAnimPath(SavedAnims& animResult, UInt16 groupId, AnimData* animData);
int GroupNameToId(const std::string& name);

extern std::unordered_map<UInt32, ReloadHandler> g_reloadTracker;

bool IsLoopingReload(UInt8 groupId);

enum class PartialLoopingReloadState
{
	NotSet, NotPartial, Partial
};

extern PartialLoopingReloadState g_partialLoopReloadState;

void HandleOnSequenceDestroy(BSAnimGroupSequence* anim);

void HandleGarbageCollection();

void CreateCommands(NVSECommandBuilder& builder);

extern std::unordered_set<BaseProcess*> g_allowedNextAnims;

std::string GetAnimBasePath(std::string_view path);
std::string ExtractCustomAnimGroupName(const std::filesystem::path& path);

float GetDefaultBlendTime(const BSAnimGroupSequence* destSequence, const BSAnimGroupSequence* sourceSequence = nullptr);
UInt16 GetNearestGroupID(AnimData* animData, AnimGroupID animGroupId);
float GetIniFloat(UInt32 addr);
NiControllerSequence::ControlledBlock* FindAnimInterp(BSAnimGroupSequence* anim, const char* interpName);

using AnimationResultCache = std::unordered_map<std::pair<UInt32, AnimData*>, std::optional<AnimationResult>, pair_hash, pair_equal>;
extern AnimationResultCache g_animationResultCache;

using AnimPathCache = std::unordered_map<std::pair<SavedAnims*, AnimData*>, AnimPath*, pair_hash, pair_equal>;
extern AnimPathCache g_animPathFrameCache;