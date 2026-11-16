#include "fire_direction.h"

#include "blend_smoothing.h"
#include "GameObjects.h"
#include "SafeWrite.h"
#include "utility.h"

namespace
{
    NiMatrix33 ToMatrix(const NiQuaternion& kRotate)
    {
        NiMatrix33 result;
        kRotate.ToRotation(result);
        return result;
    }

    // Rotation matrices are orthonormal, so the inverse is the transpose
    NiMatrix33 Transpose(const NiMatrix33& matrix)
    {
        NiMatrix33 result;
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col)
                result.m_pEntry[row][col] = matrix.m_pEntry[col][row];
        return result;
    }

    // Returns the node's world rotation with every bone above it at the rotation of its computed weights. This is the
    // node's world rotation turned by how the chain of local rotations differs, so it does not depend on the age of
    // the world transforms (Fire may run between a blend and the world update). Returns false if no bone differs.
    bool GetComputedWorldRotate(NiAVObject* node, NiMatrix33& kRotate)
    {
        constexpr unsigned int kMaxDepth = 64;
        NiAVObject* chain[kMaxDepth];
        const kBlendInterpolatorExtraData* recorded[kMaxDepth];
        unsigned int count = 0;
        bool bDiffers = false;
        for (auto* object = node; object && count < kMaxDepth; object = object->m_pkParent)
        {
            const auto* extraData = kBlendInterpolatorExtraData::GetExtraData(object);
            if (extraData && !extraData->hasComputedRotate)
                extraData = nullptr;
            bDiffers |= extraData != nullptr;
            chain[count] = object;
            recorded[count] = extraData;
            ++count;
        }
        if (!bDiffers)
            return false;

        constexpr NiMatrix33 kIdentity{ { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } } };
        NiMatrix33 kShown = kIdentity;
        NiMatrix33 kComputed = kIdentity;
        for (auto i = count; i-- > 0;)
        {
            const auto& kLocal = chain[i]->m_kLocal.m_Rotate;
            kShown = kShown * kLocal;
            if (const auto* extraData = recorded[i])
                // Uses the rotation of the bone's computed weights in place of what was written to it. Anything that
                // wrote to the bone after its blend (ragdoll controller foot and look IK, WeaponHandBlend reaching
                // with the arm) is kept.
                kComputed = kComputed * kLocal * Transpose(ToMatrix(extraData->writtenRotate))
                    * ToMatrix(extraData->computedRotate);
            else
                kComputed = kComputed * kLocal;
        }
        kRotate = node->m_kWorld.m_Rotate * Transpose(kShown) * kComputed;
        return true;
    }

    // TESObjectWEAP::Fire's call of NiAVObject::GetWorldTransform (0x5237EC) on the projectile node whose rotation the
    // shot is aimed along (NiMatrix3::ToEulerAnglesZXY), for every shooter except the player in first person
    const NiTransform* __fastcall ProjectileNodeTransformHook(NiAVObject* node)
    {
        // Fire's actor (thiscall, first stack argument). NPCs with a target aim at it, and their bones may be blended
        // on another thread while they fire.
        if (GET_CALLER_VAR(TESObjectREFR*, 0x8) != g_thePlayer)
            return &node->m_kWorld;
        thread_local NiTransform kAimed;
        if (!GetComputedWorldRotate(node, kAimed.m_Rotate))
            return &node->m_kWorld;
        kAimed.m_Translate = node->m_kWorld.m_Translate;
        kAimed.m_fScale = node->m_kWorld.m_fScale;
        return &kAimed;
    }
}

void FireDirection::Record(kBlendInterpolatorExtraData& extraData, const NiQuaternion& kComputedRotate,
    const NiQuatTransform& kValue)
{
    const auto& kWritten = kValue.GetRotate();
    extraData.hasComputedRotate = kComputedRotate.m_fX != -NI_INFINITY && kValue.IsRotateValid()
        && (kComputedRotate.m_fW != kWritten.m_fW || kComputedRotate.m_fX != kWritten.m_fX
            || kComputedRotate.m_fY != kWritten.m_fY || kComputedRotate.m_fZ != kWritten.m_fZ);
    if (extraData.hasComputedRotate)
    {
        extraData.computedRotate = kComputedRotate;
        extraData.writtenRotate = kWritten;
    }
}

void FireDirection::WriteHooks()
{
    WriteRelCall(0x5237EC, ProjectileNodeTransformHook);
}
