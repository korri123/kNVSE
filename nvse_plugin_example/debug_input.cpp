#include "debug_tools.h"

#if _DEBUG
#include "GameOSDepend.h"
#include "SafeWrite.h"
#include "utility.h"

namespace
{
    constexpr GUID kIID_IDirectInputDevice8A = {0x54D41080, 0xDC15, 0x4833, {0xA4, 0x1B, 0x74, 0x8F, 0x73, 0xA3, 0x81, 0x79}};

    // IDirectInputDevice8A vtable slots
    enum
    {
        kSlot_Acquire = 7,
        kSlot_GetDeviceState = 9,
        kSlot_GetDeviceData = 10,
    };

    using AcquireFunction = HRESULT(__stdcall*)(IDirectInputDevice8A*);
    using GetDeviceStateFunction = HRESULT(__stdcall*)(IDirectInputDevice8A*, DWORD, void*);
    using GetDeviceDataFunction = HRESULT(__stdcall*)(IDirectInputDevice8A*, DWORD, DIDEVICEOBJECTDATA*, DWORD*, DWORD);

    AcquireFunction g_acquire;
    GetDeviceStateFunction g_getDeviceState;
    GetDeviceDataFunction g_getDeviceData;

    struct
    {
        long x, y, wheel;
    } g_mouseMovement;

    bool IsForeground()
    {
        return GetForegroundWindow() == OSGlobals::GetSingleton()->window;
    }

    // DI8DEVTYPE_KEYBOARD, DI8DEVTYPE_MOUSE or 0: joysticks share the vtable and are left alone.
    DWORD GetKeyboardOrMouse(IDirectInputDevice8A* device)
    {
        DIDEVCAPS capabilities{sizeof(DIDEVCAPS)};
        if (FAILED(device->GetCapabilities(&capabilities)))
            return 0;
        const auto type = GET_DIDEVICE_TYPE(capabilities.dwDevType);
        return type == DI8DEVTYPE_KEYBOARD || type == DI8DEVTYPE_MOUSE ? type : 0;
    }

    // The game's devices are foreground devices: in the background Acquire fails and the game skips the poll
    // (BSInputManager::UpdateValues 0xA23010), NVSE's wrapper with it.
    HRESULT __stdcall Acquire(IDirectInputDevice8A* device)
    {
        const auto result = g_acquire(device);
        if (FAILED(result) && !IsForeground() && GetKeyboardOrMouse(device))
            return DI_OK;
        return result;
    }

    HRESULT __stdcall GetDeviceState(IDirectInputDevice8A* device, DWORD size, void* data)
    {
        auto result = g_getDeviceState(device, size, data);
        const auto type = GetKeyboardOrMouse(device);
        if (!type)
            return result;
        if (!IsForeground())
        {
            memset(data, 0, size);
            result = DI_OK;
        }
        if (type == DI8DEVTYPE_MOUSE && SUCCEEDED(result) && size >= sizeof(DIMOUSESTATE))
        {
            auto* state = static_cast<DIMOUSESTATE*>(data);
            state->lX += g_mouseMovement.x;
            state->lY += g_mouseMovement.y;
            state->lZ += g_mouseMovement.wheel;
            g_mouseMovement = {};
        }
        return result;
    }

    HRESULT __stdcall GetDeviceData(IDirectInputDevice8A* device, DWORD size, DIDEVICEOBJECTDATA* data, DWORD* count, DWORD flags)
    {
        const auto result = g_getDeviceData(device, size, data, count, flags);
        if (IsForeground() || !GetKeyboardOrMouse(device))
            return result;
        *count = 0;
        return DI_OK;
    }

    // The DirectInput device under NVSE's wrapper, which forwards QueryInterface to it.
    void** GetDeviceVTable(IDirectInputDevice8A* wrapper)
    {
        IDirectInputDevice8A* device = nullptr;
        if (!wrapper || FAILED(wrapper->QueryInterface(kIID_IDirectInputDevice8A, reinterpret_cast<void**>(&device))))
            return nullptr;
        auto** vtable = *reinterpret_cast<void***>(device);
        device->Release();
        return vtable;
    }
}

void DebugTools::Input::Install()
{
    const auto* input = *g_OSInputGlobals;
    auto** vtable = GetDeviceVTable(input->keyboard);
    if (!vtable || vtable != GetDeviceVTable(input->mouse))
    {
        DebugPrint("DebugTools: keyboard and mouse are not DirectInput devices of one class, input is not hooked");
        return;
    }
    g_acquire = static_cast<AcquireFunction>(vtable[kSlot_Acquire]);
    g_getDeviceState = static_cast<GetDeviceStateFunction>(vtable[kSlot_GetDeviceState]);
    g_getDeviceData = static_cast<GetDeviceDataFunction>(vtable[kSlot_GetDeviceData]);
    SafeWrite32(reinterpret_cast<UInt32>(&vtable[kSlot_Acquire]), reinterpret_cast<UInt32>(Acquire));
    SafeWrite32(reinterpret_cast<UInt32>(&vtable[kSlot_GetDeviceState]), reinterpret_cast<UInt32>(GetDeviceState));
    SafeWrite32(reinterpret_cast<UInt32>(&vtable[kSlot_GetDeviceData]), reinterpret_cast<UInt32>(GetDeviceData));
}

void DebugTools::Input::MoveMouse(long dx, long dy, long wheel)
{
    g_mouseMovement.x += dx;
    g_mouseMovement.y += dy;
    g_mouseMovement.wheel += wheel;
}
#endif
