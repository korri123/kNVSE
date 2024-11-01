#include "NiNodes.h"

const NiQuaternion NiQuaternion::IDENTITY = { 1.0f, 0.0f, 0.0f, 0.0f };
const NiQuaternion NiQuaternion::INVALID_QUATERNION = { -FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX };
const NiPoint3 NiPoint3::ZERO = { 0.0f, 0.0f, 0.0f };
const NiPoint3 NiPoint3::INVALID_POINT = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

NiFixedString::NiFixedString(const NiFixedString& other): data(other.data)
{
    if (data)
        NiGlobalStringTable::IncRefCount(data);
}

NiFixedString& NiFixedString::operator=(const NiFixedString& other)
{
    if (data)
        NiGlobalStringTable::DecRefCount(data);
    data = other.data;
    if (data)
        NiGlobalStringTable::IncRefCount(data);
    return *this;
}

NiFixedString::NiFixedString(NiFixedString&& other) noexcept: data(other.data)
{
    other.data = nullptr;
}

NiFixedString::~NiFixedString()
{
    if (data)
        NiGlobalStringTable::DecRefCount(data);
}

NiFixedString::NiFixedString(const char* data)
{
    if (data)
        this->data = NiGlobalStringTable::AddString(data);
    else
        this->data = nullptr;
}

void NiFixedString::Set(const char* newString)
{
    if (data == newString)
        return;
    if (data)
        NiGlobalStringTable::DecRefCount(data);
    data = NiGlobalStringTable::AddString(newString);
}