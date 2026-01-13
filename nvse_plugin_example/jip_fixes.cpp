#include "jip_fixes.h"

#include "main.h"
#include "SafeWrite.h"

struct PluginInfo;

namespace
{
    HMODULE hJIP = nullptr;
    UInt32 uiCrc32Table[256];
    UInt32 uiJipHash = 0x9DF36B6;

    void initCRC32Table() 
    {
        for (UInt32 i = 0; i < 256; i++) 
        {
            UInt32 crc = i;
            for (UInt32 j = 0; j < 8; j++)
            {
                constexpr UInt32 polynomial = 0xEDB88320;
                if (crc & 1)
                    crc = (crc >> 1) ^ polynomial;
                else
                    crc >>= 1;
            }
            uiCrc32Table[i] = crc;
        }
    }
    
    UInt32 crc32(const UInt8* data, size_t length) 
    {
        UInt32 crc = 0xFFFFFFFF;
        for (size_t i = 0; i < length; i++) 
        {
            UInt8 byte = data[i];
            crc = (crc >> 8) ^ uiCrc32Table[(crc ^ byte) & 0xFF];
        }
        return crc ^ 0xFFFFFFFF;
    }
    
    size_t __fastcall GetJIPAddress(size_t aiAddress)
    {
        return reinterpret_cast<size_t>(hJIP) + aiAddress - 0x10000000;
    }
    
    void PatchReloadEquippedModels()
    {
        // NiNode::DetachChild
        WriteRelCall(GetJIPAddress(0x10019C3E), INLINE_HOOK(void, __fastcall, NiNode* pNode, void*, NiAVObject* pObject)
        {
            auto* addrOfRetn = GetLambdaAddrOfRetnAddr(_AddressOfReturnAddress());
            if (auto* actor = static_cast<Actor*>(TESObjectREFR::FindReferenceFor3D(pNode)); actor && actor->IsActor() && actor->baseProcess)
            {
                if (auto* animData = actor->baseProcess->GetAnimData())
                    animData->RemoveObject(pObject);
                if (actor == g_thePlayer)
                    g_thePlayer->firstPersonAnimData->RemoveObject(pObject);
            }
            pNode->DetachChild(pObject);
            *addrOfRetn = GetJIPAddress(0x10019C44);
        }));
    }
}



void JIPFixes::Init()
{
    const auto hJIPModule = GetModuleHandle("jip_nvse.dll");
    if (!hJIPModule) 
    {
        LOG("Failed to find JIP LN!");
        return;
    }

    const PluginInfo* pInfo = g_cmdTable->GetPluginInfoByName("JIP LN NVSE");
    if (!pInfo) 
    {
        ERROR_LOG("Failed to get JIP LN plugin info!");
        return;
    }

    if (pInfo->version != 5730) 
    {
        double dVersion = pInfo->version / 100.0;
        ERROR_LOG(FormatString("Incompatible JIP LN version! Expected 57.30, got %.2f.", dVersion));
        return;
    }

    HANDLE hJIPFile = CreateFile("Data\\NVSE\\Plugins\\jip_nvse.dll", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);

    if (!hJIPFile || hJIPFile == INVALID_HANDLE_VALUE) 
    {
        LOG("Failed to find JIP LN!");
        return;
    }

    DWORD dwFileSize = GetFileSize(hJIPFile, nullptr);

    if (dwFileSize != 502272) 
    {
        ERROR_LOG("Incompatible JIP LN version!");
        return;
    }

    {
        std::vector<uint8_t> kBuffer(dwFileSize);
        DWORD dwBytesRead = 0;
        BOOL bRead = ReadFile(hJIPFile, kBuffer.data(), dwFileSize, &dwBytesRead, nullptr);
        CloseHandle(hJIPFile);

        if (bRead) 
        {
            initCRC32Table();
            uint32_t uiHash = crc32(kBuffer.data(), kBuffer.size());
            if (uiHash != uiJipHash) 
            {
                ERROR_LOG("Incompatible JIP LN binary!");
                return;
            }
        }
        else 
        {
            ERROR_LOG("Failed to read JIP LN!");
            return;
        }
    }

    hJIP = hJIPModule;
    
    PatchReloadEquippedModels();
}
