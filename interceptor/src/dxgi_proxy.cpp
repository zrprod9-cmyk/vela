// dxgi proxy: the plugin installs us as dxgi.dll in the game folder, so we load early into the process.
// We forward the DXGI entry points to the real dxgi (DXVK on the console) and, on the first factory
// call, make sure the D3D12 recon hooks are installed before the game creates its device. No adapter
// spoof: FSR 2 is cross-vendor, the game offers it on Adreno with no NVIDIA pretence).
// We also hook the swap chain's Present / Present1 (once, through the factory) to count frames: the fps
// numbers come from here, the same counter with or without the D3D12 hooks (RECON_LEVEL=0).
#include <windows.h>
// the SDK headers declare these as imports; we define them (MSVC rejects the clash, clang ignores it)
#define CreateDXGIFactory CreateDXGIFactory_sdk
#define CreateDXGIFactory1 CreateDXGIFactory1_sdk
#define CreateDXGIFactory2 CreateDXGIFactory2_sdk
#define DXGIDeclareAdapterRemovalSupport DXGIDeclareAdapterRemovalSupport_sdk
#define DXGIGetDebugInterface1 DXGIGetDebugInterface1_sdk
#include <dxgi1_6.h>
#undef CreateDXGIFactory
#undef CreateDXGIFactory1
#undef CreateDXGIFactory2
#undef DXGIDeclareAdapterRemovalSupport
#undef DXGIGetDebugInterface1
#include <detours/detours.h>
#include "log.h"

void InstallReconHooks(); // d3d12_recon.cpp
void ReconOnPresent();    // d3d12_recon.cpp

namespace
{
HMODULE g_real = nullptr;
HMODULE real()
{
    if (g_real) return g_real;
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    wcscpy_s(path + n, MAX_PATH - n, L"\\dxgi.dll");
    g_real = LoadLibraryW(path);
    if (!g_real) LOGI("dxgi proxy: failed to load system dxgi");
    return g_real;
}
template <typename T> T fn(const char* name)
{
    HMODULE m = real();
    return m ? (T) GetProcAddress(m, name) : nullptr;
}

// ---- Present counter ------------------------------------------------------------------------------------------
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
PFN_Present o_Present = nullptr;
PFN_Present1 o_Present1 = nullptr;
HRESULT STDMETHODCALLTYPE h_Present(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    if (!(flags & DXGI_PRESENT_TEST)) ReconOnPresent();
    return o_Present(sc, sync, flags);
}
HRESULT STDMETHODCALLTYPE h_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p)
{
    if (!(flags & DXGI_PRESENT_TEST)) ReconOnPresent();
    return o_Present1(sc, sync, flags, p);
}
void hookSwapChain(IUnknown* scUnk)
{
    static bool done = false;
    if (done || !scUnk) return;
    IDXGISwapChain1* sc = nullptr;
    if (FAILED(scUnk->QueryInterface(__uuidof(IDXGISwapChain1), (void**) &sc)) || !sc) return;
    done = true;
    void** vt = *(void***) sc;
    o_Present = (PFN_Present) vt[8];
    o_Present1 = (PFN_Present1) vt[22];
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&) o_Present, (PVOID) h_Present);
    DetourAttach(&(PVOID&) o_Present1, (PVOID) h_Present1);
    LONG r = DetourTransactionCommit();
    LOGI("present hooks: Present=%p Present1=%p commit=%ld", (void*) o_Present, (void*) o_Present1, r);
    sc->Release();
}

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSC)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSCHwnd)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                     IDXGISwapChain1**);
PFN_CreateSC o_CreateSC = nullptr;
PFN_CreateSCHwnd o_CreateSCHwnd = nullptr;
HRESULT STDMETHODCALLTYPE h_CreateSC(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** pp)
{
    HRESULT r = o_CreateSC(f, dev, d, pp);
    if (SUCCEEDED(r) && pp) hookSwapChain(*pp);
    return r;
}
HRESULT STDMETHODCALLTYPE h_CreateSCHwnd(IDXGIFactory2* f, IUnknown* dev, HWND w, const DXGI_SWAP_CHAIN_DESC1* d,
                                         const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o,
                                         IDXGISwapChain1** pp)
{
    HRESULT r = o_CreateSCHwnd(f, dev, w, d, fs, o, pp);
    if (SUCCEEDED(r) && pp) hookSwapChain(*pp);
    return r;
}
void hookFactory(void* factory)
{
    static bool done = false;
    if (done || !factory) return;
    IDXGIFactory2* f2 = nullptr;
    if (FAILED(((IUnknown*) factory)->QueryInterface(__uuidof(IDXGIFactory2), (void**) &f2)) || !f2) return;
    done = true;
    void** vt = *(void***) f2;
    o_CreateSC = (PFN_CreateSC) vt[10];
    o_CreateSCHwnd = (PFN_CreateSCHwnd) vt[15];
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&) o_CreateSC, (PVOID) h_CreateSC);
    DetourAttach(&(PVOID&) o_CreateSCHwnd, (PVOID) h_CreateSCHwnd);
    LONG r = DetourTransactionCommit();
    LOGI("factory hooks: CreateSwapChain=%p CreateSwapChainForHwnd=%p commit=%ld", (void*) o_CreateSC,
         (void*) o_CreateSCHwnd, r);
    f2->Release();
}
} // namespace

extern "C" {

__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** pp)
{
    static auto p = fn<HRESULT(WINAPI*)(REFIID, void**)>("CreateDXGIFactory");
    InstallReconHooks();
    HRESULT r = p ? p(riid, pp) : E_FAIL;
    if (SUCCEEDED(r) && pp) hookFactory(*pp);
    return r;
}
__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** pp)
{
    static auto p = fn<HRESULT(WINAPI*)(REFIID, void**)>("CreateDXGIFactory1");
    InstallReconHooks();
    HRESULT r = p ? p(riid, pp) : E_FAIL;
    if (SUCCEEDED(r) && pp) hookFactory(*pp);
    return r;
}
__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** pp)
{
    static auto p = fn<HRESULT(WINAPI*)(UINT, REFIID, void**)>("CreateDXGIFactory2");
    InstallReconHooks();
    HRESULT r = p ? p(flags, riid, pp) : E_FAIL;
    if (SUCCEEDED(r) && pp) hookFactory(*pp);
    return r;
}
__declspec(dllexport) HRESULT WINAPI DXGIDeclareAdapterRemovalSupport()
{
    static auto p = fn<HRESULT(WINAPI*)()>("DXGIDeclareAdapterRemovalSupport");
    return p ? p() : S_OK;
}
__declspec(dllexport) HRESULT WINAPI DXGIGetDebugInterface1(UINT flags, REFIID riid, void** pp)
{
    static auto p = fn<HRESULT(WINAPI*)(UINT, REFIID, void**)>("DXGIGetDebugInterface1");
    return p ? p(flags, riid, pp) : E_FAIL;
}
} // extern "C"
