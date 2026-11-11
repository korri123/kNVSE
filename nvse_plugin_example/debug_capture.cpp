#include "debug_tools.h"

#if _DEBUG
#include <algorithm>
#include <cmath>
#include <d3d9.h>
#include <fstream>
#include <wrl/client.h>

#include "utility.h"

std::string DebugTools::Capture::BackBuffer(float scale)
{
    using Microsoft::WRL::ComPtr;
    const auto failed = [](const char* call, HRESULT result)
    {
        return FormatString("error\n%s failed (%08X)\n", call, result);
    };

    auto* renderer = *reinterpret_cast<UInt8**>(0x11F4748); // NiRenderer::ms_pkRenderer
    auto* device = renderer ? *reinterpret_cast<IDirect3DDevice9**>(renderer + 0x288) : nullptr; // NiDX9Renderer::m_pkD3DDevice9
    if (!device)
        return "error\nno renderer\n";

    ComPtr<IDirect3DSurface9> backBuffer;
    if (const auto result = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer); FAILED(result))
        return failed("GetBackBuffer", result);
    D3DSURFACE_DESC description;
    if (const auto result = backBuffer->GetDesc(&description); FAILED(result))
        return failed("GetDesc", result);
    if (description.Format != D3DFMT_X8R8G8B8 && description.Format != D3DFMT_A8R8G8B8)
        return FormatString("error\nback buffer format %d is not 32 bit BGR\n", description.Format);
    const auto width = std::clamp<UINT>(std::lround(description.Width * scale), 1, description.Width);
    const auto height = std::clamp<UINT>(std::lround(description.Height * scale), 1, description.Height);

    // the back buffer can be multisampled and is not lockable: resolve and scale it on the GPU, then read that
    ComPtr<IDirect3DSurface9> scaled;
    if (const auto result = device->CreateRenderTarget(width, height, description.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &scaled, nullptr); FAILED(result))
        return failed("CreateRenderTarget", result);
    if (const auto result = device->StretchRect(backBuffer.Get(), nullptr, scaled.Get(), nullptr, D3DTEXF_LINEAR); FAILED(result))
        return failed("StretchRect", result);
    ComPtr<IDirect3DSurface9> copy;
    if (const auto result = device->CreateOffscreenPlainSurface(width, height, description.Format, D3DPOOL_SYSTEMMEM, &copy, nullptr); FAILED(result))
        return failed("CreateOffscreenPlainSurface", result);
    if (const auto result = device->GetRenderTargetData(scaled.Get(), copy.Get()); FAILED(result))
        return failed("GetRenderTargetData", result);
    D3DLOCKED_RECT locked;
    if (const auto result = copy->LockRect(&locked, nullptr, D3DLOCK_READONLY); FAILED(result))
        return failed("LockRect", result);

    const auto path = TempFilePath("kNVSE-debug-screenshot.bgrx");
    std::ofstream file(path, std::ios::binary);
    for (UINT row = 0; row < height; ++row)
        file.write(static_cast<const char*>(locked.pBits) + row * locked.Pitch, width * 4);
    copy->UnlockRect();
    if (!file)
        return "error\ncan't write " + path + "\n";
    return FormatString("ok\n%s\n%u %u\n", path.c_str(), width, height);
}
#endif
