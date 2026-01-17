#pragma once

#include "NiNodes.h"
#include "game_types.h"

class BSMemObject {
};

class BSHash : public BSMemObject {
public:
	enum Process {
		HASH_FILENAME	= 0,
		HASH_DIRECTORY	= 1,
		HASH_NORMAL		= 2
	};

	BSHash() : cLast(0), cLast2(0), cLength(0), cFirst(0), iCRC(0) {}
	BSHash(const char* apString, UInt32 aeProcess)
	{
		ThisStdCall(0xAFD390, this, apString, aeProcess);
	}

	UInt8	cLast;
	UInt8	cLast2;
	UInt8	cLength;
	UInt8	cFirst;
	UInt32	iCRC;
};
ASSERT_SIZE(BSHash, 0x8);

class BSFileEntry : public BSHash {
public:
	UInt32 iSize;
	UInt32 iOffset;
};

ASSERT_SIZE(BSFileEntry, 0x10);

class BSDirectoryEntry : public BSHash {
public:
	UInt32			uiFiles;
	BSFileEntry*	pFiles;
};

ASSERT_SIZE(BSDirectoryEntry, 0x10);

class BSFile : public NiFile {
public:

	virtual bool	Open(int = 0, bool abTextMode = false);
	virtual bool	OpenByFilePointer(FILE* apFile);
	virtual UInt32	GetSize();
	virtual UInt32	ReadString(BSString& arString, UInt32 auiMaxLength);
	virtual UInt32	ReadStringAlt(BSString& arString, UInt32 auiMaxLength);
	virtual UInt32	GetLine(char* apBuffer, UInt32 auiMaxBytes, UInt8 aucMark);
	virtual UInt32	WriteString(BSString& arString, bool abBinary);
	virtual UInt32	WriteStringAlt(BSString& arString, bool abBinary);
	virtual bool	IsReadable();
	virtual UInt32	DoRead(void* apBuffer, UInt32 auiBytes);
	virtual UInt32	DoWrite(const void* apBuffer, UInt32 auiBytes);

	bool		bUseAuxBuffer;
	void*		pAuxBuffer;
	SInt32		iAuxTrueFilePos;
	DWORD		dword3C;
	DWORD		dword40;
	char		cFileName[260];
	UInt32		uiResult;
	UInt32		uiIOSize;
	UInt32		uiTrueFilePos;
	UInt32		uiFileSize;
};

ASSERT_SIZE(BSFile, 0x158);

class Archive;

class ArchiveFile : public BSFile {
public:
	ArchiveFile(const char* apName, Archive* apArchive, UInt32 auiOffset, UInt32 auiSize, UInt32 auiBufferSize);
	~ArchiveFile() override;

	NiPointer<Archive>	spArchive;
	UInt32				aiOffset;
};

ASSERT_SIZE(ArchiveFile, 0x160);

class BSArchiveHeader {
public:
	BSArchiveHeader() {
		uiTag = 0x415342;
		uiVersion = 104;
		uiHeaderSize = sizeof(BSArchiveHeader);
		uiFlags.field = 0;
		uiDirectories = 0;
		uiFiles = 0;
		uiDirectoryNamesLength = 0;
		uiFileNamesLength = 0;
		usArchiveType = 0;
	}

	UInt32		uiTag;
	UInt32		uiVersion;
	UInt32		uiHeaderSize;
	Bitfield32	uiFlags;
	UInt32		uiDirectories;
	UInt32		uiFiles;
	UInt32		uiDirectoryNamesLength;
	UInt32		uiFileNamesLength;
	UInt16		usArchiveType;

	enum Flags {
		DIRSTRINGS						= 1 << 0,
		FILESTRINGS						= 1 << 1,
		COMPRESSED						= 1 << 2,
		RETAIN_DIRECTORY_NAMES			= 1 << 3,
		RETAIN_FILE_NAMES				= 1 << 4,
		RETAIN_FILE_NAME_OFFSETS		= 1 << 5,
		XBOX_ARCHIVE					= 1 << 6,
		RETAIN_STRINGS_DURING_STARTUP	= 1 << 7,
		EMBEDDED_FILE_NAMES				= 1 << 8,
		XBOX_COMPRESSED					= 1 << 9,
		NEXT_FLAG						= 1 << 10,
	};
	
	bool IsCompressed() const { return uiFlags.IsSet(COMPRESSED); }
	bool IsXBoxArchive() const { return uiFlags.IsSet(XBOX_ARCHIVE); }
	bool IsXBoxCompressed() const { return uiFlags.IsSet(XBOX_COMPRESSED); }
	bool HasEmbeddedFileNames() const { return uiFlags.IsSet(EMBEDDED_FILE_NAMES); }
	bool HasDirectoryStrings() const { return uiFlags.IsSet(DIRSTRINGS); }
	bool HasFileStrings() const { return uiFlags.IsSet(FILESTRINGS); }
	bool RetainDirectoryNames() const { return uiFlags.IsSet(RETAIN_DIRECTORY_NAMES); }
	bool RetainFileNames() const { return uiFlags.IsSet(RETAIN_FILE_NAMES); }
	bool RetainFileOffsets() const { return uiFlags.IsSet(RETAIN_FILE_NAME_OFFSETS); }
	bool RetainStringsDuringStartup() const { return uiFlags.IsSet(RETAIN_STRINGS_DURING_STARTUP); }
	bool HasNextFlag() const { return uiFlags.IsSet(NEXT_FLAG); }
};

ASSERT_SIZE(BSArchiveHeader, 0x24);

class BSArchive : public BSArchiveHeader {
public:
	BSArchive() { pDirectories = nullptr; }

	BSDirectoryEntry* pDirectories;
};

ASSERT_SIZE(BSArchive, 0x28);

enum ARCHIVE_TYPE {
	ARCHIVE_TYPE_ALL		= 0xFFFF,
	ARCHIVE_TYPE_MESHES		= 0x1,
	ARCHIVE_TYPE_TEXTURES	= 0x2,
	ARCHIVE_TYPE_MENUS		= 0x4,
	ARCHIVE_TYPE_SOUNDS		= 0x8,
	ARCHIVE_TYPE_VOICES		= 0x10,
	ARCHIVE_TYPE_SHADERS	= 0x20,
	ARCHIVE_TYPE_TREES		= 0x40,
	ARCHIVE_TYPE_FONTS		= 0x80,
	ARCHIVE_TYPE_MISC		= 0x100,
	ARCHIVE_TYPE_COUNT		= 9,
};


enum ARCHIVE_TYPE_INDEX {
	ARCHIVE_TYPE_INDEX_MESHES	= 0,
	ARCHIVE_TYPE_INDEX_TEXTURES = 1,
	ARCHIVE_TYPE_INDEX_MENUS	= 2,
	ARCHIVE_TYPE_INDEX_SOUNDS	= 3,
	ARCHIVE_TYPE_INDEX_VOICES	= 4,
	ARCHIVE_TYPE_INDEX_SHADERS	= 5,
	ARCHIVE_TYPE_INDEX_TREES	= 6,
	ARCHIVE_TYPE_INDEX_FONTS	= 7,
	ARCHIVE_TYPE_INDEX_MISC		= 8,
	ARCHIVE_TYPE_INDEX_COUNT	= 9,
};

struct ArchiveTypeExtension {
	char			cExtension[4];
	ARCHIVE_TYPE	eArchiveType;
};

class Archive : public BSFile, public NiRefObject {
public:
	Archive(const char* apArchiveName, UInt32 auiBufferSize, bool abSecondaryArchive, bool abInvalidateOtherArchives);

	BSArchive			kArchive;
	time_t				ulArchiveFileTime;
	UInt32				uiFileNameArrayOffset;
	UInt32				uiLastDirectoryIndex;
	UInt32				uiLastFileIndex;
	CRITICAL_SECTION	kArchiveCriticalSection;
	Bitfield8			ucArchiveFlags;
	char*				pDirectoryStringArray;
	UInt32*				pDirectoryStringOffsets;
	char*				pFileNameStringArray;
	UInt32**			pFileNameStringOffsets;
	UInt32				uiID;
	UInt32				unk6C;

	enum Flags {
		DISABLED				= 1 << 0,
		PRIMARY					= 1 << 2,
		SECONDARY				= 1 << 3,
		HAS_DIRECTORY_STRINGS	= 1 << 4,
		HAS_FILE_STRINGS		= 1 << 5,
	};

	static UInt32* const uiCount;
	
	void DeleteDirectoryStringArray();
	void DeleteFilenameStringArray(bool abKeepOffsets);
	void DeleteFileData();
	
	bool GetLoadStrings() const;
	bool FindNewerAndInvalidate() const;
	bool FindFile(const BSHash& arDirectoryHash, const BSHash& arFileNameHash, UInt32& arDirectoryID, UInt32& arFileID, const char* apFileName) const;
	bool FindDirectory(const BSHash& arDirectoryHash, UInt32& aruiDirectoryIndex, const char* apFileName) const;
	bool FindFileInDirectory(UInt32 auiDirectoryIndex, const BSHash& arFileNameHash, UInt32& aruiFileIndex, const char* apFileName, bool abSkipSecondaryArchiveCheck) const;
	BSFileEntry* GetFileEntryForFile(BSHash& arDirectoryHash, BSHash& arFileNameHash, const char* apFileName);

	ArchiveFile* GetFile(UInt32 auiDirectoryIndex, UInt32 auiFileIndex, UInt32 auiBufferSize, const char* apFileName);

	SInt32 GetDirectoryIndexForFileEntry(BSFileEntry* apFileEntry);
	UInt32 GetFileIndexForFileEntryFromDirectory(UInt32 auiDirectory, BSFileEntry* apFileEntry) const;
	const char* GetArchiveName() const;
	const char* GetFileString(UInt32 auiDirectoryIndex, UInt32 auiFileIndex) const;
	const char* GetFileNameForFileEntry(BSFileEntry* apFileEntry);

	void LoadFileNameStrings();
};

ASSERT_SIZE(Archive, 0x1D0);

NiSmartPointer(Archive);

using ArchiveList = BSSimpleList<Archive>;

class ArchiveManager {
public:
	static NiCriticalSection* const pCriticalSection;
	static ArchiveList* GetArchiveList() { return *reinterpret_cast<ArchiveList**>(0x11F8160); }
	static void SetArchiveList(ArchiveList* apArchiveList) { *reinterpret_cast<ArchiveList**>(0x11F8160) = apArchiveList; }
	static ArchiveTypeExtension* const pArchiveTypeExtensionsMap;

	static UInt32* const uiArchiveFileBufferSizeS;
	static bool* const bInvalidateOlderFiles;

	static void AddToFileList(BSSimpleList<const char*>* apFileNameList, const char* apSearchName, const char* apBaseFilename, ARCHIVE_TYPE aeArchiveType);
	static BSHash** AddArchiveToFileList(Archive* apArchive, UInt32 aiNumFiles, BSHash** apHashArray, BSHash& arDirectoryHash, BSHash& arSearchHash, BSSimpleList<const char*>* apFileNameList, const char* apBaseFilename);

	static void LoadInvalidationFile(const char* apFileName);
	static bool InvalidateFile(const char* apFileName);
	static void InvalidateFileInArchives(BSHash& arDirectoryHash, BSHash& arFileHash, UInt16 aiArchiveTypes);

	static Archive* GetMasterArchive(ARCHIVE_TYPE_INDEX aeTypeIndex);
	static Archive* GetArchiveForFile(const char* apFileName, ARCHIVE_TYPE aiArchiveType);
	static Archive* GetArchiveForFileEntry(BSFileEntry* apFileEntry, ARCHIVE_TYPE aiArchiveType);

	static bool OpenMasterArchives();
	static Archive* OpenArchive(const char* apArchiveName, UInt16 aiForceArchiveType, bool abInvalidateOtherArchives);

	static bool FindFile(const char* apFileName, ARCHIVE_TYPE aiArchiveType);
	 
	static ArchiveFile* GetFile(const char* apFileName, UInt32 aiBufferSize, ARCHIVE_TYPE aeArchiveType);
	static ArchiveFile* GetArchiveFile(const char* apFileName, UInt32 aiBufferSize, ARCHIVE_TYPE aeArchiveType);
	static BSFileEntry* GetFileEntryForFile(ARCHIVE_TYPE_INDEX aeArchiveTypeIndex, BSHash& arDirectoryHash, BSHash& arFileNameHash, const char* apFileName);

	static bool WildCardMatch(char* apSearchName, BSHash* apHash);

	static ARCHIVE_TYPE GetArchiveTypeFromFileName(const char* apFileName);
	static ARCHIVE_TYPE GetArchiveTypeFromFileExtension(const char* apExtension);

	static bool GetFileNameForSmallestFileInDirectory(const char* apDirectory, char* apFileName, ARCHIVE_TYPE aeArchiveType);
	static bool GetRandomFileNameForDirectory(const char* apDirectory, char* apFileName, ARCHIVE_TYPE aeArchiveType);

	static const char* TrimFileName(const char* apFileName);
	
	static bool RemoveArchive(Archive* apArchive);

	static ScopedList<char> GetDirectoryPaths(const char* searchName, const char* baseName, ARCHIVE_TYPE aeArchiveType)
	{
		ScopedList<char> result;
		CdeclCall(0xAF6BA0, &result, searchName, baseName, aeArchiveType);
		return result;
	}

	static ScopedList<char> GetDirectoryAnimPaths(std::string_view path)
	{
		const std::string searchPath = FormatString(R"(%s*.kf)", path.data());
		return GetDirectoryPaths(searchPath.c_str(), searchPath.c_str(), ARCHIVE_TYPE_MESHES);
	}

	static void Lock() {
		pCriticalSection->Lock();
	}

	static void Unlock() {
		pCriticalSection->Unlock();
	}
};
