#include "NiNodes.h"

// 0x4968B0
NiRefObject::NiRefObject() {
    ThisStdCall(0x4968B0, this);
}

NiBinaryStream::NiBinaryStream()
{
    m_pfnRead = nullptr;
    m_pfnWrite = nullptr;
    m_uiAbsoluteCurrentPos = 0;
}

// 0xAA14B0
NiFile::NiFile()
{
    ThisStdCall(0xAA14B0, this);
}
