#pragma once
#include "GameObjects.h"

namespace Decoding
{
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
		UInt8								byt134;				// 134
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
		BSAnimGroupSequence* animSequence[3];	// 1C4
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
		UInt32								unk22C[2];			// 22C
		float								radsSec234;			// 234
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
		UInt32								unk428;				// 428
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

	inline __declspec(naked) ContChangesEntry* __fastcall Actor_GetAmmoInfo(Actor*, void* _)
	{
		__asm
		{
			mov		eax, [ecx+0x68]
			test	eax, eax
			jz		done
			cmp		dword ptr [eax+0x28], 1
			ja		retnNULL
			mov		eax, [eax+0x118]
			retn
		retnNULL:
			xor		eax, eax
		done:
			retn
		}
	}

}
