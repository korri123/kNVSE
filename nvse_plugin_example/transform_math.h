#pragma once

#include "NiNodes.h"
#include "NiTypes.h"

// Rigid transforms of bones (NiTransform: R row-major, applied as R * v; a * b applies b first)
namespace TransformMath
{
    inline NiTransform IdentityTransform()
    {
        NiTransform result;
        result.m_Rotate = NiMatrix33{ { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } } };
        result.m_Translate = NiPoint3::ZERO;
        result.m_fScale = 1.0f;
        return result;
    }

    inline NiMatrix33 Transpose(const NiMatrix33& matrix)
    {
        NiMatrix33 result;
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col)
                result.m_pEntry[row][col] = matrix.m_pEntry[col][row];
        return result;
    }

    // Rotation matrices in bone locals are orthonormal, so the inverse rotation is the transpose
    inline NiTransform Inverse(const NiTransform& transform)
    {
        NiTransform result;
        result.m_Rotate = Transpose(transform.m_Rotate);
        result.m_fScale = 1.0f / transform.m_fScale;
        result.m_Translate = result.m_Rotate * -transform.m_Translate * result.m_fScale;
        return result;
    }

    // Key interpolation does not always produce unit quaternions. A non-unit quaternion gives a matrix that is not
    // orthonormal, which breaks the transpose-as-inverse and makes identical rotations measure as different.
    inline void ToUnitRotation(NiQuaternion quaternion, NiMatrix33& rotation)
    {
        quaternion.Normalize();
        quaternion.ToRotation(rotation);
    }

    inline NiTransform CurrentLocal(const NiAVObject* node)
    {
        NiTransform result = node->m_kLocal;
        NiQuaternion rotation;
        rotation.FromRotation(result.m_Rotate);
        ToUnitRotation(rotation, result.m_Rotate);
        return result;
    }
}
