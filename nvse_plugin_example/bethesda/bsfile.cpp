#include "bethesda_types.h"

// 0xAFF180
BSFile::BSFile()
{
    ThisStdCall(0xAFF180, this);
}

// 0xB00260
BSFile::BSFile(const char* apName, OpenMode aeMode, UInt32 auiBufferSize, bool abTextMode)
{
    ThisStdCall(0xB00260, this, apName, aeMode, auiBufferSize, abTextMode);
}
