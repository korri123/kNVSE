#include "NiStream.h"

NiStream* NiStream::Create(NiStream* apThis)
{
    return ThisStdCall<NiStream*>(0xA66150, apThis);
}

NiStream* NiStream::Create()
{
    auto* alloc = static_cast<NiStream*>(FormHeap_Allocate(sizeof(NiStream)));
    return Create(alloc);
}
