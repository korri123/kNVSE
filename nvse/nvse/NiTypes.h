#pragma once
#include "Utilities.h"

#if RUNTIME

const UInt32 _NiTMap_Lookup = 0x00853130;

#endif

// 8
struct NiRTTI
{
	const char	* name;
	NiRTTI		* parent;
};

// C
struct NiVector3
{
	float	x, y, z;
};

// 10 - always aligned?
struct NiVector4
{
	float	x, y, z, w;
};

// 10 - always aligned?
struct NiQuaternion
{
	float	x, y, z, w;
};

// 24
struct NiMatrix33
{
	float	data[9];
};

// 34
struct NiTransform
{
	NiMatrix33	rotate;		// 00
	NiVector3	translate;	// 24
	float		scale;		// 30
};

// 10
struct NiSphere
{
	float	x, y, z, radius;
};

// 1C
struct NiFrustum
{
	float	l;			// 00
	float	r;			// 04
	float	t;			// 08
	float	b;			// 0C
	float	n;			// 10
	float	f;			// 14
	UInt8	o;			// 18
	UInt8	pad19[3];	// 19
};

// 10
struct NiViewport
{
	float	l;
	float	r;
	float	t;
	float	b;
};

// C
struct NiColor
{
	float	r;
	float	g;
	float	b;
};

// 10
struct NiColorAlpha
{
	float	r;
	float	g;
	float	b;
	float	a;
};

// 10
struct NiPlane
{
	NiVector3	nrm;
	float		offset;
};

// 10
// NiTArrays are slightly weird: they can be sparse
// this implies that they can only be used with types that can be NULL?
// not sure on the above, but some code only works if this is true
// this can obviously lead to fragmentation, but the accessors don't seem to care
// weird stuff
template <typename T_Data>
struct NiTArray
{
	virtual void* Destroy(UInt32 doFree);

	T_Data*     data;			// 04
	UInt16		capacity;		// 08 - init'd to size of preallocation
	UInt16		firstFreeEntry;	// 0A - index of the first free entry in the block of free entries at the end of the array (or numObjs if full)
	UInt16		numObjs;		// 0C - init'd to 0
	UInt16		growSize;		// 0E - init'd to size of preallocation

	T_Data operator[](UInt32 idx)
	{
		if (idx < firstFreeEntry)
			return data[idx];
		return NULL;
	}

	T_Data Get(UInt32 idx) { return data[idx]; }

	UInt16 Length() { return firstFreeEntry; }
	void AddAtIndex(UInt32 index, T_Data* item);	// no bounds checking
	void SetCapacity(UInt16 newCapacity);	// grow and copy data if needed

	class Iterator
	{
		friend NiTArray;

		T_Data* pData;
		UInt32		count;

	public:
		explicit operator bool() const { return count != 0; }
		void operator++()
		{
			pData++;
			count--;
		}

		T_Data& operator*() const { return *pData; }
		T_Data& operator->() const { return *pData; }
		T_Data& Get() const { return *pData; }

		Iterator(NiTArray& source) : pData(source.data), count(source.firstFreeEntry) {}
	};

	Iterator Begin() { return Iterator(*this); }
};


#if RUNTIME

template <typename T> void NiTArray<T>::AddAtIndex(UInt32 index, T* item)
{
	ThisStdCall(0x00869640, this, index, item);
}

template <typename T> void NiTArray<T>::SetCapacity(UInt16 newCapacity)
{
	ThisStdCall(0x008696E0, this, newCapacity);
}

#endif

// 18
// an NiTArray that can go above 0xFFFF, probably with all the same weirdness
// this implies that they make fragmentable arrays with 0x10000 elements, wtf
template <typename T>
class NiTLargeArray
{
public:
	NiTLargeArray();
	~NiTLargeArray();

	void	** _vtbl;		// 00
	T		* data;			// 04
	UInt32	capacity;		// 08 - init'd to size of preallocation
	UInt32	firstFreeEntry;	// 0C - index of the first free entry in the block of free entries at the end of the array (or numObjs if full)
	UInt32	numObjs;		// 10 - init'd to 0
	UInt32	growSize;		// 14 - init'd to size of preallocation

	T operator[](UInt32 idx) {
		if (idx < firstFreeEntry)
			return data[idx];
		return NULL;
	}

	T Get(UInt32 idx) { return (*this)[idx]; }

	UInt32 Length(void) { return firstFreeEntry; }
};

// 8
template <typename T>
struct NiTSet
{
	T		* data;		// 00
	UInt16	capacity;	// 04
	UInt16	length;		// 06
};

// 10
// this is a NiTPointerMap <UInt32, T_Data>
// todo: generalize key
template <typename T_Data>
class NiTPointerMap // credits to Yvileapsis for the changes
{
public:
	NiTPointerMap();
	virtual ~NiTPointerMap();

	struct Entry
	{
		Entry* next;
		UInt32	key;
		T_Data* data;
	};

	virtual void	Destroy(bool doFree);
	virtual UInt32	CalculateBucket(UInt32 key);
	virtual bool	CompareKey(UInt32 lhs, UInt32 rhs);
	virtual void	FillEntry(Entry* entry, UInt32 key, T_Data data);
	virtual void	FreeKey(Entry* entry);
	virtual Entry* AllocNewEntry();
	virtual void	FreeEntry(Entry* entry);

	T_Data* Lookup(UInt32 key);
	bool			Insert(Entry* nuEntry);

	//	void	** _vtbl;				// 0
	UInt32			m_numBuckets;	// 4
	Entry** m_buckets;		// 8
	UInt32			m_numItems;		// C

	// note: traverses in non-numerical order
	class Iterator
	{
		friend NiTPointerMap;

		NiTPointerMap* m_table;
		Entry* m_entry;
		Entry** m_bucket;

		void FindValid();
		void FindNonEmpty()
		{
			for (Entry** end = &m_table->m_buckets[m_table->m_numBuckets]; m_bucket != end; m_bucket++)
				if (m_entry = *m_bucket) return;
		}

	public:
		Iterator(NiTPointerMap& _table) : m_table(&_table), m_bucket(m_table->m_buckets), m_entry(NULL) { FindNonEmpty(); }

		~Iterator() { }

		T_Data* Get(void);
		UInt32		GetKey(void);
		bool		Next(void);
		bool		Done(void);

		T_Data Get() const { return m_entry->data; }
		//		T_Key Key() const { return m_entry->key; }

		explicit operator bool() const { return m_entry != NULL; }
		void operator++()
		{
			m_entry = m_entry->next;
			if (!m_entry)
			{
				m_bucket++;
				FindNonEmpty();
			}
		}
	};

	Iterator Begin() { return Iterator(*this); }
};

template <typename T_Data>
T_Data * NiTPointerMap <T_Data>::Lookup(UInt32 key)
{
	for(Entry * traverse = m_buckets[key % m_numBuckets]; traverse; traverse = traverse->next)
		if(traverse->key == key)
			return traverse->data;
	
	return NULL;
}

template <typename T_Data>
bool NiTPointerMap<T_Data>::Insert(Entry* nuEntry)
{
	// game code does not appear to care about ordering of entries in buckets
	UInt32 bucket = nuEntry->key % m_numBuckets;
	Entry* prev = NULL;
	for (Entry* cur = m_buckets[bucket]; cur; cur = cur->next) {
		if (cur->key == nuEntry->key) {
			return false;
		}
		else if (!cur->next) {
			prev = cur;
			break;
		}
	}

	if (prev) {
		prev->next = nuEntry;
	}
	else {
		m_buckets[bucket] = nuEntry;
	}

	m_numBuckets++;
	return true;
}

template <typename T_Data>
T_Data * NiTPointerMap <T_Data>::Iterator::Get(void)
{
	if(m_entry)
		return m_entry->data;

	return NULL;
}

template <typename T_Data>
UInt32 NiTPointerMap <T_Data>::Iterator::GetKey(void)
{
	if(m_entry)
		return m_entry->key;

	return 0;
}

template <typename T_Data>
bool NiTPointerMap <T_Data>::Iterator::Next(void)
{
	if(m_entry)
		m_entry = m_entry->next;

	while(!m_entry && (m_bucket < (m_table->m_numBuckets - 1)))
	{
		m_bucket++;

		m_entry = m_table->m_buckets[m_bucket];
	}

	return m_entry != NULL;
}

template <typename T_Data>
bool NiTPointerMap <T_Data>::Iterator::Done(void)
{
	return m_entry == NULL;
}

template <typename T_Data>
void NiTPointerMap <T_Data>::Iterator::FindValid(void)
{
	// validate bucket
	if(m_bucket >= m_table->m_numBuckets) return;

	// get bucket
	m_entry = m_table->m_buckets[m_bucket];

	// find non-empty bucket
	while(!m_entry && (m_bucket < (m_table->m_numBuckets - 1)))
	{
		m_bucket++;

		m_entry = m_table->m_buckets[m_bucket];
	}
}

// 10
// todo: NiTPointerMap should derive from this
// cleaning that up now could cause problems, so it will wait
template <typename T_Key, typename T_Data>
class NiTMapBase
{
public:
	NiTMapBase();
	~NiTMapBase();

	struct Entry
	{
		Entry	* next;	// 000
		T_Key	key;	// 004
		T_Data	data;	// 008
	};

	virtual NiTMapBase<T_Key, T_Data>*	Destructor(bool doFree);						// 000
	virtual UInt32						Hash(T_Key key);								// 001
	virtual void						Equal(T_Key key1, T_Key key2);					// 002
	virtual void						FillEntry(Entry entry, T_Key key, T_Data data);	// 003
	virtual	void						Unk_004(void * arg0);							// 004
	virtual	void						Unk_005(void);									// 005
	virtual	void						Unk_006();										// 006

	//void	** _vtbl;	// 0
	UInt32	numBuckets;	// 4
	Entry	** buckets;	// 8
	UInt32	numItems;	// C

	DEFINE_MEMBER_FN_LONG(NiTMapBase, Lookup, bool, _NiTMap_Lookup, T_Key key, T_Data * dataOut);
};

// 14
template <typename T_Data>
class NiTStringPointerMap : public NiTPointerMap <T_Data>
{
public:
	NiTStringPointerMap();
	~NiTStringPointerMap();

	UInt32	unk010;
};

// not sure how much of this is in NiTListBase and how much is in NiTPointerListBase
// 10
template <typename T>
class NiTListBase
{
public:
	NiTListBase();
	~NiTListBase();

	struct Node
	{
		Node	* next;
		Node	* prev;
		T		* data;
	};

	virtual void	Destructor(void);
	virtual Node *	AllocateNode(void);
	virtual void	FreeNode(Node * node);

//	void	** _vtbl;	// 000
	Node	* start;	// 004
	Node	* end;		// 008
	UInt32	numItems;	// 00C
};

// 10
template <typename T>
class NiTPointerListBase : public NiTListBase <T>
{
public:
	NiTPointerListBase();
	~NiTPointerListBase();
};

// 10
template <typename T>
class NiTPointerList : public NiTPointerListBase <T>
{
public:
	NiTPointerList();
	~NiTPointerList();
};

// 4
template <typename T>
class NiPointer
{
public:
	NiPointer(T *init) : data(init)		{	}

	T	* data;

	const T&	operator *() const { return *data; }
	T&			operator *() { return *data; }

	operator const T*() const { return data; }
	operator T*() { return data; }
};

// 14
template <typename T>
class BSTPersistentList
{
public:
	BSTPersistentList();
	~BSTPersistentList();

	virtual void	Destroy(bool destroy);

//	void	** _vtbl;	// 00
	UInt32	unk04;		// 04
	UInt32	unk08;		// 08
	UInt32	unk0C;		// 0C
	UInt32	unk10;		// 10
};
