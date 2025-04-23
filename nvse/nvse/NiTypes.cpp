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

float CounterWarp(float t, float fCos) 
{
    const float ATTENUATION = 0.82279687f;
    const float WORST_CASE_SLOPE = 0.58549219f;

    float fFactor = 1.0f - ATTENUATION * fCos;
    fFactor *= fFactor;
    float fK = WORST_CASE_SLOPE * fFactor;

    return t*(fK*t*(2.0f*t - 3.0f) + 1.0f + fK);
}

static const float ISQRT_NEIGHBORHOOD = 0.959066f;
static const float ISQRT_SCALE = 1.000311f;
static const float ISQRT_ADDITIVE_CONSTANT = ISQRT_SCALE / 
    std::sqrt(ISQRT_NEIGHBORHOOD);
static const float ISQRT_FACTOR = ISQRT_SCALE * (-0.5f / 
    (ISQRT_NEIGHBORHOOD * std::sqrt(ISQRT_NEIGHBORHOOD)));
float ISqrt_approx_in_neighborhood(float s) 
{
    return ISQRT_ADDITIVE_CONSTANT + (s - ISQRT_NEIGHBORHOOD) * ISQRT_FACTOR;
}

void NiQuaternion::FastNormalize() 
{
    float s = m_fX*m_fX + m_fY*m_fY + m_fZ*m_fZ + m_fW*m_fW; // length^2
    float k = ISqrt_approx_in_neighborhood(s);

    if (s <= 0.91521198f) {
        k *= ISqrt_approx_in_neighborhood(k * k * s);

        if (s <= 0.65211970f) {
            k *= ISqrt_approx_in_neighborhood(k * k * s);
        }
    }

    m_fX *= k;
    m_fY *= k;
    m_fZ *= k;
    m_fW *= k;
}

NiQuaternion NiQuaternion::Slerp(float t, const NiQuaternion& p, const NiQuaternion& q)
{
    // assert:  Dot(p,q) >= 0 (guaranteed in NiRotKey::Interpolate methods)
    // (but not necessarily true when coming from a Squad call)

    // This algorithm is Copyright (c) 2002 Jonathan Blow, from his article 
    // "Hacking Quaternions" in Game Developer Magazine, March 2002.
    
    float fCos = Dot(p, q);

    float fTPrime;
    if (t <= 0.5f) {
        fTPrime = CounterWarp(t, fCos);
    } else {
        fTPrime = 1.0f - CounterWarp(1.0f - t, fCos);
    }

    NiQuaternion kResult(
        std::lerp(p.GetW(), q.GetW(), fTPrime),
        std::lerp(p.GetX(), q.GetX(), fTPrime),
        std::lerp(p.GetY(), q.GetY(), fTPrime),
        std::lerp(p.GetZ(), q.GetZ(), fTPrime));

    kResult.FastNormalize();
    return kResult;
}