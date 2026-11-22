#pragma once

#include <atomic>
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
#include "anim_overrides.h"
#include "actor_overrides.h"
#include "ammo_keys.h"
#include "burst_fire.h"
#include "timed_execution.h"

extern std::span<AnimGroupInfo> g_animGroupInfos;
using FormID = UInt32;
using GroupID = UInt16;

enum class POVSwitchState
{
	NotSet, POV3rd, POV1st
};

inline std::string_view GetTextAfterColon(std::string_view in)
{
	auto str = in.substr(in.find_first_of(':') + 1);
	while (!str.empty() && isspace(*str.begin()))
		str = str.substr(1);
	return str;
}

struct Sounds
{
	BSSoundHandle sound;
	bool failed = false;
	Sounds() = default;

	static bool IsSoundFile(const std::string_view fileName)
	{
		const auto fileExt = sv::get_file_extension(fileName);
		return sv::equals_ci(fileExt, ".wav") || sv::equals_ci(fileExt, ".ogg");
	}
	
	Sounds(std::string_view path, bool is3D)
	{
		// Game has a garbage collector for sounds
		sv::stack_string<0x400> newPath = sv::stack_string<0x400>(R"(sound\%s)", path.data());
		const auto flags = is3D ? BSSoundHandle::kAudioFlags_3D : BSSoundHandle::kAudioFlags_2D;
		if (sv::get_file_extension(path).empty())
		{
			if (newPath.back() != '\\')
				newPath += '\\';
			const auto* audioManager = BSWin32Audio::GetSingleton();
			if (!audioManager->PickSoundFileFromFolder(newPath.data()))
			{
				failed = true;
				return;
			}
			newPath.calculate_size();
		}
		if (!IsSoundFile(newPath.str()))
		{
			failed = true;
			return;
		}
		this->sound = BSSoundHandle::InitByFilename(newPath.c_str(), flags);
		if (sound.soundID == -1)
		{
			failed = true;
			return;
		}
	}

	void Play(Actor* actor, bool is3D)
	{
		// pick random sound
		if (is3D)
			sound.Set3D(actor);
		sound.Play();
	}
	
	static std::optional<Sounds> FromTextKey(AnimData* animData, std::string_view key)
	{
		if (!sv::starts_with_ci(key, "SoundPath:"))
			return std::nullopt;
		const auto line = GetTextAfterColon(key);
		if (line.empty())
			return std::nullopt;
		const auto is3D = animData != PlayerCharacter::GetSingleton()->firstPersonAnimData;
		auto result = Sounds(line, is3D);
		if (result.failed)
			return std::nullopt;
		return result;
	}
};

// One play of a sequence whose keys kNVSE acts on (TextKeys::Table), or that a script started: kept in g_timeTrackedAnims
// and updated every frame on the main thread (HandleCustomTextKeys) until the play ends.
struct AnimTime
{
	// Who plays the sequence, which decides when the play ends (besides the sequence becoming inactive and the actor
	// going away)
	enum class Player : UInt8
	{
		// The engine's anim groups, or kNVSE in their place: until another sequence takes the section
		Group,
		// ActivateAnim: until it is inactive
		Script,
		// PlayAnimationPath, not looping: until its end key, then deactivated
		ScriptToEnd,
		// An additive played over a reference group (AdditiveManager): until the group leaves the weapon section, an
		// attack loop also on releasing attack or running out of ammo; then deactivated
		AdditiveOverlay,
	};

	UInt32 actorId = 0;
	// The Animation the sequence plays on. The entry is released with it (Animation::~Animation).
	AnimData* animData = nullptr;
	NiPointer<BSAnimGroupSequence> anim = nullptr;
	bool firstPerson = false;
	Player player = Player::Group;
	// AdditiveOverlay: the group it plays over
	FullAnimGroupID referenceAnimGroupId = 0xFFFF;
	// The KF is listed in sLegacyAnimTimePaths: its time is the Animation's, not the sequence's
	bool useLegacyTime = false;

	// Where this play is in the KF's timed keys
	TimedExecution<Script*>::Context scriptLines;
	TimedExecution<Script*>::Context scriptCalls;
	TimedExecution<UInt32>::Context soundKeys;
	// The sounds of the KF's SoundPath keys for this play, by TextKeys::Table::soundPaths index (a folder picks its file
	// when the play starts)
	std::vector<Sounds> sounds;
	// loadAmmo / unloadAmmo keys (AmmoKeys); they act only if ammoKeysAct
	TimedExecution<AmmoKeys::Key>::Context ammoKeys;
	bool ammoKeysAct = false;
	TimedExecution<BurstFire::Key>::Context burstKeys;
	BurstFire::State burst;

	bool allowAttack = false;
	float allowAttackTime = INVALID_TIME;
	// True if the sequence has spawn:/despawn: keys (SpawnKeys, in its SequenceExtraData).
	bool hasSpawnKeys = false;
	bool hasCustomAnimGroups = false;
	std::set<std::pair<Script*, std::string>> cleanUpScripts;

	// The player's 1st person sequence with a respectEndKey key
	bool respectEndKey = false;
	struct RespectEndKeyData
	{
		// The player's 3rd person sequence whose times this animation takes in 1st person (RespectEndKey).
		NiPointer<BSAnimGroupSequence> anim3rdCounterpart;
	};
	RespectEndKeyData respectEndKeyData;

	explicit AnimTime(Actor* actor, AnimData* animData, BSAnimGroupSequence* anim)
		: actorId(actor->refID), animData(animData), anim(anim), firstPerson(animData == PlayerCharacter::GetSingleton()->firstPersonAnimData)
	{
	}

	// Deactivates the sequence if this play started it and owns it to its end (ScriptToEnd, AdditiveOverlay)
	~AnimTime();
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

using GameAnimMap = NiTPointerMap<AnimSequenceBase*>;
using TimeTrackedAnimsMap = std::unordered_map<BSAnimGroupSequence*, std::unique_ptr<AnimTime>>;
extern TimeTrackedAnimsMap g_timeTrackedAnims;

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

float GetDefaultBlendTime(const BSAnimGroupSequence* destSequence, const BSAnimGroupSequence* sourceSequence);
float GetDefaultBlendOutTime(const BSAnimGroupSequence* destSequence);

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

float GetTimePassed(AnimData* animData, UInt8 animGroupID);

BSAnimGroupSequence* GetGameAnimation(AnimData* animData, UInt16 groupID);
// A sequence starts playing on the Animation (any caller that starts one, and after its time was set): acts on the
// keys of its KF that act at the start, and tracks the play (AnimTime) if keys act later or a script started it
// (player other than Group). Returns the play's AnimTime, nullptr if it is not tracked.
AnimTime* HandleExtraOperations(AnimData* animData, BSAnimGroupSequence* anim, AnimTime::Player player = AnimTime::Player::Group);
float GetAnimMult(const AnimData* animData, UInt8 animGroupID);
bool IsAnimGroupReload(AnimGroupID animGroupId);
bool IsAnimGroupReload(UInt8 animGroupId);
bool IsAnimGroupMovement(AnimGroupID animGroupId);
bool IsAnimGroupMovement(UInt8 animGroupId);

// Returns the KF as a sequence of the Animation: the one it already has under that name, otherwise the KF loaded as an
// override animation (Overrides::Load).
BSAnimGroupSequence* FindOrLoadAnim(AnimData* animData, const char* path);
// Returns the full ID of the anim group a KF is of, from its file name or else its contents. Returns
// Overrides::kInvalidGroupId if neither identifies it.
UInt16 GetAnimGroupId(std::string_view path);

void HandleGarbageCollection();

void CreateCommands(NVSECommandBuilder& builder);

extern std::unordered_set<BaseProcess*> g_allowedNextAnims;

std::string_view GetAnimBasePath(std::string_view path);
std::string_view ExtractCustomAnimGroupName(std::string_view path);

UInt16 GetNearestGroupID(AnimData* animData, AnimGroupID animGroupId);
float GetIniFloat(UInt32 addr);
NiControllerSequence::InterpArrayItem* FindAnimInterp(BSAnimGroupSequence* anim, const char* interpName);

// True if this thread caches the results of condition scripts for the frame (CallFunction).
extern thread_local bool g_isThreadCacheEnabled;

std::string_view GetBaseAnimGroupName(std::string_view name);

AnimGroupID GroupNameToId(std::string_view name);
// The group for a name cut at its first '_', the way KFModel::InitSequence names a sequence.
AnimGroupID SimpleGroupNameToId(std::string_view name);

extern std::recursive_mutex g_animTimeMutex;

// kNVSEReset: registers the override files again (sets registered by scripts are dropped) and re-reads every KF from
// disk when it next plays. The player's animations restart if an override was playing. Main thread, and only while the
// other threads are not picking animations (console, menus).
void ReloadOverrideFiles();

extern "C" { 
	__declspec(dllexport) void PluginOverrideFormAnimation(const TESForm* form, const char* path, bool firstPerson, bool enable, Script* conditionScript, bool pollCondition);
	__declspec(dllexport) bool CopyAnimationsToForm(TESForm* fromForm, TESForm* toForm);
	__declspec(dllexport) bool ClearFormAnimations(TESForm* form);
}