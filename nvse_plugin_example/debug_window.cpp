#include "debug_tools.h"

#if _DEBUG
#include <cstdio>

#include "SafeWrite.h"
#include "utility.h"

namespace
{
    constexpr auto kRequestFile = "kNVSE-debug-background.txt";

    constexpr UInt32 kCreateGameWindowCall = 0x86AF42;   // WinMain
    constexpr UInt32 kCreateRenderWindowCall = 0x4DA89C; // TESRenderer::Init
    constexpr UInt32 kSetWindowPosImport = 0xFDF2A4;
    constexpr UInt32 kGetActiveWindowImport = 0xFDF2FC;

    constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    // WS_EX_NOACTIVATE: the system never activates the window, not when it is shown and not when the foreground
    // window closes with it next in line. WindowProc activates it on a click into the picture.
    constexpr DWORD kExStyle = WS_EX_NOACTIVATE | WS_EX_APPWINDOW;

    RECT g_clientRect; // of the game's window, screen coordinates
    HWND g_window;
    WNDPROC g_windowProc;

    // In front of the game's WinProc (0x86A0A0), for its window and the child the frames are presented to.
    LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_ACTIVATE:
            // the game would stop its clock (TESMain::SetActive 0x871C90) and open the pause menu
            if (LOWORD(wParam) == WA_INACTIVE)
                return 0;
            break;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
            SetForegroundWindow(g_window);
            break;
        default:
            break;
        }
        return CallWindowProcA(g_windowProc, window, message, wParam, lParam);
    }

    // CreateWindowExW: OneTweak hooks CreateWindowExA while the game starts and makes the window it sees
    // borderless at the size of the monitor.
    HWND CreateWindowInFront(DWORD exStyle, LPCSTR className, LPCSTR name, DWORD style, const RECT& rect, HWND parent, HMENU menu, HINSTANCE instance, LPVOID parameter)
    {
        const auto widen = [](LPCSTR text) { return text ? std::wstring(text, text + strlen(text)) : std::wstring(); };
        const auto window = CreateWindowExW(exStyle, widen(className).c_str(), widen(name).c_str(), style, rect.left, rect.top, rect.right - rect.left,
                                            rect.bottom - rect.top, parent, menu, instance, parameter);
        g_windowProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WindowProc)));
        return window;
    }

    // The game creates a visible 320x240 window, which activates it.
    HWND WINAPI CreateGameWindow(DWORD, LPCSTR className, LPCSTR name, DWORD, int, int, int, int, HWND parent, HMENU menu, HINSTANCE instance, LPVOID parameter)
    {
        RECT rect = g_clientRect;
        AdjustWindowRectEx(&rect, kStyle, FALSE, kExStyle);
        g_window = CreateWindowInFront(kExStyle, className, name, kStyle, rect, parent, menu, instance, parameter);
        ShowWindow(g_window, SW_SHOWNOACTIVATE);
        return g_window;
    }

    // The game creates the child at the back buffer's size. At the size of its parent, the frames are scaled to it.
    HWND WINAPI CreateRenderWindow(DWORD exStyle, LPCSTR className, LPCSTR name, DWORD style, int, int, int, int, HWND parent, HMENU menu, HINSTANCE instance, LPVOID parameter)
    {
        RECT rect;
        GetClientRect(parent, &rect);
        return CreateWindowInFront(exStyle, className, name, style, rect, parent, menu, instance, parameter);
    }

    // The game sizes its window to the back buffer, which also activates it (TESRenderer::Init 0x4DA951).
    BOOL WINAPI SetGameWindowPos(HWND window, HWND insertAfter, int x, int y, int width, int height, UINT flags)
    {
        return window == g_window || SetWindowPos(window, insertAfter, x, y, width, height, flags);
    }

    // The game's main loop only updates while this is its window (WinMain 0x86B343), and its input devices are
    // created for the window this returns (BSInputManager::BSInputManager 0xA22660).
    HWND WINAPI GetGameWindow()
    {
        return g_window;
    }
}

void DebugTools::StartBackgroundMode()
{
    // a file rather than an environment variable: a game started through a running Mod Organizer gets that
    // instance's environment
    const auto request = TakeRequest(kRequestFile);
    if (request.empty())
        return;
    int x, y, width, height;
    if (sscanf_s(request.c_str(), "%d,%d,%d,%d", &x, &y, &width, &height) != 4)
    {
        DebugPrint(FormatString("DebugTools: %s is not x,y,width,height: %s", kRequestFile, request.c_str()));
        return;
    }
    g_clientRect = {x, y, x + width, y + height};

    // both are 6 byte calls through the import table
    WriteRelCall(kCreateGameWindowCall, CreateGameWindow);
    SafeWrite8(kCreateGameWindowCall + 5, 0x90);
    WriteRelCall(kCreateRenderWindowCall, CreateRenderWindow);
    SafeWrite8(kCreateRenderWindowCall + 5, 0x90);
    SafeWrite32(kSetWindowPosImport, reinterpret_cast<UInt32>(SetGameWindowPos));
    SafeWrite32(kGetActiveWindowImport, reinterpret_cast<UInt32>(GetGameWindow));
}
#else
void DebugTools::StartBackgroundMode()
{
}
#endif
