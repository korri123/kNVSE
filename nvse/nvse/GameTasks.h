#pragma once

#include "GameTypes.h"
#include "NiTypes.h"
//#include "NiNodes.h"

/*******************************************************
*
* BSTask
*	IOTask
*		QueuedFile
*			QueuedFileEntry
*				QueuedModel
*					QueuedDistantLOD
*					QueuedTreeModel
*				QueuedTexture
*					QueuedTreeBillboard
*				QueuedKF
*					QueuedAnimIdle
*				DistantLODLoaderTask
*				TerrainLODQuadLoadTask
*				SkyTask
*				LipTask
*				GrassLoadTask
*			QueuedReference
*				QueuedTree
*				QueuedActor
*					QueuedCharacter
*						QueuedPlayer
*					QueuedCreature
*			QueuedHead
*			QueuedHelmet
*			QueuedMagicItem
*		AttachDistant3DTask
*		ExteriorCellLoaderTask
*
* NiTArray< NiPointer<QueuedFile> >
*	QueuedChildren
*
*********************************************************/

class TESObjectREFR;
class TESModel;
class QueuedCharacter;
class TESNPC;
class BSFaceGenNiNode;
class BackgroundCloneThread;
class TESAnimGroup;
class BSFaceGenModel;
class QueuedChildren;
class QueuedReference;
class Character;
class AttachDistant3DTask;
class BSTaskManagerThread;
class AnimIdle;
struct AnimData;
class QueuedReplacementKFList;
class QueuedHelmet;
class BSFileEntry;
class LoadedFile;
template <typename T_Data> class LockFreeQueue;
class LockFreePriorityQueue;

class ModelLoader;

class NiNode;
class NiControllerSequence;
class NiRefObject;
class RefNiObject;

class BSAnimGroupSequence;
struct BSAData;

class RefNiRefObject
{
public:
	NiRefObject*	niRefObject;
	
	//	RefNiRefObject SetNiRefObject(NiRefObject* niRefObject);
};

// 18
class BSTask
{
public:
	virtual void Destroy(bool doFree);
	virtual void Run(void) = 0;
	virtual void Unk_02(void) = 0;
	virtual void Unk_03(UInt32 arg0, UInt32 arg1);						// doesNothing
	virtual bool GetDebugDescription(char* outDesc, UInt32 * arg1) = 0;	// return 0

	// void		** vtbl
	
	BSTask	* unk004;	// uninitialized OBSE, not confirmed, NiRefObject
	UInt32	refCounter; // Counter: NiRefObject RefCounter
	UInt32	unk00C;		// Semaphore/Status
	UInt32	unk010;		// Paired : 10 and 14 as a 64 bit integer, it could be very large flag bits or an integer
	UInt32	unk014;

	static UInt32*	GetCounterSingleton();
};

// 18
class IOTask : public BSTask
{
public:
	virtual void Unk_05(void);			// doesNothing
	virtual void Unk_06(void);				
	virtual void Unk_07(UInt32 arg0);	// most (all?) implementations appear to call IOManager::00C3DF40(this, arg0) eventually. It updates the bits 23/32 of the giant bit flag possibly.

	IOTask();
	~IOTask();

};

class QueuedFile;

// 014
class QueuedChildren : public BSSimpleArray<NiPointer<QueuedFile>>
{
	UInt32	counter;
};

// 028
class QueuedFile : public IOTask
{
public:
	QueuedFile();
	~QueuedFile();

	//Unk_01:	doesNothing
	//Unk_02:	virtual void Call_Unk_0A(void);
	//Unk_03:	implemented
	//Unk_07:	recursivly calls Unk_07(arg_0) on all non null children before calling its parent.
	virtual void Unk_08(void);				// doesNothing
	virtual void Unk_09(UInt32 arg0);
	virtual void Unk_0A(void);				

	// size?
	struct FileEntry {
		UInt32		unk00;
		UInt32		unk04;
		UInt32		size;
		UInt32		offset;
	};

	BSTask			* unk018;			// 018 init to tlsData:2B4 not confirmed OBSE for QueuedModel, seen QueuedReference (ref to which model will be attached)
	QueuedReference	* queuedRef;		// 01C could be last QueuedRef
	QueuedChildren	* queuedChildren;	// 020
	UInt32			* unk024;			// 024	struct, 004 is a base, 008 is a length
};

// 40
class QueuedReference : public QueuedFile
{
public:
	QueuedReference();
	~QueuedReference();

	virtual void Unk_0B(void);			// Initialize validBip01Names (and cretae the 3D model ?)
	virtual void Unk_0C(void);
	virtual void Unk_0D(NiNode* arg0);
	virtual bool Unk_0E(void);
	virtual void Unk_0F(void);
	virtual void Unk_10(void);			// doesNothing

	TESObjectREFR	* refr;				// 028 
	RefNiRefObject	* unk02C;			// OBSE QueuedChildren	* queuedChildren;	// 02C
	NiRefObject		* unk030;			// 030 
	NiRefObject		* unk034;			// 034 
	RefNiRefObject	* unk038;			// 038 
	UInt32			unk03C;				// 03C uninitialized
};

// 40
class QueuedActor : public QueuedReference
{
public:
	QueuedActor();
	~QueuedActor();
};

// 40
class QueuedCreature : public QueuedActor
{
public:
	QueuedCreature();
	~QueuedCreature();
};

// 48
class QueuedCharacter : public QueuedActor
{
public:
	QueuedCharacter();
	~QueuedCharacter();

	typedef RefNiRefObject RefQueuedHead;

	RefQueuedHead	* refQueuedHead;	// 040
	RefNiRefObject	* unk044;	// 044

};

// 48
class QueuedPlayer : public QueuedCharacter
{
public:
	QueuedPlayer();
	~QueuedPlayer();
};

// 030
class QueuedFileEntry : public QueuedFile
{
public:
	QueuedFileEntry();
	~QueuedFileEntry();

	virtual bool Unk_0B(void) = 0;

	char	* name;		// 028
	BSAData	* bsaData;	// 02C
};

class Model // NiObject
{
	const char	* path;		// 004
	UInt32		counter;	// 008
	NiNode		* ninode;	// 00C
};

// 44
class QueuedModel : public QueuedFileEntry
{
public:
	QueuedModel();
	~QueuedModel();

	virtual void Unk_0C(UInt32 arg0);

	Model		* model;		// 030
	TESModel	* tesModel;		// 034
	UInt32		baseFormClass;	// 038	table at offset : 0x045C708. Pickable, invisible, unpickable ? 6 is VisibleWhenDistant or internal
	UInt8		flags;			// 03C	bit 0 and bit 1 init'd by parms, bit 2 set after textureSwap, bit 3 is model set, bit 4 is file found.
	UInt8		pad03D[3];		// 03D
	float		flt040;			// 040

	// There are at least 3 Create/Initiator
};

// 30
class QueuedTexture : public QueuedFileEntry
{
public:
	QueuedTexture();
	~QueuedTexture();

	void	* niTexture;	// 030
};

// 014
class KFModel
{
public:
	const char			* path;					// 000
	BSAnimGroupSequence	* controllerSequence;	// 004
	TESAnimGroup		* animGroup;			// 008
	UInt32				unk0C;					// 00C
	UInt32				refCount;				// 010
};

// 30
class QueuedKF : public QueuedFileEntry
{
public:
	QueuedKF();
	~QueuedKF();

	KFModel		* kf;		// 030
	UInt8		unk034;		// 034
	UInt8		pad035[3];	// 035
};

// 040
class QueuedAnimIdle : public QueuedKF
{
public:
	QueuedAnimIdle();
	~QueuedAnimIdle();

	ModelLoader	* modelLoader;	// 038	Init"d by arg2
	RefNiObject	* unk03C;		// 03C	Init"d by arg1
};

// 38
class QueuedHead : public QueuedFile
{
public:
	QueuedHead();
	~QueuedHead();

	TESNPC			* npc;				// 028
	BSFaceGenNiNode * faceNiNodes[2];	// 02C OBSE presumably male and female
	UInt32			unk034;				// 034
};

/*
// 38
class QueuedHelmet : public QueuedFile
{
public:
	QueuedHelmet();
	~QueuedHelmet();

	QueuedCharacter		* queuedCharacter;		// 18
	QueuedChildren		* queuedChildren;		// 1C
	void				* unk20;				// 20
	QueuedModel			* queuedModel;			// 24
	BSFaceGenModel		* faceGenModel;			// 28
	NiNode				* niNode;				// 2C
	Character			* character;			// 30
	UInt32				unk34;					// 34
};
*/

// O4 assumed
class InterfacedClass {
public:
	virtual ~InterfacedClass(); // 00
	virtual void AllocateTLSValue(void);		// not implemented
};

// 40
template<typename T_Key, class T_Data>
class LockFreeMap: public InterfacedClass
{
public:
	// 0C
	struct Data004
	{
		UInt32	unk000;		// 00
		UInt32	unk004;		// 04
		UInt32	*unk008;	// 08
	};

	// 24
	struct TLSValue
	{
		LockFreeMap	*map;				// 00
		UInt32		mapData004Unk000;	// 04
		UInt32		mapData004Unk008;	// 08
		UInt32		*mapData004Unk00C;	// 0C	stores first DWord of bucket during lookup, next pointer is data, next flags bit 0 is status ok/found
		UInt32		unk010;				// 10	stores bucketOffset during lookup
		UInt32		*unk014;			// 14	stores pointer to bucket during lookup
		UInt32		*unk018;			// 18
		UInt32		unk01C;				// 1C
		UInt32		unk020;				// 20
	};

	// 10
	struct Data014
	{
		// 08
		struct Data008
		{
			UInt32		threadID;	// 00 threadID
			TLSValue	*tlsValue;	// 04 lpTlsValue obtained from AllocateTLSValue of LockFreeMap
		};

		UInt32	alloc;			// Init'd to arg0, count of array at 008
		UInt32	tlsDataIndex;	// Init'd by TlsAlloc
		Data008	**dat008;		// array of pair of DWord
		UInt32	count;		// Init'd to 0
	};	// most likely an array or a map
	

	virtual bool Get(T_Key key, T_Data *destination); // 02
	virtual bool SetAt(UInt32 auiPos, T_Key key, T_Data& val, bool); // 03
	virtual bool SetAtAlt(T_Key key, T_Data& val, bool); // 04
	virtual bool RemoveAt(T_Key key); // 05
	virtual void Unk_06(void); // 06
	virtual void Unk_07(void); // 07
	virtual void Unk_08(void); // 08
	virtual UInt32 GetBucketForKey(T_Key key); // 09
	virtual void DeleteKey(T_Key key); // 0A
	virtual T_Key GenerateKey(T_Key src); // 0B
	virtual T_Key CopyKey(T_Key src, T_Key& val); // 0C
	virtual bool IsKeyGreaterOrEqual(T_Key lkey, T_Key rkey);// 0D
	virtual bool IsKeyEqual(T_Key lkey, T_Key rkey); // 0E
	virtual UInt32 IncrementCount(); // 0F
	virtual UInt32 DecrementCount(); // 10
	virtual UInt32 GetCount() const; // 11

	Data004	**dat004;		// 04 array of arg0 12 bytes elements (uninitialized)
	UInt32	bucketCount;	// 08 Init'd to arg1, count of DWord to allocate in array at 000C
	UInt32	**buckets;		// 0C array of arg1 DWord elements
	UInt32	unk010;			// 10 Init'd to arg2
	Data014	*dat014;		// 14 Init'd to a 16 bytes structure
	UInt32	count;			// 18
	UInt32	unk01C;			// 1C
	UInt32	kLock[8];		// 20 BSSpinLock
};
static_assert(sizeof(LockFreeMap<int, int>) == 0x40);

template<class _C>
class LockFreeStringMap: public LockFreeMap<char const *, _C> {};

template<class _C>
class LockFreeCaseInsensitiveStringMap: public LockFreeStringMap<_C>
{
public:
	struct InternalIterator
	{
		InternalIterator()
		{
			ThisStdCall(0x449960, this);
		}
		virtual ~InternalIterator(){}
		UInt32 uiCount;
		const char* key;
		UInt8 ucFlags;
		char cBuffer[256];

		enum StateFlags
		{
			STATE_UNK_1 = 1,
			STATE_FINISHED = 2,
		};

		bool IsFinished()
		{
			return (ucFlags & STATE_FINISHED) != 0;
		}
	};
	static_assert(sizeof(InternalIterator) == 0x110);
public:
	class Iterator
	{
		LockFreeCaseInsensitiveStringMap* mapPtr{};
		InternalIterator iter;
		UInt32 index = 0;
		
		bool GetNextInternalIter()
		{
			return ThisStdCall<bool>(0x44C640, mapPtr, &iter, &pair.first, &pair.second, true);
		}
		
	public:
		std::pair<const char*, _C> pair;
		
		Iterator(LockFreeCaseInsensitiveStringMap* mapPtr) : mapPtr(mapPtr)
		{
			++*this;
		}

		Iterator(LockFreeCaseInsensitiveStringMap* mapPtr, UInt32 index) : mapPtr(mapPtr), index(index){}

		Iterator& operator++()
		{
			GetNextInternalIter();
			if (!iter.IsFinished())
				++index;
			else
			{
				index = -1;
				pair.first = nullptr;
				pair.second = nullptr;
			}
			return *this;
		}

		operator bool() const
		{
			return !iter.IsFinished();
		}

		bool operator==(const Iterator& other) const
		{
			return index == other.index;
		}

		std::pair<const char*, _C>& operator*()
		{
			return pair;
		}
	};

	Iterator begin()
	{
		return Iterator(this);
	}
	
	Iterator end()
	{
		return Iterator(this, -1);
	}

};

// C
struct BSSemaphore
{
	UInt32 uiCount;
	HANDLE hSemaphore;
	UInt32 uiMaxCount;
};

// 30
class BSTaskThread
{
public:
	virtual ~BSTaskThread();

	HANDLE		hThread;			// 04
	UInt32		uiThreadID;			// 08
	BSSemaphore	kTaskSemaphore;		// 0C
	BSSemaphore	kPauseSemaphore;	// 18
	UInt32		kHavokData[3];		// 24 BSHavokThreadData
};
static_assert(sizeof(BSTaskThread) == 0x30);

class BSTaskManager;

// 34
class BSTaskManagerThread : public BSTaskThread
{
public:
	BSTaskManager* pOwner;	// 30
};
static_assert(sizeof(BSTaskManagerThread) == 0x34);

// 3C
class BackgroundCloneThread : public BSTaskThread
{
public:
	bool										bExit;					// 30
	UInt32										iRunningCount;			// 34
	LockFreeQueue<NiPointer<QueuedReference>>*	pCloneReferencesQueue;	// 38
};
static_assert(sizeof(BackgroundCloneThread) == 0x3C);

// 5C
class BSTaskManager : public LockFreeMap<SInt64, NiPointer<BSTask>>
{
public:
	bool					bExit;				// 40
	UInt32					uiSleepingThreads;	// 44
	UInt32					uiSleepLock;		// 48
	UInt32					uiThreadCount;		// 4C
	BSTaskManagerThread**	ppThreads;			// 50 uiThreadCount of them
	UInt32**				pBucketCounts;		// 54
	UInt32					uiBucketCountTotal;	// 58
};
static_assert(sizeof(BSTaskManager) == 0x5C);

// A0
class IOManager : public BSTaskManager
{
public:
	UInt32					unk05C;							// 5C
	UInt32					uiTaskManagerThreadID;			// 60
	LockFreePriorityQueue*	pPostProcessQueue;				// 64
	UInt32					eLoadingQueuedPriority;			// 68
	UInt32					(__thiscall* pGetExternalCountCallback)(IOManager*);			// 6C
	void					(__thiscall* pUpdateQueueCallback)(IOManager*);					// 70
	bool					(__thiscall* pUpdateSecondaryQueueCallback)(IOManager*);		// 74
	void					(__thiscall* pLoadQueuedPriorityCallback)(IOManager*, float);	// 78
	void					(__thiscall* pPauseCallback)(IOManager*);						// 7C
	void					(__thiscall* pResumeCallback)(IOManager*);						// 80
	UInt32					unk084[7];						// 84

	static IOManager* GetSingleton();
};
static_assert(sizeof(IOManager) == 0xA0);

// 30
class ModelLoader
{
public:
	ModelLoader();
	~ModelLoader();

	LockFreeCaseInsensitiveStringMap<Model*>*								pModelMap;					// 00
	LockFreeCaseInsensitiveStringMap<KFModel*>*								pKFModelMap;				// 04
	LockFreeMap<TESObjectREFR*, NiPointer<QueuedReference>>*				pQueuedReferences;			// 08
	LockFreeMap<TESObjectREFR*, NiPointer<QueuedReference>>*				pDelayedQueue;				// 0C
	LockFreeMap<AnimIdle*, NiPointer<QueuedAnimIdle>>*						pQueuedAnimIdleMap;			// 10
	LockFreeMap<AnimData*, NiPointer<QueuedReplacementKFList>>*				pQueuedReplacementKFListMap;	// 14
	LockFreeMap<TESObjectREFR*, NiPointer<QueuedHelmet>>*					pQueuedHelmetMap;			// 18
	LockFreeQueue<NiPointer<AttachDistant3DTask>>*							pAttachDistant3DTaskQueue;	// 1C
	LockFreeMap<BSFileEntry*, NiPointer<QueuedTexture>>*					pQueuedTextureMap;			// 20
	LockFreeCaseInsensitiveStringMap<LoadedFile*>*							pLoadingFileMap;			// 24
	BackgroundCloneThread*													pBackgroundCloneThread;		// 28
	bool																	bHasDelayedFree;			// 2C

	static ModelLoader* GetSingleton();

	void QueueReference(TESObjectREFR* refr, UInt32 arg1, bool ifInMainThread);

	static KFModel* LoadKFModel(const char* path)
	{
		const auto* pThis = GetSingleton();
		return ThisStdCall<KFModel*>(0x4471C0, pThis, path);
	}
};
static_assert(sizeof(ModelLoader) == 0x30);