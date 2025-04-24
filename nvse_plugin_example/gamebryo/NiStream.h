#pragma once

#include "NiObjects.h"

class NiObjectGroup;
class NiSearchPath;
class NiTexturePalette;
class NiThread;
class BackgroundLoadProcedure;

class NiStream {
public:
	NiStream();

	enum ErrorMessages {
		STREAM_OKAY			= 0,
		FILE_NOT_LOADED		= 1,
		NOT_NIF_FILE		= 2,
		OLDER_VERSION		= 3,
		LATER_VERSION		= 4,
		NO_CREATE_FUNCTION	= 5,
		ENDIAN_MISMATCH		= 6,
	};

	enum ThreadStatus {
		IDLE		= 0,
		LOADING		= 1,
		CANCELLING	= 2,
		PAUSING		= 3,
		PAUSED		= 4,
	};

	virtual				~NiStream();
	virtual bool		Load(NiBinaryStream* pkIstr);
	virtual bool		Load1(char* pcBuffer, int32_t* iBufferSize);
	virtual bool		Load2(const char* pcName);
	virtual bool		Save(NiBinaryStream* pkOstr);
	virtual bool		Save1(char** pcBuffer, int32_t* iBufferSize);
	virtual bool		Save2(const char* pcName);
	virtual void		Unk_07();
	virtual void		RegisterFixedString();
	virtual bool		RegisterSaveObject(NiObject* pkObject);
	virtual void		ChangeObject(NiObject* pkNewObject);
	virtual uint32_t	GetLinkIDFromObject(const NiObject* pkObject);
	virtual void		SaveLinkID(NiObject* pkObject);
	virtual bool		LoadHeader();
	virtual void		SaveHeader();
	virtual bool		LoadStream();
	virtual bool		SaveStream();
	virtual void		RegisterObjects();
	virtual void		LoadTopLevelObjects();
	virtual void		SaveTopLevelObjects();
	virtual bool		LoadObject();
	virtual uint32_t	PreSaveObjectSizeTable();
	virtual bool		SaveObjectSizeTable(uint32_t uiStartOffset);
	virtual bool		LoadObjectSizeTable();

	struct BSStreamHeader {
		uint32_t	m_uiNifBSVersion;
		char		cAuthor[64];
		char		cProcessScript[64];
		char		cExportScript[64];
	};

	BSStreamHeader								m_BSStreamHeader;
	NiTArray<NiObjectGroup*>			m_kGroups;
	uint32_t									m_uiNifFileVersion;
	uint32_t									m_uiNifFileUserDefinedVersion;
	char										m_acFileName[260];
	bool										m_bSaveLittleEndian;
	bool										m_bSourceIsLittleEndian;
	NiSearchPath*								m_pkSearchPath;
	NiTLargeArray<NiPointer<NiObject>>			m_kObjects;
	NiTLargeArray<uint32_t>			m_kObjectSizes;
	NiTLargeArray<NiPointer<NiObject>>			m_kTopObjects;
	NiTLargeArray<NiFixedString>			m_kFixedStrings;
	NiBinaryStream*								m_pkIstr;
	NiBinaryStream*								m_pkOstr;
	NiTSet<UInt32>							m_kLinkIDs;
	uint32_t									m_uiLinkIndex;
	NiTSet<UInt32>							m_kLinkIDBlocks;
	uint32_t									m_uiLinkBlockIndex;
	NiTPointerMap_t<const NiObject*, uint32_t>	m_kRegisterMap;
	NiPointer<NiTexturePalette>					m_spTexturePalette;
	uint16_t									m_usNiAVObjectFlags;
	uint16_t									m_usNiTimeControllerFlags;
	uint16_t									m_usNiPropertyFlags;
	NiStream::ThreadStatus						m_eBackgroundLoadStatus;
	bool										m_bBackgroundLoadExitStatus;
	uint32_t									m_uiLoad;
	uint32_t									m_uiLink;
	uint32_t									m_uiPostLink;
	NiThread*									m_pkThread;
	BackgroundLoadProcedure*					m_pkBGLoadProc;
	uint32_t									m_ePriority;
	uint32_t									m_kAffinity;
	char										m_acLastLoadedRTTI[260];
	NiStream::ErrorMessages						m_uiLastError;
	char										m_acLastErrorMessage[260];
	char										m_acReferencePath[260];
	NiObject*									unk5C4;

	static NiCriticalSection* const pCleanupCriticalSection;

	static NiStream* Create(NiStream* apThis);
	static NiStream* Create();
};
ASSERT_SIZE(NiStream, 0x5C8);