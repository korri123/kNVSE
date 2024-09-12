#include "NiTypes.h"

const NiQuaternion NiQuaternion::IDENTITY = { 1.0f, 0.0f, 0.0f, 0.0f };
const NiQuaternion NiQuaternion::INVALID_QUATERNION = { -FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX };
const NiPoint3 NiPoint3::ZERO = { 0.0f, 0.0f, 0.0f };
const NiPoint3 NiPoint3::INVALID_POINT = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
