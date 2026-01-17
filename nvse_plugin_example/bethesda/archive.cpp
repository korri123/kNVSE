#include "bethesda_types.h"

constexpr UInt32 ENTRY_SIZE = sizeof(BSDirectoryEntry);

UInt32* const Archive::uiCount = reinterpret_cast<UInt32*>(0x11F81D8);

NiCriticalSection* const ArchiveManager::pCriticalSection = reinterpret_cast<NiCriticalSection*>(0x11F8170);
ArchiveTypeExtension* const ArchiveManager::pArchiveTypeExtensionsMap = reinterpret_cast<ArchiveTypeExtension*>(0x11ACE08);
UInt32* const ArchiveManager::uiArchiveFileBufferSizeS = reinterpret_cast<UInt32*>(0x11F8164);
bool* const ArchiveManager::bInvalidateOlderFiles = reinterpret_cast<bool*>(0x11ACDF9);

// 0xAF9230
void Archive::DeleteDirectoryStringArray() {
    ThisStdCall(0xAF9230, this);
}

// 0xAF92F0
void Archive::DeleteFilenameStringArray(bool abKeepOffsets) {
    ThisStdCall(0xAF92F0, this, abKeepOffsets);
}

// 0xAF9410
void Archive::DeleteFileData() {
    ThisStdCall(0xAF9410, this);
}

// 0xAFD020
bool Archive::GetLoadStrings() const
{
    return ThisStdCall<bool>(0xAFD020, this);
}

// 0xAFAD00
bool Archive::FindNewerAndInvalidate() const
{
    return ThisStdCall(0xAFAD00, this);
}

// 0xAF9BF0
bool Archive::FindFile(const BSHash& arDirectoryHash, const BSHash& arFileNameHash, UInt32& arDirectoryID, UInt32& arFileID, const char* apFileName) const
{
    return FindDirectory(arDirectoryHash, arDirectoryID, apFileName) && FindFileInDirectory(arDirectoryID, arFileNameHash, arFileID, apFileName, false);
}

// 0xAF9C50
bool Archive::FindDirectory(const BSHash& arDirectoryHash, UInt32& aruiDirectoryIndex, const char* apFileName) const
{
    return ThisStdCall<bool>(0xAF9C50, this, &arDirectoryHash, &aruiDirectoryIndex, apFileName);
}

// 0xAFA120
bool Archive::FindFileInDirectory(UInt32 auiDirectoryIndex, const BSHash& arFileNameHash, UInt32& aruiFileIndex, const char* apFileName, bool abSkipSecondaryArchiveCheck) const
{
    return ThisStdCall<bool>(0xAFA120, this, auiDirectoryIndex, &arFileNameHash, &aruiFileIndex, apFileName, abSkipSecondaryArchiveCheck);
}

// 0xAFA6E0
BSFileEntry* Archive::GetFileEntryForFile(BSHash& arDirectoryHash, BSHash& arFileNameHash, const char* apFileName) {
    return ThisStdCall<BSFileEntry*>(0xAFA6E0, this, &arDirectoryHash, &arFileNameHash, apFileName);
}

// 0xAFA550
ArchiveFile* Archive::GetFile(UInt32 auiDirectoryIndex, UInt32 auiFileIndex, UInt32 auiBufferSize, const char* apFileName) {
    return ThisStdCall<ArchiveFile*>(0xAFA550, this, auiDirectoryIndex, auiFileIndex, auiBufferSize, apFileName);
}

// 0xAF9AD0
SInt32 Archive::GetDirectoryIndexForFileEntry(BSFileEntry* apFileEntry) {
    return ThisStdCall(0xAF9AD0, this, apFileEntry);
}

// 0xAF9B60
UInt32 Archive::GetFileIndexForFileEntryFromDirectory(UInt32 auiDirectory, BSFileEntry* apFileEntry) const {
    return ThisStdCall<UInt32>(0xAF9B60, this, auiDirectory, apFileEntry);
}

const char* Archive::GetArchiveName() const {
    return &cFileName[5];
}

// 0xAF96D0
const char* Archive::GetFileString(UInt32 auiDirectoryIndex, UInt32 auiFileIndex) const
{
    return ThisStdCall<const char*>(0xAF96D0, this, auiDirectoryIndex, auiFileIndex);
}

// 0xAF9BA0
const char* Archive::GetFileNameForFileEntry(BSFileEntry* apFileEntry) {
    return ThisStdCall<const char*>(0xAF9BA0, this, apFileEntry);
}

// 0xAF8F10
void Archive::LoadFileNameStrings() {
	ThisStdCall(0xAF8F10, this);
}

// 0xAF6BA0
void ArchiveManager::AddToFileList(BSSimpleList<const char*>* apFileNameList, const char* apSearchName, const char* apBaseFilename, ARCHIVE_TYPE aeArchiveType) {
    CdeclCall(0xAF6BA0, apFileNameList, apSearchName, apBaseFilename, aeArchiveType);
}

// 0xAF7060
BSHash** ArchiveManager::AddArchiveToFileList(Archive* apArchive, UInt32 aiNumFiles, BSHash** apHashArray, BSHash& arDirectoryHash, BSHash& arSearchHash, BSSimpleList<const char*>* apFileNameList, const char* apBaseFilename) {
    return CdeclCall<BSHash**>(0xAF7060, apArchive, aiNumFiles, apHashArray, &arDirectoryHash, &arSearchHash, apFileNameList, apBaseFilename);
}

// 0xAF5AB0
void ArchiveManager::LoadInvalidationFile(const char* apFileName) {
    CdeclCall(0xAF5AB0, apFileName);
}

// 0xAF5E40
bool ArchiveManager::InvalidateFile(const char* apFileName) {
    return CdeclCall<bool>(0xAF5E40, apFileName);
}

// 0xAF7C40
void ArchiveManager::InvalidateFileInArchives(BSHash& arDirectoryHash, BSHash& arFileHash, UInt16 aiArchiveTypes) {
    CdeclCall(0xAF7C40, &arDirectoryHash, &arFileHash, aiArchiveTypes);
}

// 0x4898F0
Archive* ArchiveManager::GetMasterArchive(ARCHIVE_TYPE_INDEX aeTypeIndex) {
    return CdeclCall<Archive*>(0x4898F0, aeTypeIndex);
}

// 0xAF5FA0
ArchiveFile* ArchiveManager::GetArchiveFile(const char* apFileName, UInt32 aiBufferSize, ARCHIVE_TYPE aeArchiveType) {
    return CdeclCall<ArchiveFile*>(0xAF5FA0, apFileName, aiBufferSize, aeArchiveType);
}

// 0xAF6160
Archive* ArchiveManager::GetArchiveForFile(const char* apFileName, ARCHIVE_TYPE aiArchiveType) {
    return CdeclCall<Archive*>(0xAF6160, apFileName, aiArchiveType);
}

// 0xAF6910
Archive* ArchiveManager::GetArchiveForFileEntry(BSFileEntry* apFileEntry, ARCHIVE_TYPE aiArchiveType) {
    return CdeclCall<Archive*>(0xAF6910, apFileEntry, aiArchiveType);
}

bool ArchiveManager::OpenMasterArchives() {
    return StdCall<bool>(0xAF4550);
}

// 0xAF4BE0
Archive* ArchiveManager::OpenArchive(const char* apArchiveName, UInt16 aiForceArchiveType, bool abInvalidateOtherArchives) {
    return CdeclCall<Archive*>(0xAF4BE0, apArchiveName, aiForceArchiveType, abInvalidateOtherArchives);
}

// 0xAF6320
bool ArchiveManager::FindFile(const char* apFileName, ARCHIVE_TYPE aiArchiveType) {
    return GetArchiveForFile(apFileName, aiArchiveType) != nullptr;
}

// 0xAF5FA0
ArchiveFile* ArchiveManager::GetFile(const char* apFileName, UInt32 aiBufferSize, ARCHIVE_TYPE aeArchiveType) {
    return CdeclCall<ArchiveFile*>(0xAF5FA0, apFileName, aiBufferSize, aeArchiveType);
}

// 0xAF6540
BSFileEntry* ArchiveManager::GetFileEntryForFile(ARCHIVE_TYPE_INDEX aeArchiveTypeIndex, BSHash& arDirectoryHash, BSHash& arFileHash, const char* apFileName) {
    return CdeclCall<BSFileEntry*>(0xAF6540, aeArchiveTypeIndex, &arDirectoryHash, &arFileHash, apFileName);
}

// 0xAF7D80
ARCHIVE_TYPE ArchiveManager::GetArchiveTypeFromFileName(const char* apFileName) {
    return CdeclCall<ARCHIVE_TYPE>(0xAF7D80, apFileName);
}

// 0xAF7DB0
ARCHIVE_TYPE ArchiveManager::GetArchiveTypeFromFileExtension(const char* apExtension) {
    return CdeclCall<ARCHIVE_TYPE>(0xAF7DB0, apExtension);
}

// 0xAF7800
bool ArchiveManager::GetFileNameForSmallestFileInDirectory(const char* apDirectory, char* apFileName, ARCHIVE_TYPE aeArchiveType) {
    return CdeclCall<bool>(0xAF7800, apDirectory, apFileName, aeArchiveType);
}

// 0xAF7400
bool ArchiveManager::GetRandomFileNameForDirectory(const char* apDirectory, char* apFileName, ARCHIVE_TYPE aeArchiveType) {
    return CdeclCall<bool>(0xAF7400, apDirectory, apFileName, aeArchiveType);
}

// 0xB014B0
const char* ArchiveManager::TrimFileName(const char* apFileName) {
    return CdeclCall<const char*>(0xB014B0, apFileName);
}

// 0xAF5550
bool ArchiveManager::RemoveArchive(Archive* apArchive)
{
    return CdeclCall<bool>(0xAF5550, apArchive);
}

// 0xAF7E80
bool ArchiveManager::WildCardMatch(char* apSearchName, BSHash* apHash) {
    return CdeclCall<bool>(0xAF7E80, apSearchName, apHash);
}