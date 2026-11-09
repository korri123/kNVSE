#include "debug_tools.h"

#if _DEBUG
#include <chrono>
#include <crtdbg.h>
#include <cstdio>
#include <fstream>
#include <future>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#include "GameAPI.h"
#include "GameScript.h"
#include "main.h"
#include "SafeWrite.h"
#include "utility.h"

namespace
{
    constexpr auto kPipeName = R"(\\.\pipe\kNVSE-debug)";
    constexpr DWORD kBufferSize = 64 * 1024;
    constexpr auto kMainThreadTimeout = std::chrono::seconds(10);

    std::string OutputFilePath()
    {
        char tempDir[MAX_PATH];
        GetTempPathA(MAX_PATH, tempDir);
        return std::string(tempDir) + "kNVSE-debug-console.txt";
    }

    // Same as NVSE's RunScriptLine, except the console-mode argument of Script::CompileAndRun is passed as an int:
    // NVSE passes a bool, the game compares the whole int against 1, so the garbage upper bytes leave console mode
    // off and commands such as GetPos print nothing.
    void RunConsoleLine(const char* text)
    {
        auto* console = *reinterpret_cast<ConsoleManager**>(0x11D8CE8);
        alignas(Script) UInt8 buffer[sizeof(Script)];
        auto* script = reinterpret_cast<Script*>(buffer);
        ThisStdCall(0x5AA0F0, script); // Script::Script
        ThisStdCall(0x484490, script); // TESForm::MarkAsTemporary
        ThisStdCall(0x5ABE50, script, text); // Script::SetText
        ThisStdCall(0x5AC400, script, console->scriptContext, 1, static_cast<TESObjectREFR*>(nullptr)); // Script::CompileAndRun
        ThisStdCall(0x5AA1A0, script); // Script::~Script
    }

    // Main thread: run the lines with the console mirrored into a file (MenuConsole::WriteToFile, the mechanism
    // behind the SCOF command), then return what was printed. Interface::PrintLine, which command results such as
    // GetPos use, only prints while the console is open or CombatController::bShowVerboseMessages is set, so the
    // flag is raised for the duration.
    std::string RunLines(const std::string& request)
    {
        auto* console = *reinterpret_cast<ConsoleManager**>(0x11D8CE8);
        if (!console)
            return "error\nconsole not initialized\n";
        const auto outputPath = OutputFilePath();
        std::remove(outputPath.c_str());
        char previousOutputFile[sizeof(ConsoleManager::COFileName)] = {};
        memcpy(previousOutputFile, console->COFileName, sizeof(previousOutputFile));
        strcpy_s(console->COFileName, outputPath.c_str());
        auto& showVerboseMessages = *reinterpret_cast<bool*>(0x11F158C);
        const bool previousShowVerboseMessages = showVerboseMessages;
        showVerboseMessages = true;

        std::istringstream lines(request);
        std::string line;
        while (std::getline(lines, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (!line.empty())
                RunConsoleLine(line.c_str());
        }

        showVerboseMessages = previousShowVerboseMessages;
        memcpy(console->COFileName, previousOutputFile, sizeof(previousOutputFile));

        std::ifstream output(outputPath, std::ios::binary);
        std::ostringstream reply;
        reply << "ok\n" << output.rdbuf();
        return reply.str();
    }

    std::string RunOnMainThread(const std::string& request)
    {
        auto result = std::make_shared<std::promise<std::string>>();
        auto future = result->get_future();
        {
            ScopedLock lock(g_executionQueueCS);
            g_synchronizedExecutionQueue.emplace_back([request, result] { result->set_value(RunLines(request)); });
        }
        if (future.wait_for(kMainThreadTimeout) != std::future_status::ready)
            return "timeout\nthe main game loop did not run the request within 10 seconds\n";
        return future.get();
    }

    void ServeClient(HANDLE pipe)
    {
        std::string buffer(kBufferSize, '\0');
        while (true)
        {
            DWORD bytesRead = 0;
            if (!ReadFile(pipe, buffer.data(), kBufferSize, &bytesRead, nullptr) || bytesRead == 0)
                return;
            const auto reply = RunOnMainThread(buffer.substr(0, bytesRead));
            DWORD bytesWritten = 0;
            if (!WriteFile(pipe, reply.data(), static_cast<DWORD>(reply.size()), &bytesWritten, nullptr))
                return;
        }
    }

    void ServerLoop()
    {
        while (true)
        {
            HANDLE pipe = CreateNamedPipeA(kPipeName, PIPE_ACCESS_DUPLEX, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                           1, kBufferSize, kBufferSize, 0, nullptr);
            if (pipe == INVALID_HANDLE_VALUE)
            {
                ERROR_LOG(FormatString("DebugTools: CreateNamedPipe failed (%u)", GetLastError()));
                return;
            }
            if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED)
                ServeClient(pipe);
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
        }
    }

    int __cdecl ReportHook(int reportType, char* message, int* returnValue)
    {
        if (reportType == _CRT_WARN)
            return FALSE;
        DebugPrint(std::string(reportType == _CRT_ASSERT ? "CRT assertion failed: " : "CRT error: ") + (message ? message : ""));
        *returnValue = 1; // _CrtDbgReport returns 1, so the failing site executes _CrtDbgBreak
        return TRUE; // handled: no dialog
    }
}

void DebugTools::StartCommandPipe()
{
    std::thread(ServerLoop).detach();
}

void DebugTools::InstallAssertHook()
{
    _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, ReportHook);
}
#else
void DebugTools::StartCommandPipe()
{
}

void DebugTools::InstallAssertHook()
{
}
#endif
