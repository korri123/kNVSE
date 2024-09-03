#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <set>
#include <shared_mutex>
#include <span>
#include <unordered_set>

#include "CommandTable.h"
#include "GameForms.h"
#include "GameObjects.h"
#include "GameProcess.h"
#include "game_types.h"
#include "LambdaVariableContext.h"
#include "ParamInfos.h"
#include "string_view_util.h"
#include "utility.h"
#include "bethesda/bethesda_types.h"

extern std::span<AnimGroupInfo> g_animGroupInfos;
using FormID = UInt32;
using GroupID = UInt16;

struct BurstFireData
{
	bool firstPerson = false;
	NiPointer<BSAnimGroupSequence> anim;
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

enum class POVSwitchState
{
	NotSet, POV3rd, POV1st
};

struct Sounds
{
	std::vector<BSSoundHandle> sounds;
	bool failed = false;

	Sounds() = default;

	static bool IsSoundFile(const std::string_view& fileName)
	{
		const auto fileExt = sv::get_file_extension(fileName);
		return fileExt == ".wav" || fileExt == ".mp3" || fileExt == ".ogg";
	}
	
	Sounds(std::string_view path, bool is3D)
	{
		const auto flags = is3D ? BSSoundHandle::kAudioFlags_3D : BSSoundHandle::kAudioFlags_2D;
		if (sv::get_file_extension(path).empty())
		{
			const std::string searchPath = FormatString(R"(data\sound\%s\*)", path.data());
			const auto result = FileFinder::FindFiles(searchPath.c_str(), searchPath.c_str(), ARCHIVE_TYPE_SOUNDS);
			for (const auto* file : result)
			{
				auto fileName = sv::get_file_name(file);
				if (!IsSoundFile(fileName))
					continue; // for some reason we get . and .. files
				auto sound = BSSoundHandle::InitByFilename(file, flags);
				if (!sound.soundID)
					continue;
				sounds.emplace_back(sound);
			}
			if (sounds.empty())
				failed = true;
			return;
		}
		if (!IsSoundFile(path))
		{
			failed = true;
			return;
		}
		const std::string realPath = FormatString(R"(data\sound\%s)", path.data());
		auto sound = BSSoundHandle::InitByFilename(realPath.c_str(), flags);
		if (!sound.soundID)
			failed = true;
		else
			sounds = { sound };
	}

	void Play(Actor* actor, bool is3D)
	{
		if (sounds.empty())
			return;
		// pick random sound
		auto& sound = sounds.at(GetRandomUInt(sounds.size()));
		if (is3D)
			sound.Set3D(actor);
		sound.Play();
	}
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
	NiPointer<BSAnimGroupSequence> anim = nullptr;
	float lastNiTime = -FLT_MAX;
	bool respectEndKey = false;
	bool firstPerson = false;
	TimedExecution<Script*>::Context scriptLines;
	TimedExecution<Script*>::Context scriptCalls;
	std::optional<TimedExecution<Sounds>> soundPathsBase;
	TimedExecution<Sounds>::Context soundPaths;
	bool allowAttack = false;
	float allowAttackTime = INVALID_TIME;

	using TimedCallbacks = TimedExecution<std::function<void()>>;
	std::optional<TimedCallbacks> callbacksBase;
	TimedCallbacks::Context callbacks;

	bool hasCustomAnimGroups = false;
	std::set<std::pair<Script*, std::string>> cleanUpScripts;

	struct RespectEndKeyData
	{
		POVSwitchState povState = POVSwitchState::NotSet;
		BSAnimGroupSequence* anim3rdCounterpart = nullptr;
	};
	RespectEndKeyData respectEndKeyData;

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

struct SavedAnimsTime
{
	Script* conditionScript = nullptr;
	UInt16 groupId = 0;
	NiPointer<BSAnimGroupSequence> anim = nullptr;
	UInt32 actorId = 0;
	AnimData* animData = nullptr;

	friend auto operator<=>(const SavedAnimsTime& lhs, const SavedAnimsTime& rhs) = default;
};

struct AnimPath
{
	std::string_view path;
	bool partialReload = false;
	bool isStartAnim = false;
};

enum class FolderConditionType
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

struct SavedAnims
{
	std::vector<std::unique_ptr<AnimPath>> anims; // inludes all random variants, or ordered variants, or in case reloads normal and partial reload animations
	std::unordered_set<NiPointer<BSAnimGroupSequence>> linkedSequences;
	bool hasOrder = false;
	bool loaded = false;
	std::function<bool(const Actor*)> folderCondition;
	FolderConditionType folderConditionType = FolderConditionType::None;
	LambdaVariableContext conditionScript = nullptr;
	std::string_view conditionScriptText;
	bool pollCondition = false;
	bool matchBaseGroupId = false;
	bool hasStartAnim = false;
	bool hasPartialReload = false;
	bool disabled = false;
	SavedAnims() = default;

	bool MatchesConditions(const Actor* actor) const
	{
		if (!folderCondition)
			return true;
		return folderCondition(actor);
	}

	void Load()
	{
		if (!conditionScript && !conditionScriptText.empty())
			conditionScript = CompileConditionScript(conditionScriptText);

		for (const auto& anim : anims)
		{
			const auto fileStem = sv::get_file_stem(anim->path);
			if (!hasOrder && sv::contains_ci(fileStem, "_order_"))
				hasOrder = true;
			if (!hasPartialReload && !anim->partialReload && sv::contains_ci(fileStem, "_partial"))
			{
				anim->partialReload = true;
				hasPartialReload = true;
			}
			if (!hasStartAnim && !anim->isStartAnim && sv::ends_with_ci(fileStem, "_start"))
			{
				anim->isStartAnim = true;
				hasStartAnim = true;
			}
		}
		if (hasOrder)
			std::ranges::sort(anims, [&](const auto& a, const auto& b) {return a->path < b->path; });
		loaded = true;
	}
};

struct AnimStacks
{
	std::vector<std::unique_ptr<SavedAnims>> anims;
};

// Per ref ID there is a stack of animation variants per group ID
class AnimOverrideStruct
{
public:
	std::unordered_map<FullAnimGroupID, AnimStacks> stacks;
};

using AnimOverrideMap = std::unordered_map<FormID, AnimOverrideStruct>;

struct BurstState
{
	int index = 0;
	BurstState() = default;
};

struct JSONAnimContext
{
	std::string condition;
	bool pollCondition = false;
	bool matchBaseGroupId = false;
#if _DEBUG
	std::string conditionScriptText;
#endif
	void Reset()
	{
		condition.clear();
		pollCondition = false;
		matchBaseGroupId = false;
#if _DEBUG
		conditionScriptText.clear();
#endif
	}

	JSONAnimContext() { Reset(); }
};

using TimeTrackedAnimsMap = std::unordered_map<BSAnimGroupSequence*, std::unique_ptr<AnimTime>>;
extern TimeTrackedAnimsMap g_timeTrackedAnims;

using TimeTrackedGroupsKey = std::pair<SavedAnims*, AnimData*>;
using TimeTrackedGroupsPair = std::pair<const TimeTrackedGroupsKey, std::unique_ptr<SavedAnimsTime>>;
using TimeTrackedGroupsMap = std::unordered_map<TimeTrackedGroupsKey, std::unique_ptr<SavedAnimsTime>, pair_hash, pair_equal>;
extern TimeTrackedGroupsMap g_timeTrackedGroups;

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
	inline auto* TransitionToSequence = reinterpret_cast<BSAnimGroupSequence * (__thiscall*)(AnimData*, BSAnimGroupSequence*, int, int)>(0x4949A0);
	inline auto* PlayAnimGroup = reinterpret_cast<BSAnimGroupSequence * (__thiscall*)(AnimData*, UInt16, int, int, int)>(0x494740);
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
}

void HandleOnAnimDataDelete(AnimData* animData);


class AnimationResult
{
public:
	SavedAnims* animBundle;
	AnimationResult(SavedAnims* animBundle): animBundle(animBundle)
	{
	}
};

struct BSAnimationContext
{
	NiPointer<BSAnimGroupSequence> anim;
	AnimSequenceBase* base;

	BSAnimationContext(BSAnimGroupSequence* anim, AnimSequenceBase* base): anim(anim), base(base)
	{
	}
};

std::optional<BSAnimationContext> LoadCustomAnimation(std::string_view path, AnimData* animData);
std::optional<BSAnimationContext> LoadCustomAnimation(SavedAnims& animBundle, UInt16 groupId, AnimData* animData);
BSAnimGroupSequence* LoadAnimationPath(const AnimationResult& result, AnimData* animData, UInt16 groupId);

std::optional<AnimationResult> GetActorAnimation(FullAnimGroupID animGroupId, AnimData* animData);

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
	{"match base anim group id", kParamType_Integer, 1},
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

struct AnimOverrideData
{
	std::string_view path;
	UInt32 identifier{};
	bool enable{};
	std::unordered_set<UInt16> groupIdFillSet;
	std::string_view conditionScriptText;
	Script* conditionScript{};
	bool pollCondition{};
	bool matchBaseGroupId{};
};

bool OverrideModIndexAnimation(AnimOverrideData& data, bool firstPerson);
bool OverrideFormAnimation(AnimOverrideData& data, bool firstPerson);

void HandleOnActorReload();

float GetTimePassed(AnimData* animData, UInt8 animGroupID);

BSAnimGroupSequence* GetGameAnimation(AnimData* animData, UInt16 groupID);
bool HandleExtraOperations(AnimData* animData, BSAnimGroupSequence* anim);
float GetAnimMult(const AnimData* animData, UInt8 animGroupID);
bool IsAnimGroupReload(AnimGroupID animGroupId);
bool IsAnimGroupReload(UInt8 animGroupId);
bool IsAnimGroupMovement(AnimGroupID animGroupId);
bool IsAnimGroupMovement(UInt8 animGroupId);

bool WeaponHasNthMod(Decoding::ContChangesEntry* weaponInfo, TESObjectWEAP* weap, UInt32 mod);

BSAnimGroupSequence* FindOrLoadAnim(AnimData* animData, const char* path);

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

extern std::unordered_map<UInt32, ReloadHandler> g_reloadTracker;

bool IsLoopingReload(UInt8 groupId);

enum class PartialLoopingReloadState
{
	NotSet, NotPartial, Partial
};

extern PartialLoopingReloadState g_partialLoopReloadState;

void HandleGarbageCollection();

void CreateCommands(NVSECommandBuilder& builder);

extern std::unordered_set<BaseProcess*> g_allowedNextAnims;

std::string_view GetAnimBasePath(std::string_view path);
std::string_view ExtractCustomAnimGroupName(std::string_view path);

float GetDefaultBlendTime(const BSAnimGroupSequence* destSequence, const BSAnimGroupSequence* sourceSequence = nullptr);
UInt16 GetNearestGroupID(AnimData* animData, AnimGroupID animGroupId);
float GetIniFloat(UInt32 addr);
NiControllerSequence::InterpArrayItem* FindAnimInterp(BSAnimGroupSequence* anim, const char* interpName);

using AnimationResultKey = std::pair<UInt32, AnimData*>;
using AnimationResultValue = std::optional<AnimationResult>;
using AnimationResultPair = std::pair<const AnimationResultKey, AnimationResultValue>;
using AnimationResultCache = std::unordered_map<AnimationResultKey, AnimationResultValue, pair_hash, pair_equal>;
extern AnimationResultCache g_animationResultCache;

using AnimPathKey = std::pair<SavedAnims*, AnimData*>;
using AnimPathValue = AnimPath*;
using AnimPathPair = std::pair<const AnimPathKey, AnimPathValue>;
using AnimPathCache = std::unordered_map<AnimPathKey, AnimPathValue, pair_hash, pair_equal>;

extern AnimPathCache g_animPathFrameCache;

std::string_view GetBaseAnimGroupName(std::string_view name);

extern std::shared_mutex g_animMapMutex;
extern std::shared_mutex g_animPathFrameCacheMutex;