// Entry point. We install the recon hooks as early as we can (DllMain, if d3d12.dll is already in the
// process), and the dxgi proxy installs them again on the first factory call as a safety net. There is
// no loader/signature/nvngx work here (that was the DLSS path); FSR 2 needs none of it.
#include <windows.h>
#include "log.h"
#include <cstring>

void InstallReconHooks(); // d3d12_recon.cpp

// Access violations: log where (module + offset, a short backtrace) before the process dies -- a crash in the game
// with our DLL inside must point at a function (map file of the PC build / llvm-addr2line of the console build).
static void where(void* a, char* out, size_t n)
{
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR) a, &m) && m)
    {
        GetModuleFileNameA(m, name, MAX_PATH);
        const char* b = strrchr(name, 92); // backslash
        snprintf(out, n, "%s+0x%llx", b ? b + 1 : name, (unsigned long long) ((char*) a - (char*) m));
    }
    else
        snprintf(out, n, "%p", a);
}
static LONG CALLBACK onCrash(EXCEPTION_POINTERS* e)
{
    static LONG count = 0;
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || InterlockedIncrement(&count) > 3)
        return EXCEPTION_CONTINUE_SEARCH;
    char w[300];
    where(e->ExceptionRecord->ExceptionAddress, w, sizeof(w));
    LOGI("CRASH access violation at %s (%s %p)", w, e->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
         (void*) e->ExceptionRecord->ExceptionInformation[1]);
    void* bt[16] = {};
    USHORT k = CaptureStackBackTrace(0, 16, bt, nullptr);
    for (USHORT i = 0; i < k; i++)
    {
        where(bt[i], w, sizeof(w));
        LOGI("  #%u %s", i, w);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(inst);
        LOGI("fsr2 static recon attached (pid %lu)", GetCurrentProcessId());
        // opt-in: under Wine/FEX (the console) access violations are raised and handled routinely
        char e[4] = {};
        if (GetEnvironmentVariableA("VELA_CRASHLOG", e, sizeof(e)) > 0 && e[0] == '1') AddVectoredExceptionHandler(1, onCrash);
        InstallReconHooks();
    }
    return TRUE;
}
