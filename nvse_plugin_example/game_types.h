#pragma once
#include "GameObjects.h"
#include "GameOSDepend.h"

namespace Decoding
{

	enum class IsDXKeyState
	{
		IsHeld = 0x0,
		IsPressed = 0x1,
		IsDepressed = 0x2,
		IsChanged = 0x3,
	};


	struct PackageInfo
	{
		TESPackage* package;		// 00
		union
		{
			TESPackageData* packageData;	// 04
			void* actorPkgData;
		};
		TESObjectREFR* targetRef;		// 08
		UInt32			unk0C;			// 0C	Initialized to 0FFFFFFFFh, set to 0 on start
		float			unk10;			// 10	Initialized to -1.0	. Set to GameHour on start so some time
		UInt32			flags;			// 14	Flags, bit0 would be not created and initialized
	};

	struct QueuedEquipItem
	{
		TESForm* item;
		BaseExtraList* extraData;
		UInt32 count;
		UInt8 isEquipNotUnequip;
		UInt8 shouldApplyEnchantment;
		UInt8 noUnequip;
		UInt8 shouldPlayEquipSound;
		UInt8 byte10;
		UInt8 byte11;
		UInt8 gap12[2];
		NiRefObject* modelFile;
	};


	// B4
	class LowProcess : public BaseProcess
	{
	public:
		struct FloatPair
		{
			float	flt000;
			float	flt004;
		};

		struct ActorValueModifier
		{
			UInt8	actorValue;	// 00 Might allow for other values
			UInt8	pad[3];		// 01
			float	damage;		// 04
		};

		struct ActorValueModifiers
		{
			tList<ActorValueModifier>	avModifierList;	// 00
			UInt8						unk008;			// 08
			UInt8						pad009[3];		// 09
			void** modifiedAV;	// 0C	array of damaged actorValue
		};	// 10

		virtual void	Unk_1EE();
		virtual void	Unk_1EF();
		virtual void	Unk_1F0();
		virtual void	Unk_1F1();
		virtual void	Unk_1F2();
		virtual void	Unk_1F3();
		virtual void	Unk_1F4();
		virtual void	Unk_1F5();
		virtual void	Unk_1F6();
		virtual void	Unk_1F7();
		virtual void	Unk_1F8();
		virtual void	Unk_1F9();
		virtual void	Unk_1FA();
		virtual void	Unk_1FB();
		virtual void	Unk_1FC();
		virtual void	Unk_1FD();
		virtual void	Unk_1FE();
		virtual void	Unk_1FF();
		virtual void	Unk_200();
		virtual void	Unk_201();
		virtual void	Unk_202();
		virtual void	Unk_203();
		virtual void	Unk_204();
		virtual void	Unk_205();
		virtual void	Unk_206();

		UInt8				byte30;		// 8 = IsAlerted
		UInt8				pad31[3];
		UInt32				unk34;
		FloatPair			unk38;
		TESForm* unk40;		// Used when picking idle anims.
		UInt32				unk44;		// not initialized!	refr, expected actor, might be CombatTarget
		UInt32				unk48;
		UInt32				unk4C;
		UInt32				unk50;
		UInt32				unk54;		// not initialized!
		UInt32				unk58;
		UInt32				unk5C;
		tList<UInt32>		unk60;		// List
		UInt32				unk68;
		UInt32				unk6C;
		tList<TESForm>		unk70;
		tList<UInt32>		unk78;
		tList<UInt32>		unk80;
		UInt32				unk88;
		UInt32				unk8C;
		UInt32				unk90;
		UInt32				unk94;
		ActorValueModifiers	damageModifiers;
		float				gameDayDied;
		UInt32				unkAC;		// not initialized!
		UInt32				unkB0;		// not initialized!
	};

	class MiddleLowProcess : public LowProcess
	{
	public:
		virtual void		Unk_207();

		UInt32				unk0B4;			// B4
		ActorValueModifiers	tempModifiers;	// B8
	};
	
	// 25C
	class MiddleHighProcess : public MiddleLowProcess
	{
	public:
		virtual void	SetAnimation(UInt32 newAnimation);
		virtual void	Unk_209();
		virtual void	Unk_20A();
		virtual void	Unk_20B();
		virtual void	Unk_20C();
		virtual void	Unk_20D();
		virtual void	Unk_20E();
		virtual void	Unk_20F();
		virtual void	Unk_210();
		virtual void	Unk_211();
		virtual void	Unk_212();
		virtual void	Unk_213();
		virtual void	Unk_214();
		virtual void	Unk_215();
		virtual void	Unk_216();
		virtual void	Unk_217();
		virtual void	Unk_218();
		virtual void	Unk_219();
		virtual void	Unk_21A();
		virtual void	Unk_21B();

		enum KnockedState
		{
			kState_None,
			kState_KnockedDown,
			kState_Ragdolled,
			kState_Unconscious,
			kState_Unknown4,
			kState_Unknown5,
			kState_GettingUp
		};

		tList<TESForm>						unk0C8;				// 0C8
		tList<UInt32>						unk0D0;				// 0D0
		UInt32								unk0D8[3];			// 0D8
		PackageInfo							interruptPackage;	// 0E4
		UInt8								unk0FC[12];			// 0FC	Saved as one, might be Pos/Rot given size
		UInt32								unk108;				// 108
		TESIdleForm* LastPlayedIdle;	// 10C
		UInt32								unk110;				// 110  EntryData, also handled as part of weapon code. AmmoInfo.
		ExtraContainerChanges::EntryData* weaponInfo;		// 114
		ExtraContainerChanges::EntryData* ammoInfo;			// 118
		QueuedFile* unk11C;			// 11C
		UInt8								byt120;				// 120
		UInt8								byt121;				// 121
		UInt8								byt122;				// 122
		UInt8								fil123;				// 123
		UInt8								usingOneHandGrenade;// 124
		UInt8								usingOneHandMine;	// 125
		UInt8								usingOneHandThrown;	// 126
		UInt8								byte127;			// 127
		UInt32								unk128;				// 128 Gets copied over during TESNPC.CopyFromBase
		NiNode* weaponNode;		// 12C
		NiNode* projectileNode;	// 130
		UInt8								wantsWeaponOut;				// 134
		bool								isWeaponOut;		// 135
		UInt8								byt136;				// 136
		UInt8								byt137;				// 137
		bhkCharacterController* charCtrl;			// 138
		UInt8								knockedState;		// 13C
		UInt8								sitSleepState;		// 13D
		UInt8								unk13E[2];			// 13E
		TESObjectREFR* usedFurniture;		// 140
		UInt8								byte144;			// 144
		UInt8								unk145[3];			// 145
		UInt32								unk148[6];			// 148
		MagicItem* magicItem160;		// 160
		UInt32								unk164[3];			// 164
		float								actorAlpha;			// 170
		UInt32								unk174;				// 174
		BSFaceGenAnimationData* unk178;			// 178
		UInt8								byte17C;			// 17C
		UInt8								byte17D;			// 17D
		UInt8								byte17E;			// 17E
		UInt8								byte17F;			// 17F
		UInt32								unk180;				// 180
		UInt32								unk184;				// 184
		UInt8								hasCaughtPCPickpocketting;	// 188
		UInt8								byte189;			// 189
		UInt8								byte18A;			// 18A
		UInt8								byte18B;			// 18B
		UInt8								byte18C;			// 18C
		UInt8								byte18D[3];			// 18D
		UInt32								unk190[10];			// 190
		void* unk1B8;			// 1B8
		MagicTarget* magicTarget1BC;	// 1BC
		AnimData* animData;			// 1C0
		BSAnimGroupSequence* weaponSequence[3];	// 1C4
		float angle1D0;
		float time1D4;
		UInt8 byte1D8;
		UInt8 isUsingAutomaticWeapon;
		UInt8 bGetAttacked;
		UInt8 gap1DB;
		NiNode* limbNodes[15];		// 1DC
		NiNode* unk218;			// 218
		NiNode* unk21C;			// 21C
		void* ptr220;			// 220
		BSBound* boundingBox;		// 224
		bool								isAiming;			// 228
		UInt8								pad229[3];			// 229
		int unk22C;			// 22C
		tList<QueuedEquipItem> queuedEquipItems; // 230
		float								rads238;			// 238
		float								waterRadsSec;		// 23C
		void* hitData240;		// 240
		UInt32								unk244;				// 244
		BSFaceGenNiNode* unk248;			// 248
		BSFaceGenNiNode* unk24C;			// 24C
		NiTriShape* unk250;			// 250
		void* hitData254;		// 254
		UInt32								unk258;				// 258
	};
	STATIC_ASSERT(sizeof(MiddleHighProcess) == 0x25C);

	enum AnimAction : SInt16
	{
		kAnimAction_None = -0x1,
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

	struct ActorValueArrayValue
	{
		UInt8 isModified;
		UInt8 gap01[3];
		float value;
	};

	struct ActorValueArray
	{
		struct Value
		{
			UInt8 isModified;
			UInt8 gap01[3];
			float value;
		};

		Value avs[77];
	};

	
	// 46C
	class HighProcess : public MiddleHighProcess
	{
	public:
		tList<void>* detectedActors;	// 25C
		tList<void>* detectingActors;	// 260
		void* ptr264;			// 264
		void* ptr268;			// 268
		void* ptr26C;			// 26C
		UInt32								unk270;				// 270
		tList<void>					list274;			// 274
		tList<void>							list27C;			// 27C
		tList<void>							list284;			// 284
		tList<void>							list28C;			// 28C
		float								flt294;				// 294
		float								flt298;				// 298
		UInt32								unk29C;				// 29C
		float								flt2A0;				// 2A0
		UInt32								unk2A4;				// 2A4
		float								flt2A8;				// 2A8
		UInt32								unk2AC;				// 2AC
		float								actorAlpha2;		// 2B0
		float								flt2B4;				// 2B4
		float								flt2B8;				// 2B8
		float								flt2BC;				// 2BC
		UInt16								word2C0;			// 2C0
		UInt8								byte2C2;			// 2C2
		UInt8								byte2C3;			// 2C3
		UInt8								byte2C4;			// 2C4
		UInt8								byte2C5;			// 2C5
		UInt8								byte2C6;			// 2C6
		UInt8								byte2C7;			// 2C7
		float								flt2C8;				// 2C8
		UInt32								unk2CC;				// 2CC
		float								flt2D0;				// 2D0
		float								flt2D4;				// 2D4
		float								flt2D8;				// 2D8
		UInt32								unk2DC;				// 2DC
		float								flt2E0;				// 2E0
		void* ptr2E4;			// 2E4
		UInt32								unk2E8;				// 2E8
		AnimAction							currentAction;		// 2EC
		UInt8								pad2EE[2];			// 2EE
		BSAnimGroupSequence* currentSequence;	// 2F0
		UInt8								forceFireWeapon;	// 2F4
		UInt8								pad2F5[3];			// 2F5
		float								flt2F8;				// 2F8
		UInt32								unk2FC[5];			// 2FC
		float								flt310;				// 310
		UInt32								unk314[6];			// 314
		UInt8								byte32C;			// 32C
		UInt8								byte32D;			// 32D
		UInt8								byte32E;			// 32E
		UInt8								byte32F;			// 32F
		float								flt330;				// 330
		float								flt334;				// 334
		float								flt338;				// 338
		float								diveBreath;			// 33C
		UInt32								unk340;				// 340
		float								flt344;				// 344
		UInt32								unk348;				// 348
		float								flt34C;				// 34C
		TESIdleForm* idleForm350;		// 350
		UInt32								unk354[4];			// 354
		void** ptr364;			// 364
		UInt32								unk368[4];			// 368
		float								flt378;				// 378
		float								flt37C;				// 37C
		UInt32								unk380;				// 380
		float								flt384;				// 384
		float								flt388;				// 388
		tList<void>							list38C;			// 38C
		tList<void>							list394;			// 394
		UInt32								unk39C;				// 39C
		UInt32								unk3A0;				// 3A0
		float								flt3A4;				// 3A4
		UInt32								unk3A8[5];			// 3A8
		float								flt3BC;				// 3BC
		float								flt3C0;				// 3C0
		float								lightAmount;		// 3C4
		float								flt3C8;				// 3C8
		UInt32								unk3CC;				// 3CC
		UInt32								unk3D0;				// 3D0
		void* projData;			// 3D4
		UInt32								unk3D8;				// 3D8
		void* detectionEvent;	// 3DC
		UInt32								unk3E0;				// 3E0
		UInt32								unk3E4;				// 3E4
		UInt32								fadeType;			// 3E8
		float								delayTime;			// 3EC
		UInt32								unk3F0;				// 3F0
		UInt32								unk3F4;				// 3F4
		UInt32								unk3F8[3];			// 3F8
		Actor* combatTarget;		// 404
		UInt32								unk408[4];			// 408
		float								flt418;				// 418
		TESObjectREFR* packageTarget;		// 41C
		UInt32								unk420;				// 420
		UInt32								queuedIdleFlags;	// 424
		ActorValueArray*					actorValueArray428;	// 428
		float								flt42C;				// 42C
		UInt32								unk430;				// 430
		void* ptr434;			// 434
		UInt32								unk438;				// 438
		float								unk43C;				// 43C
		float								radsSec440;			// 440
		UInt8								plantedExplosive;	// 444
		UInt8								pad445[3];			// 445
		float								flt448;				// 448
		UInt32								unk44C;				// 44C
		float								flt450;				// 450
		UInt32								unk454[6];			// 454
	};
	STATIC_ASSERT(sizeof(HighProcess) == 0x46C);

	class ExtendDataList : public tList<ExtraDataList>
	{
	public:
		SInt32 AddAt(ExtraDataList* item, SInt32 index);
		void RemoveAll() const;
		ExtraDataList* RemoveNth(SInt32 n);
	};
	
	struct ContChangesEntry
	{
		ExtendDataList* extendData;
		SInt32			countDelta;
		TESForm* type;
	};

	struct OSInputGlobals
	{
		enum
		{
			kFlag_HasJoysticks = 1 << 0,
			kFlag_HasMouse = 1 << 1,
			kFlag_HasKeyboard = 1 << 2,
			kFlag_BackgroundMouse = 1 << 3,
		};

		enum
		{
			kMaxControlBinds = 0x1C,
		};
		UInt8 isControllerDisabled;
		UInt8 byte0001;
		UInt8 byte0002;
		UInt8 byte0003;
		UInt32 flags;
		void* directInput;
		UInt32 unk000C;
		UInt32 unk0010;
		UInt32 unk0014;
		UInt32 unk0018;
		UInt32 unk001C;
		UInt32 unk0020;
		UInt32 unk0024;
		UInt32 unk0028;
		void* unk002C;
		void* unk0030;
		UInt32 unk0034[320];
		UInt32 unk534[1264];
		UInt32 unk18F4;
		UInt8 currKeyStates[256];
		UInt8 lastKeyStates[256];
		UInt32 unk1AF8;
		UInt32 unk1AFC;
		UInt32 unk1B00;
		UInt32 unk1B04;
		UInt32 numMouseButtons;
		UInt32 unk1B0C;
		UInt32 unk1B10;
		UInt32 unk1B14;
		UInt32 unk1B18;
		UInt32 unk1B1C;
		UInt32 unk1B20;
		int xDelta;
		int yDelta;
		int mouseWheelScroll;
		UInt8 currButtonStates[8];
		int lastxDelta;
		int lastyDelta;
		int lastMouseWheelScroll;
		UInt8 lastButtonStates[8];
		UInt32 swapLeftRightMouseButtons;
		UInt8 mouseSensitivity;
		UInt8 byte1B51;
		UInt8 byte1B52;
		UInt8 byte1B53;
		UInt32 doubleClickTime;
		UInt8 buttonStates1B58[8];
		UInt32 unk1B60[8];
		UInt32* controllerVibration;
		UInt32 unk1B84;
		UInt8 isControllerEnabled;
		UInt8 byte1B89;
		UInt8 byte1B8A;
		UInt8 byte1B8B;
		UInt32 unk1B8C;
		UInt8 byte1B90;
		UInt8 byte1B91;
		UInt16 overrideFlags;
		UInt8 keyBinds[28];
		UInt8 mouseBinds[28];
		UInt8 joystickBinds[28];
		UInt8 controllerBinds[28];
	};

}

enum ControlType
{
	kControlType_Keyboard,
	kControlType_Mouse,
	kControlType_Joystick
};


enum AnimState
{
	kAnimState_Inactive = 0x0,
	kAnimState_Animating = 0x1,
	kAnimState_EaseIn = 0x2,
	kAnimState_EaseOut = 0x3,
	kAnimState_TransSource = 0x4,
	kAnimState_TransDest = 0x5,
	kAnimState_MorphSource = 0x6,
};

enum class ControlCode
{
	Forward = 0x0,
	Backward = 0x1,
	Left = 0x2,
	Right = 0x3,
	Attack = 0x4,
	Activate = 0x5,
	Aim = 0x6,
	ReadyItem = 0x7,
	Crouch = 0x8,
	Run = 0x9,
	AlwaysRun = 0xA,
	AutoMove = 0xB,
	Jump = 0xC,
	TogglePOV = 0xD,
	MenuMode = 0xE,
	Rest = 0xF,
	VATS_ = 0x10,
	Hotkey1 = 0x11,
	AmmoSwap = 0x12,
	Hotkey3 = 0x13,
	Hotkey4 = 0x14,
	Hotkey5 = 0x15,
	Hotkey6 = 0x16,
	Hotkey7 = 0x17,
	Hotkey8 = 0x18,
	QuickSave = 0x19,
	QuickLoad = 0x1A,
	Grab = 0x1B,
	Escape = 0x1C,
	Console = 0x1D,
	Screenshot = 0x1E,
};

// 0C
struct Sound
{
	UInt32 soundID{};
	UInt8 success{};
	UInt8 pad05{};
	UInt8 pad06{};
	UInt8 pad07{};
	UInt32 unk08{};

	Sound() = default;

	Sound(const char* soundPath, UInt32 flags)
	{
		ThisStdCall(0xAD7550, CdeclCall<void*>(0xAD9060), this, soundPath, flags);
	}

	void Play()
	{
		ThisStdCall(0xAD8830, this, 1);
	}

	void PlayDelayed(int delayMS, int unused)
	{
		ThisStdCall(0xAD8870, this, delayMS, unused);
	}

	static Sound InitByFilename(const char* path)
	{
		Sound sound;
		ThisStdCall(0xAD7480, CdeclCall<void*>(0x453A70), &sound, path, 0, nullptr);
		return sound;
	}

	void Set3D(Actor* actor)
	{
		if (!actor)
			return;
		auto* pos = actor->GetPos();
		auto* node = actor->GetNiNode();
		if (pos && node)
		{
			ThisStdCall(0xAD8B60, this, pos->x, pos->y, pos->z);
			ThisStdCall(0xAD8F20, this, node);
		}
	}
};

extern OSInputGlobals** g_inputGlobals;

NiAVObject* __fastcall GetNifBlock(TESObjectREFR* thisObj, UInt32 pcNode, const char* blockName);

#define GetExtraType(xDataList, Type) (Extra ## Type*)xDataList.GetByType(kExtraData_ ## Type)

TESForm* __stdcall LookupFormByRefID(UInt32 refID);
void FormatScriptText(std::string& str);

UInt16 GetActorRealAnimGroup(Actor* actor, UInt8 groupID);

// 64
struct ActorHitData
{
	enum HitFlags
	{
		kFlag_TargetIsBlocking = 1,
		kFlag_TargetWeaponOut = 2,
		kFlag_IsCritical = 4,
		kFlag_OnDeathCritEffect = 8,
		kFlag_IsFatal = 0x10,
		kFlag_DismemberLimb = 0x20,
		kFlag_ExplodeLimb = 0x40,
		kFlag_CrippleLimb = 0x80,
		kFlag_BreakWeaponNonEmbedded = 0x100,
		kFlag_BreakWeaponEmbedded = 0x200,
		kFlag_IsSneakAttack = 0x400,
		kFlag_ArmorPenetrated = 0x80000000	// JIP only
	};

	Actor* source;		// 00
	Actor* target;		// 04
	union								// 08
	{
		void* projectile;
		void* explosion;
	};
	UInt32				weaponAV;		// 0C
	SInt32				hitLocation;	// 10
	float				healthDmg;		// 14
	float				wpnBaseDmg;		// 18	Skill and weapon condition modifiers included
	float				fatigueDmg;		// 1C
	float				limbDmg;		// 20
	float				blockDTMod;		// 24
	float				armorDmg;		// 28
	float				weaponDmg;		// 2C
	TESObjectWEAP* weapon;		// 30
	float				healthPerc;		// 34
	NiPoint3			impactPos;		// 38
	NiPoint3			impactAngle;	// 44
	UInt32				unk50;			// 50
	void* ptr54;			// 54
	UInt32				flags;			// 58
	float				dmgMult;		// 5C
	SInt32				unk60;			// 60	Unused; rigged by CopyHitDataHook to store hitLocation
};


enum APCostActions
{
	kActionPointsAttackUnarmed = 0x0,
	kActionPointsAttackOneHandMelee = 0x1,
	kActionPointsAttackTwoHandMelee = 0x2,
	kActionPointsAttackPistol = 0x3,
	kActionPointsAttackRifle = 0x4,
	kActionPointsAttackHandle = 0x5,
	kActionPointsAttackLauncher = 0x6,
	kActionPointsAttackGrenade = 0x7,
	kActionPointsAttackMine = 0x8,
	kActionPointsReload = 0x9,
	kActionPointsCrouch = 0xA,
	kActionPointsStand = 0xB,
	kActionPointsSwitchWeapon = 0xC,
	kActionPointsToggleWeaponDrawn = 0xD,
	kActionPointsHeal = 0xE,
	kActionPointsVATSUnarmedAttack1 = 0x11,
	kActionPointsOneHandThrown = 0x13,
	kActionPointsAttackThrown = 0x14,
	kActionPointsVATSUnarmedAttackGround = 0x15,
};

enum ActorValueCode
{
	kAVCode_Aggression = 0x0,
	kAVCode_Confidence = 0x1,
	kAVCode_Energy = 0x2,
	kAVCode_Responsibility = 0x3,
	kAVCode_Mood = 0x4,
	kAVCode_Strength = 0x5,
	kAVCode_Perception = 0x6,
	kAVCode_Endurance = 0x7,
	kAVCode_Charisma = 0x8,
	kAVCode_Intelligence = 0x9,
	kAVCode_Agility = 0xA,
	kAVCode_Luck = 0xB,
	kAVCode_ActionPoints = 0xC,
	kAVCode_CarryWeight = 0xD,
	kAVCode_CritChance = 0xE,
	kAVCode_HealRate = 0xF,
	kAVCode_Health = 0x10,
	kAVCode_MeleeDamage = 0x11,
	kAVCode_DamageResistance = 0x12,
	kAVCode_PoisonResist = 0x13,
	kAVCode_RadResist = 0x14,
	kAVCode_SpeedMult = 0x15,
	kAVCode_Fatigue = 0x16,
	kAVCode_Karma = 0x17,
	kAVCode_XP = 0x18,
	kAVCode_PerceptionCondition = 0x19,
	kAVCode_EnduranceCondition = 0x1A,
	kAVCode_LeftAttackCondition = 0x1B,
	kAVCode_RightAttackCondition = 0x1C,
	kAVCode_LeftMobilityCondition = 0x1D,
	kAVCode_RightMobilityCondition = 0x1E,
	kAVCode_BrainCondition = 0x1F,
	kAVCode_Barter = 0x20,
	kAVCode_BigGuns = 0x21,
	kAVCode_EnergyWeapons = 0x22,
	kAVCode_Explosives = 0x23,
	kAVCode_Lockpick = 0x24,
	kAVCode_Medicine = 0x25,
	kAVCode_MeleeWeapons = 0x26,
	kAVCode_Repair = 0x27,
	kAVCode_Science = 0x28,
	kAVCode_Guns = 0x29,
	kAVCode_Sneak = 0x2A,
	kAVCode_Speech = 0x2B,
	kAVCode_Survival = 0x2C,
	kAVCode_Unarmed = 0x2D,
	kAVCode_InventoryWeight = 0x2E,
	kAVCode_Paralysis = 0x2F,
	kAVCode_Invisibility = 0x30,
	kAVCode_Chameleon = 0x31,
	kAVCode_NightEye = 0x32,
	kAVCode_Turbo = 0x33,
	kAVCode_FireResist = 0x34,
	kAVCode_WaterBreathing = 0x35,
	kAVCode_RadiationRads = 0x36,
	kAVCode_BloodyMess = 0x37,
	kAVCode_UnarmedDamage = 0x38,
	kAVCode_Assistance = 0x39,
	kAVCode_ElectricResist = 0x3A,
	kAVCode_FrostResist = 0x3B,
	kAVCode_EnergyResist = 0x3C,
	kAVCode_EmpResist = 0x3D,
	kAVCode_Variable01 = 0x3E,
	kAVCode_Variable02 = 0x3F,
	kAVCode_Variable03 = 0x40,
	kAVCode_Variable04 = 0x41,
	kAVCode_Variable05 = 0x42,
	kAVCode_Variable06 = 0x43,
	kAVCode_Variable07 = 0x44,
	kAVCode_Variable08 = 0x45,
	kAVCode_Variable09 = 0x46,
	kAVCode_Variable10 = 0x47,
	kAVCode_IgnoreCrippledLimbs = 0x48,
	kAVCode_Dehydration = 0x49,
	kAVCode_Hunger = 0x4A,
	kAVCode_SleepDeprivation = 0x4B,
	kAVCode_DamageThreshold = 0x4C,
	kAVCode_Max = 0x4D,
};

struct __declspec(align(4)) VATSQueuedAction
{
	APCostActions actionType;
	UInt8 isSuccess;
	UInt8 byte05;
	UInt8 isMysteriousStrangerVisit;
	UInt8 byte07;
	UInt8 remainingShotsToFire_Burst;
	UInt8 count09;
	UInt8 gap0A[2];
	TESObjectREFR* unkref;
	ActorValueCode bodyPart;
	ActorHitData* hitData;
	float unk18;
	float unk1C;
	float apCost;
	UInt8 isMissFortuneVisit;
	UInt8 gap25[3];
};

enum AnimMoveTypes
{
	kAnimMoveType_Walking = 0x0,
	kAnimMoveType_Sneaking = 0x1,
	kAnimMoveType_Swimming = 0x2,
	kAnimMoveType_Flying = 0x3,
};

enum MovementFlags
{
	kMoveFlag_Forward = 0x1,
	kMoveFlag_Backward = 0x2,
	kMoveFlag_Left = 0x4,
	kMoveFlag_Right = 0x8,
	kMoveFlag_TurnLeft = 0x10,
	kMoveFlag_TurnRight = 0x20,
	kMoveFlag_NonController = 0x40,
	kMoveFlag_Walking = 0x100,
	kMoveFlag_Running = 0x200,
	kMoveFlag_Sneaking = 0x400,
	kMoveFlag_Swimming = 0x800,
	kMoveFlag_Jump = 0x1000,
	kMoveFlag_Flying = 0x2000,
	kMoveFlag_Fall = 0x4000,
	kMoveFlag_Slide = 0x8000,
};

struct SettingT
{
	union Info
	{
		unsigned int uint;
		int i;
		float f;
		char* str;
		bool b;
		UInt16 us;
	};

	virtual ~SettingT();
	Info uValue;
	const char* pKey;

	Info* GetFloatValue()
	{
		return ThisStdCall<Info*>(0x403E20, this);
	}
};

const char* MoveFlagToString(UInt32 flag);

namespace BSCoreMessage
{
	inline const auto Warning = reinterpret_cast<int(__cdecl *)(const char *, ...)>(0x5b5e40);
}

namespace INISettings
{
	inline const auto UseRagdollAnimFootIK = reinterpret_cast<bool(__cdecl *)()>(0x495580);
}

struct TES
{
	bool IsRunningCellTests() const
	{
		return ThisStdCall<bool>(0x451530, this);
	}

	static TES* GetSingleton()
	{
		return reinterpret_cast<TES*>(0x11dea10);
	}
};

class ShadowSceneLight;
class Projectile;
class ImageSpaceModifierInstanceForm;
class ImageSpaceModifierInstanceRB;

class VATSCameraData
{
public:
	enum VATSMode
	{
		VATS_MODE_NONE = 0x0,
		VATS_PLAYBACK = 0x4,
		VATS_MODE_COUNT = 0x5,
	};
	
	BSSimpleList<void> targetsList; // 00
	VATSMode eMode; // 08
	UInt32 unk0C; // 0C
	BGSCameraShot *camShot; // 10
	float flt14; // 14
	float flt18; // 18
	Projectile *projectile; // 1C
	Projectile *unk20; // 20
	TESIdleForm *pMeleeAttack; // 24
	ImageSpaceModifierInstanceForm *isModInstForm; // 28
	ImageSpaceModifierInstanceRB *isModInstRB; // 2C
	UInt32 unk30; // 30
	ShadowSceneLight *spShadowSceneLight; // 34
	UInt8 byte38; // 38
	UInt8 pad39[3]; // 39
	UInt32 numKills; // 3C
	UInt32 unk40; // 40
	UInt32 unk44; // 44

	static VATSCameraData* GetSingleton()
	{
		return reinterpret_cast<VATSCameraData*>(0x11f2250);
	}
};
ASSERT_SIZE(VATSCameraData, 0x48);

namespace GameSettings
{
	namespace General
	{
		inline auto* fAnimationDefaultBlend = reinterpret_cast<SettingT*>(0x11c56fc);
		inline auto* fAnimationMult = reinterpret_cast<SettingT*>(0x11c5724);
	}

	namespace Interface
	{
		inline auto* fMenuModeAnimBlend = reinterpret_cast<SettingT*>(0x11c5740);
	}
}