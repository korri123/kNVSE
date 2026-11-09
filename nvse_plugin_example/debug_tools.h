#pragma once

// Tooling for Debug builds; both functions do nothing in Release.
namespace DebugTools
{
    // Named pipe \\.\pipe\kNVSE-debug: each message is one or more console script lines, run on the main thread in
    // console mode. The reply is "ok" or "timeout"/"error" on the first line followed by the console output.
    void StartCommandPipe();

    // CRT/STL assertion failures (e.g. span index checks) are logged to kNVSE.log and raise a breakpoint instead of
    // showing a modal dialog, so an attached debugger stops at the failing frame and an unattended run crashes
    // into CrashLogger with a stack.
    void InstallAssertHook();
}
