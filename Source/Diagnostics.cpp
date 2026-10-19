/*
    Crash tracer for offline verification runs.

    A hosted VST3 that faults inside the DLL is hard to inspect without a
    debugger, so when RIVAGE_CRASH_TRACE is set in the environment this installs
    an unhandled-exception filter that walks the stack with DbgHelp and appends
    a readable report (module!symbol+offset, source file and line when the PDB
    is available) to the file named by RIVAGE_CRASH_LOG.

    It is completely inert unless the environment variable is present, so the
    shipping build carries no global exception handler.
*/

#include <juce_audio_basics/juce_audio_basics.h>

#if JUCE_WINDOWS

#include <windows.h>
#include <dbghelp.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#pragma comment(lib, "dbghelp.lib")

namespace rivage
{
namespace diag
{

static void appendText (const char* text)
{
    char path[MAX_PATH] = {};

    if (GetEnvironmentVariableA ("RIVAGE_CRASH_LOG", path, MAX_PATH) == 0 || path[0] == 0)
        strcpy_s (path, "rivage_crash.txt");

    FILE* file = nullptr;

    if (fopen_s (&file, path, "a") == 0 && file != nullptr)
    {
        std::fputs (text, file);
        std::fflush (file);
        std::fclose (file);
    }
}

static void tracef (const char* format, ...)
{
    char buffer[4096];
    va_list args;
    va_start (args, format);
    _vsnprintf_s (buffer, sizeof (buffer), _TRUNCATE, format, args);
    va_end (args);

    appendText (buffer);
    appendText ("\n");
}

static const char* moduleNameFor (HANDLE process, DWORD64 address, char* storage, size_t storageSize)
{
    const DWORD64 base = SymGetModuleBase64 (process, address);

    if (base == 0)
        return "?";

    if (GetModuleFileNameA (reinterpret_cast<HMODULE> (base), storage, (DWORD) storageSize) == 0)
        return "?";

    const char* slash = std::strrchr (storage, '\\');
    return slash != nullptr ? slash + 1 : storage;
}

static LONG WINAPI crashHandler (EXCEPTION_POINTERS* info)
{
    tracef ("=== RIVAGE EXCEPTION code=0x%08lX address=%p ===",
            (unsigned long) info->ExceptionRecord->ExceptionCode,
            (void*) info->ExceptionRecord->ExceptionAddress);

    HANDLE process = GetCurrentProcess();
    HANDLE thread  = GetCurrentThread();

    SymSetOptions (SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize (process, nullptr, TRUE);

    CONTEXT context = *info->ContextRecord;

    STACKFRAME64 frame = {};
    frame.AddrPC.Offset    = context.Rip;
    frame.AddrPC.Mode      = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode   = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode   = AddrModeFlat;

    for (int depth = 0; depth < 40; ++depth)
    {
        if (! StackWalk64 (IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
                           SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
            break;

        const DWORD64 address = frame.AddrPC.Offset;

        if (address == 0)
            break;

        char moduleStorage[MAX_PATH] = {};
        const char* module = moduleNameFor (process, address, moduleStorage, sizeof (moduleStorage));

        char symbolStorage[sizeof (SYMBOL_INFO) + 1024] = {};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*> (symbolStorage);
        symbol->SizeOfStruct = sizeof (SYMBOL_INFO);
        symbol->MaxNameLen   = 1000;

        DWORD64 displacement = 0;
        IMAGEHLP_LINE64 line = {};
        line.SizeOfStruct = sizeof (line);
        DWORD lineDisplacement = 0;

        if (SymFromAddr (process, address, &displacement, symbol))
        {
            if (SymGetLineFromAddr64 (process, address, &lineDisplacement, &line))
                tracef ("#%-2d %s!%s+0x%llx  [%s:%lu]", depth, module, symbol->Name,
                        (unsigned long long) displacement, line.FileName, (unsigned long) line.LineNumber);
            else
                tracef ("#%-2d %s!%s+0x%llx", depth, module, symbol->Name,
                        (unsigned long long) displacement);
        }
        else
        {
            const DWORD64 base = SymGetModuleBase64 (process, address);
            tracef ("#%-2d %s+0x%llx", depth, module, (unsigned long long) (address - base));
        }
    }

    tracef ("=== end of report ===");
    return EXCEPTION_EXECUTE_HANDLER;
}

struct Installer
{
    Installer()
    {
        char value[8] = {};

        if (GetEnvironmentVariableA ("RIVAGE_CRASH_TRACE", value, 8) > 0)
        {
            appendText ("--- rivage crash tracer installed ---\n");
            SetUnhandledExceptionFilter (&crashHandler);
        }
    }
};

static Installer& installer()
{
    static Installer instance;
    return instance;
}

void touch()
{
    // Keeps the installer alive when the linker would otherwise drop it.
    static volatile bool referenced = false;
    juce::ignoreUnused (referenced);
    juce::ignoreUnused (installer());
}

/** Always-on breadcrumb, enabled with RIVAGE_TRACE=1. Written to stderr so the
    offline host picks it up, and to the debugger output for DAW sessions. */
void note (const char* message)
{
    static const bool enabled = []
    {
        char value[8] = {};
        return GetEnvironmentVariableA ("RIVAGE_TRACE", value, 8) > 0;
    }();

    if (! enabled)
        return;

    std::fprintf (stderr, "[rivage] %s\n", message);
    std::fflush (stderr);
    OutputDebugStringA (message);
}


} // namespace diag
} // namespace rivage

#endif
