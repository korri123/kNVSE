#pragma once
#include <string>

// Tooling for Debug builds; the functions outside the #if do nothing in Release.
namespace DebugTools
{
    // Named pipe \\.\pipe\kNVSE-debug. A message is either console script lines, run on the main thread in console
    // mode, or one tool command:
    //   !screenshot [scale]                the back buffer of the next frame, scaled (0-1, default 1), as raw BGRX rows
    //                                      in a temp file; replies the file's path and "<width> <height>"
    //   !mouse <dx> <dy> [wheel]           mouse movement for the next input poll (see Input)
    // The reply is "ok" or "timeout"/"error" on the first line, followed by the console output or the command's result.
    void StartCommandPipe();

    // CRT/STL assertion failures (e.g. span index checks) are logged to kNVSE.log and raise a breakpoint instead of
    // showing a modal dialog, so an attached debugger stops at the failing frame and an unattended run crashes
    // into CrashLogger with a stack.
    void InstallAssertHook();

    // Background mode, for driving the game while the PC is in use. On when the temp directory holds
    // kNVSE-debug-background.txt with "x,y,width,height" (screen pixels), written at most two minutes before; the
    // file is deleted. The game gets an ordinary window with that client rectangle instead of the one it and
    // OneTweak make: the frames are scaled into it, it can be dragged, and it only takes the foreground on a
    // click into the picture. The game keeps running while another window has the foreground.
    void StartBackgroundMode();

    // Every NVSE message (NVSEMessagingInterface::Message::type) passes through here.
    void HandleMessage(unsigned int type);

#if _DEBUG
    std::string TempFilePath(const char* name);

    namespace Input
    {
        // The keyboard and mouse deliver nothing of the user's while the game is not the foreground window, but
        // still report as acquired, so what NVSE inserts (TapKey, HoldKey, TapControl, ...) and MoveMouse reach
        // the game either way.
        void Install();
        void MoveMouse(long dx, long dy, long wheel);
    }

    namespace Capture
    {
        // The pipe's reply to !screenshot; call on the thread that presents, before the frame is presented.
        std::string BackBuffer(float scale);
    }
#endif
}
