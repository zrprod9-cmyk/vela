// Graphics-API recon of an FSR 2 that is built into the game's exe (RE Engine / RE4), where there is no
// ffx_fsr2_api DLL and no amd_fidelityfx DLL to hook -- so the API-export interceptor)
// never sees it. We watch the D3D12 layer the game actually drives instead (see README for why D3D12
// and not a Vulkan layer):
//
//   device vtable:  CreateComputePipelineState (shader identity: DXIL hash + any surviving names),
//                   CreateRootSignature (exact table ranges -> which register sits where),
//                   CreateDescriptorHeap / CreateShaderResourceView / CreateUnorderedAccessView /
//                   CreateConstantBufferView / CopyDescriptors / CopyDescriptorsSimple (a mirror of every
//                   CBV_SRV_UAV slot: RE Engine builds descriptors in CPU heaps and copies them over);
//   resource vtable: Map / Unmap (CPU pointers of mapped buffers, to read a constant buffer by GPU VA);
//   command list:   SetPipelineState / SetComputeRootSignature / SetComputeRoot{DescriptorTable,
//                   ConstantBufferView,32BitConstants} (what is bound) and Dispatch (the pass runs).
//
// Identification without reading the exe: the FSR 2 upscale (accumulate) pass is the compute dispatch
// that writes a display-resolution UAV while reading render-resolution SRVs; in RE4 its shader also
// keeps two FSR 2 names (r_dilated_motion_vectors, fDeviceToViewDepth). When we first see it we log
// the dispatches that preceded it on the same command list (the other FSR 2 passes), then on the next
// frame log every pass's exact bindings (register, size, format) and its constant buffer, and (level 2)
// dump every texture they bind, once, in the bench layout.
//
// RECON_LEVEL env: 1 = log only; 2 = log + one-time dump (default).
#include <windows.h>
#include <d3d12.h>
#include <detours/detours.h>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include "dxil.h"
#include "vela_fsr2.h"
#include "ext_upscalers.h"
#include "log.h"

namespace
{
std::recursive_mutex g_mtx;
using Lock = std::lock_guard<std::recursive_mutex>;
ID3D12Device* g_dev = nullptr;
int g_level = 2;
uint64_t g_dispatch = 0;
int g_logBudget = 6000;
bool logOk() { return g_logBudget-- > 0; }

int fsr2NameHits(const std::vector<std::string>& ss)
{
    static const char* toks[] = {"r_input_color", "r_input_depth", "r_input_motion", "dilated_motion",
                                  "r_dilated_depth", "lock_status", "internal_upscaled", "r_reactive",
                                  "rw_upscaled", "DeviceToViewDepth", "jitterOffset", "reconstructed_prev",
                                  "Fsr2", "FSR2", "fsr2", "depth_clip", "luma_history", "prepared_input",
                                  "exposure", "lanczos", "upsample_maximum_bias", "spd_global_atomic",
                                  "rcas", "RCAS", "img_mip_"};
    int hits = 0;
    for (auto& s : ss)
        for (auto t : toks)
            if (s.find(t) != std::string::npos) { hits++; break; }
    return hits;
}
std::string fsr2Names(const std::vector<std::string>& ss)
{
    std::string out;
    for (auto& s : ss)
        if (fsr2NameHits({s}) && out.size() < 500) out += s + " ";
    return out;
}

// ---- descriptor mirror ----------------------------------------------------------------------------
struct Slot
{
    int kind = 0; // 0 empty, 1 SRV, 2 UAV, 3 CBV
    ID3D12Resource* res = nullptr;
    DXGI_FORMAT viewFmt = DXGI_FORMAT_UNKNOWN;
    UINT64 cbvVA = 0;
    UINT cbvSize = 0;
};
struct Heap
{
    SIZE_T cpu0 = 0;
    UINT64 gpu0 = 0;
    UINT inc = 0, count = 0;
    std::vector<Slot> slots;
};
std::map<SIZE_T, Heap> g_heaps; // keyed by CPU start
uint64_t g_staleHeaps = 0, g_freedTracked = 0; // counters for the log (lifetime tracking)
std::set<ID3D12Resource*> g_freedSet; // RECON_CHECK=1: freed resources (until the pointer comes back for a new one)
int g_check = -1;
UINT g_inc = 0;

Slot* slotByCpu(SIZE_T cpu)
{
    auto it = g_heaps.upper_bound(cpu);
    if (it == g_heaps.begin()) return nullptr;
    --it;
    Heap& h = it->second;
    if (cpu >= h.cpu0 + (SIZE_T) h.count * h.inc) return nullptr;
    return &h.slots[(cpu - h.cpu0) / h.inc];
}
Heap* heapByGpu(UINT64 gpu, UINT& index)
{
    for (auto& kv : g_heaps)
    {
        Heap& h = kv.second;
        if (h.gpu0 && gpu >= h.gpu0 && gpu < h.gpu0 + (UINT64) h.count * h.inc)
        {
            index = (UINT) ((gpu - h.gpu0) / h.inc);
            return &h;
        }
    }
    return nullptr;
}

// ---- mapped buffers ---------------------------------------------------------------------------------
struct Mapped
{
    UINT64 size = 0;
    uint8_t* ptr = nullptr;
    ID3D12Resource* res = nullptr;
};
std::map<UINT64, Mapped> g_mapped; // keyed by GPU VA start

bool readable(const void* p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (const uint8_t*) p + n <= (const uint8_t*) mbi.BaseAddress + mbi.RegionSize;
}
const uint8_t* bytesAt(UINT64 va, UINT64 need)
{
    auto it = g_mapped.upper_bound(va);
    if (it == g_mapped.begin()) return nullptr;
    --it;
    if (va + need > it->first + it->second.size) return nullptr;
    const uint8_t* p = it->second.ptr + (va - it->first);
    return readable(p, need) ? p : nullptr;
}

// ---- buffer copies (RE Engine fills constants in an upload ring, then copies them into a GPU buffer) ---
struct Copied
{
    UINT64 size = 0;
    ID3D12Resource* dst = nullptr;
    UINT64 dstOff = 0;
    UINT64 srcVA = 0;
};
std::map<UINT64, Copied> g_copied; // keyed by destination GPU VA

const Copied* copiedAt(UINT64 va, UINT64& base)
{
    auto it = g_copied.upper_bound(va);
    if (it == g_copied.begin()) return nullptr;
    --it;
    if (va >= it->first + it->second.size) return nullptr;
    base = it->first;
    return &it->second;
}
// every buffer the game creates (committed or placed), so a constant buffer can be read back by its GPU VA
struct BufRes
{
    UINT64 size = 0;
    ID3D12Resource* res = nullptr;
};
std::map<UINT64, BufRes> g_bufs;
ID3D12Resource* bufAt(UINT64 va, UINT64& off)
{
    auto it = g_bufs.upper_bound(va);
    if (it == g_bufs.begin()) return nullptr;
    --it;
    if (va >= it->first + it->second.size) return nullptr;
    off = va - it->first;
    return it->second.res;
}

// CPU bytes of a constant buffer: directly mapped, or the mapped source of the copy that filled it
const uint8_t* cbBytes(UINT64 va, UINT64 need)
{
    if (const uint8_t* p = bytesAt(va, need)) return p;
    UINT64 base = 0;
    if (const Copied* c = copiedAt(va, base))
        if (c->srcVA) return bytesAt(c->srcVA + (va - base), need);
    return nullptr;
}

// ---- root signatures ------------------------------------------------------------------------------
struct Range
{
    int kind; // 1 SRV, 2 UAV, 3 CBV, 4 sampler
    UINT num, baseReg, space, offset;
};
struct Param
{
    D3D12_ROOT_PARAMETER_TYPE type;
    std::vector<Range> ranges;
    UINT reg = 0, space = 0, num32 = 0;
};
struct RootSig
{
    std::vector<Param> params;
};
std::unordered_map<ID3D12RootSignature*, RootSig> g_rs;

typedef HRESULT(WINAPI* PFN_Deser)(LPCVOID, SIZE_T, REFIID, void**);
PFN_Deser g_deser = nullptr;

bool parseRootSig(const void* blob, SIZE_T size, RootSig& out)
{
    if (!g_deser) return false;
    ID3D12VersionedRootSignatureDeserializer* d = nullptr;
    if (FAILED(g_deser(blob, size, IID_PPV_ARGS(&d))) || !d) return false;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* vd = nullptr;
    if (FAILED(d->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &vd)) || !vd)
    {
        d->Release();
        return false;
    }
    const auto& rs = vd->Desc_1_1;
    for (UINT i = 0; i < rs.NumParameters; i++)
    {
        const auto& p = rs.pParameters[i];
        Param q;
        q.type = p.ParameterType;
        if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
        {
            UINT running = 0;
            for (UINT r = 0; r < p.DescriptorTable.NumDescriptorRanges; r++)
            {
                const auto& rg = p.DescriptorTable.pDescriptorRanges[r];
                UINT off = rg.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
                               ? running
                               : rg.OffsetInDescriptorsFromTableStart;
                UINT num = rg.NumDescriptors == UINT_MAX ? 16 : rg.NumDescriptors; // unbounded: peek 16
                int kind = rg.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SRV   ? 1
                           : rg.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_UAV ? 2
                           : rg.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_CBV ? 3
                                                                             : 4;
                q.ranges.push_back({kind, num, rg.BaseShaderRegister, rg.RegisterSpace, off});
                running = off + num;
            }
        }
        else if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS)
        {
            q.reg = p.Constants.ShaderRegister;
            q.space = p.Constants.RegisterSpace;
            q.num32 = p.Constants.Num32BitValues;
        }
        else
        {
            q.reg = p.Descriptor.ShaderRegister;
            q.space = p.Descriptor.RegisterSpace;
        }
        out.params.push_back(q);
    }
    d->Release();
    return true;
}

// ---- PSO registry ----------------------------------------------------------------------------------
struct PsoInfo
{
    int id = 0;
    std::string hash, names;
    size_t bytes = 0;
    int nameHits = 0;
    bool fsrSeq = false;   // part of the FSR 2 pass sequence (learned from the accumulate pass)
    bool accumulate = false;
    bool logged = false;   // exact bindings logged once
    bool dumped = false;
    bool psvOk = false;
    std::vector<dxil::Bind> binds; // what the shader really declares (PSV0)
};
std::unordered_map<ID3D12PipelineState*, PsoInfo> g_pso;
int g_nextPso = 1;

// ---- per command list state ----------------------------------------------------------------------
struct RootArg
{
    int type = 0; // 1 table, 2 CBV, 3 constants, 4 root SRV, 5 root UAV
    UINT64 v = 0;
    uint32_t c[64] = {};
    UINT nc = 0;
};
struct Recent
{
    ID3D12PipelineState* pso;
    UINT x, y, z;
};
struct CLState
{
    ID3D12PipelineState* pso = nullptr;
    ID3D12RootSignature* rs = nullptr;
    RootArg root[64];
    std::vector<Recent> ring;
    bool afterAcc = false; // the last FSR dispatch on this list was an accumulate
    // the heaps the game last bound on this list (SetDescriptorHeaps), replayed after fsr4vk's interop work
    ID3D12DescriptorHeap* heaps[2] = {};
    UINT nHeaps = 0;
};
std::unordered_map<ID3D12GraphicsCommandList*, CLState> g_cl;
bool g_seqLearned = false;
// one-frame capture: armed by the trigger file or a delay, starts at the frame boundary after the next accumulate
int g_cap = 0;          // 0 idle, 1 armed (waiting for an accumulate), 2 capturing this frame
int g_capIndex = 0;     // c00, c01, ...
ULONGLONG g_firstAcc = 0, g_lastPoll = 0;
std::set<int> g_capDone; // passes dumped in the current capture

// ---- bindings at a dispatch -----------------------------------------------------------------------
struct Binding
{
    int kind; // 1 SRV, 2 UAV, 3 CBV
    UINT reg, space;
    ID3D12Resource* res;
    DXGI_FORMAT viewFmt;
    UINT64 cbvVA;
    UINT cbvSize;
};
bool declared(const PsoInfo* pi, int kind, UINT reg, UINT space)
{
    static int off = -1;
    if (off < 0) { char e[4] = {}; off = GetEnvironmentVariableA("RECON_NOPSV", e, 4) > 0; }
    if (off || !pi || !pi->psvOk) return true; // no PSV0 (or disabled): keep everything
    for (auto& b : pi->binds)
        if (b.kind == kind && b.space == space && reg >= b.lo && (reg <= b.hi || b.hi == 0xffffffffu)) return true;
    return false;
}
void bindingsOf(const CLState& st, std::vector<Binding>& out, std::vector<std::pair<UINT, const RootArg*>>& consts)
{
    auto pit = st.pso ? g_pso.find(st.pso) : g_pso.end();
    const PsoInfo* pi = pit != g_pso.end() ? &pit->second : nullptr;
    auto rit = g_rs.find(st.rs);
    if (rit == g_rs.end()) return;
    const RootSig& rs = rit->second;
    for (UINT p = 0; p < rs.params.size() && p < 64; p++)
    {
        const Param& q = rs.params[p];
        const RootArg& a = st.root[p];
        if (q.type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE && a.type == 1)
        {
            UINT idx = 0;
            Heap* h = heapByGpu(a.v, idx);
            if (!h) continue;
            for (auto& rg : q.ranges)
            {
                if (rg.kind == 4) continue;
                for (UINT i = 0; i < rg.num; i++)
                {
                    UINT s = idx + rg.offset + i;
                    if (s >= h->slots.size()) break;
                    const Slot& sl = h->slots[s];
                    if (!declared(pi, rg.kind, rg.baseReg + i, rg.space)) continue;
                    out.push_back({rg.kind, rg.baseReg + i, rg.space, sl.res, sl.viewFmt, sl.cbvVA, sl.cbvSize});
                }
            }
        }
        else if (q.type == D3D12_ROOT_PARAMETER_TYPE_CBV && a.type == 2 && declared(pi, 3, q.reg, q.space))
            out.push_back({3, q.reg, q.space, nullptr, DXGI_FORMAT_UNKNOWN, a.v, 256});
        else if (q.type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS && a.type == 3)
            consts.push_back({q.reg, &a});
    }
}

std::string resStr(ID3D12Resource* r, DXGI_FORMAT viewFmt)
{
    if (!r) return "null";
    auto d = r->GetDesc();
    char b[160];
    if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        snprintf(b, sizeof(b), "buffer %llu B", (unsigned long long) d.Width);
    else
        snprintf(b, sizeof(b), "%llux%u%s fmt %d view %d mips %u flags 0x%x", (unsigned long long) d.Width, d.Height,
                 d.DepthOrArraySize > 1 ? "xN" : "", (int) d.Format, (int) viewFmt, d.MipLevels, (unsigned) d.Flags);
    return b;
}

void logCb(const char* tag, const uint8_t* p, UINT n)
{
    n = n > 256 ? 256 : n;
    const int32_t* ii = (const int32_t*) p;
    const float* f = (const float*) p;
    for (UINT o = 0; o + 16 <= n; o += 32)
    {
        char line[400];
        int k = snprintf(line, sizeof(line), "    %s +%3u:", tag, o);
        for (UINT j = o / 4; j < (o + 32) / 4 && j * 4 + 4 <= n; j++)
            k += snprintf(line + k, sizeof(line) - k, " %d|%.6g", ii[j], f[j]);
        if (logOk()) LOGI("%s", line);
    }
}

// ---- one-time dump (bench-like layout: <dir>\p<pso>.json + p<pso>_<t|u><reg>.bin) ------------------
const char* kDumpRoot = "C:\\sgsr3intercept";
const char* kDumpDir = "C:\\sgsr3intercept\\fsr2dump";
std::set<ID3D12Resource*> g_grabbed;
uint32_t texelBytes(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_TYPELESS: return 16;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_TYPELESS: return 8;
    case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_TYPELESS: case DXGI_FORMAT_R16_UINT: return 2;
    case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UINT: return 1;
    default: return 4;
    }
}
struct DumpRes
{
    ID3D12Resource* rb = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    uint32_t w = 0, h = 0, rowBytes = 0;
    std::string name, info;
};
struct Pending
{
    std::vector<DumpRes> res;
    std::string base, json;
    ULONGLONG at = 0;
};
std::vector<Pending> g_pending;

ID3D12Resource* makeReadback(UINT64 bytes)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr;
    g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                   IID_PPV_ARGS(&r));
    return r;
}
void barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER rb{};
    rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    rb.Transition.pResource = r;
    rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    rb.Transition.StateBefore = a;
    rb.Transition.StateAfter = b;
    cl->ResourceBarrier(1, &rb);
}
bool isDepthFmt(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_D32_FLOAT || f == DXGI_FORMAT_R32_TYPELESS || f == DXGI_FORMAT_D24_UNORM_S8_UINT ||
           f == DXGI_FORMAT_R24G8_TYPELESS || f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
           f == DXGI_FORMAT_R32G8X24_TYPELESS || f == DXGI_FORMAT_D16_UNORM;
}
void grab(ID3D12GraphicsCommandList* cl, const Binding& b, Pending& p)
{
    if (!b.res || g_grabbed.count(b.res)) return;
    auto d = b.res->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.SampleDesc.Count != 1) return;
    g_grabbed.insert(b.res);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 rowBytes = 0, total = 0;
    g_dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, &rows, &rowBytes, &total);
    DumpRes r;
    r.rb = makeReadback(total);
    if (!r.rb) return;
    r.fp = fp;
    r.w = (uint32_t) d.Width;
    r.h = d.Height;
    r.rowBytes = (uint32_t) rowBytes;
    char nm[32];
    snprintf(nm, sizeof(nm), "%c%u", b.kind == 2 ? 'u' : 't', b.reg);
    r.name = nm;
    char inf[256];
    snprintf(inf, sizeof(inf),
             "{\"size\": [%u, %u], \"dxgi\": %d, \"plane_dxgi\": %d, \"view_dxgi\": %d, \"texel_bytes\": %u, "
             "\"row_bytes\": %u, \"flags\": %u}",
             r.w, r.h, (int) d.Format, (int) d.Format, (int) b.viewFmt, (unsigned) (rowBytes / (d.Width ? d.Width : 1)),
             r.rowBytes, (unsigned) d.Flags);
    r.info = inf;
    D3D12_RESOURCE_STATES st = b.kind == 2 ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                               : isDepthFmt(d.Format)
                                   ? (D3D12_RESOURCE_STATES) (D3D12_RESOURCE_STATE_DEPTH_READ |
                                                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                                   : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    static int noBarrier = -1, noCopy = -1;
    // Barriers are opt-in (RECON_BARRIER=1): we cannot know the game's exact current state, and a wrong
    // StateBefore removed the device on real D3D12 (PC harness: DXGI_ERROR_INVALID_CALL). Inputs are in a
    // shader-readable state at an FSR 2 dispatch, which a copy tolerates on NVIDIA and on vkd3d/Turnip.
    if (noBarrier < 0) { char e[4] = {}; noBarrier = GetEnvironmentVariableA("RECON_BARRIER", e, 4) == 0; noCopy = GetEnvironmentVariableA("RECON_NOCOPY", e, 4) > 0; }
    if (!noBarrier) barrier(cl, b.res, st, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{}, s{};
    dst.pResource = r.rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    s.pResource = b.res;
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s.SubresourceIndex = 0;
    if (!noCopy) cl->CopyTextureRegion(&dst, 0, 0, 0, &s, nullptr);
    if (!noBarrier) barrier(cl, b.res, D3D12_RESOURCE_STATE_COPY_SOURCE, st);
    p.res.push_back(r);
}
void flushDumps(bool force = false)
{
    ULONGLONG now = GetTickCount64();
    for (auto it = g_pending.begin(); it != g_pending.end();)
    {
        if (!force && now < it->at) { ++it; continue; }
        CreateDirectoryA(kDumpRoot, nullptr);
        CreateDirectoryA(kDumpDir, nullptr);
        std::string imgs;
        for (auto& r : it->res)
        {
            uint8_t* src = nullptr;
            HRESULT mr = r.rb->Map(0, nullptr, (void**) &src); // whole buffer is read
            if (FAILED(mr) || !src)
                LOGI("  readback map failed for %s: hr=0x%08x", r.name.c_str(), (unsigned) mr);
            if (SUCCEEDED(mr) && src)
            {
                if (r.h == 1 && !r.name.empty() && r.name[0] == 'b')
                {
                    char tag[48];
                    snprintf(tag, sizeof(tag), "%s.%s", it->base.substr(it->base.find_last_of('\\') + 1).c_str(),
                             r.name.c_str());
                    logCb(tag, src, r.w);
                }
                if (FILE* bf = fopen((it->base + "_" + r.name + ".bin").c_str(), "wb"))
                {
                    for (uint32_t y = 0; y < r.h; y++)
                        fwrite(src + (size_t) y * r.fp.Footprint.RowPitch, 1, r.rowBytes, bf);
                    fclose(bf);
                }
                r.rb->Unmap(0, nullptr);
                imgs += (imgs.empty() ? "" : ", ") + std::string("\"") + r.name + "\": " + r.info;
            }
            r.rb->Release();
        }
        std::string js = it->json;
        auto pos = js.find("@IMAGES@");
        if (pos != std::string::npos) js.replace(pos, 8, "{" + imgs + "}");
        if (FILE* jf = fopen((it->base + ".json").c_str(), "w")) { fputs(js.c_str(), jf); fclose(jf); }
        LOGI("dump written: %s (%zu images)", it->base.c_str(), it->res.size());
        it = g_pending.erase(it);
    }
}

// =================================================================================================
// hooks
// =================================================================================================
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateCPSO)(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID,
                                                   void**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateRS)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateHeap)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**);
typedef void(STDMETHODCALLTYPE* PFN_CreateSRV)(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*,
                                               D3D12_CPU_DESCRIPTOR_HANDLE);
typedef void(STDMETHODCALLTYPE* PFN_CreateUAV)(ID3D12Device*, ID3D12Resource*, ID3D12Resource*,
                                               const D3D12_UNORDERED_ACCESS_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
typedef void(STDMETHODCALLTYPE* PFN_CreateCBV)(ID3D12Device*, const D3D12_CONSTANT_BUFFER_VIEW_DESC*,
                                               D3D12_CPU_DESCRIPTOR_HANDLE);
typedef void(STDMETHODCALLTYPE* PFN_CopyDesc)(ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*,
                                              UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*,
                                              D3D12_DESCRIPTOR_HEAP_TYPE);
typedef void(STDMETHODCALLTYPE* PFN_CopyDescS)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE,
                                               D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Map)(ID3D12Resource*, UINT, const D3D12_RANGE*, void**);
typedef ULONG(STDMETHODCALLTYPE* PFN_Release)(ID3D12Resource*);
typedef void(STDMETHODCALLTYPE* PFN_Unmap)(ID3D12Resource*, UINT, const D3D12_RANGE*);

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateCommitted)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                                        const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                                        const D3D12_CLEAR_VALUE*, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreatePlaced)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*,
                                                     D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
PFN_CreateCommitted o_Committed;
PFN_CreatePlaced o_Placed;
void noteBuffer(const D3D12_RESOURCE_DESC* rd, void** pp)
{
    if (!rd || rd->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || !pp || !*pp) return;
    ID3D12Resource* r = (ID3D12Resource*) *pp; // riid is a resource interface; same object
    UINT64 va = r->GetGPUVirtualAddress();
    if (!va) return;
    Lock g(g_mtx);
    // a new buffer over an old one's VA range: the old one is gone (placed resources reuse heap ranges)
    for (auto it = g_bufs.lower_bound(va > (1ull << 32) ? va - (1ull << 32) : 0); it != g_bufs.end() && it->first < va + rd->Width;)
    {
        if (it->first + it->second.size > va) it = g_bufs.erase(it);
        else ++it;
    }
    g_bufs[va] = BufRes{rd->Width, r};
}
HRESULT STDMETHODCALLTYPE h_Committed(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
                                      const D3D12_RESOURCE_DESC* rd, D3D12_RESOURCE_STATES st,
                                      const D3D12_CLEAR_VALUE* cv, REFIID riid, void** pp)
{
    HRESULT r = o_Committed(d, hp, hf, rd, st, cv, riid, pp);
    if (SUCCEEDED(r) && pp && *pp && g_check > 0) { Lock g(g_mtx); g_freedSet.erase((ID3D12Resource*) *pp); }
    if (SUCCEEDED(r)) noteBuffer(rd, pp);
    return r;
}
HRESULT STDMETHODCALLTYPE h_Placed(ID3D12Device* d, ID3D12Heap* heap, UINT64 off, const D3D12_RESOURCE_DESC* rd,
                                   D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID riid, void** pp)
{
    HRESULT r = o_Placed(d, heap, off, rd, st, cv, riid, pp);
    if (SUCCEEDED(r) && pp && *pp && g_check > 0) { Lock g(g_mtx); g_freedSet.erase((ID3D12Resource*) *pp); }
    if (SUCCEEDED(r)) noteBuffer(rd, pp);
    return r;
}
PFN_CreateCPSO o_CPSO;
PFN_CreateRS o_RS;
PFN_CreateHeap o_Heap;
PFN_CreateSRV o_SRV;
PFN_CreateUAV o_UAV;
PFN_CreateCBV o_CBV;
PFN_CopyDesc o_Copy;
PFN_CopyDescS o_CopyS;
PFN_Map o_Map;
PFN_Release o_ResRelease = nullptr, o_TexRelease = nullptr;

int g_noPurge = -1; // RECON_NOPURGE=1: negative control for the lifetime test (old behaviour: keep freed entries)
bool noPurge()
{
    if (g_noPurge < 0) { char e[4] = {}; g_noPurge = GetEnvironmentVariableA("RECON_NOPURGE", e, 4) > 0 ? 1 : 0; }
    return g_noPurge > 0;
}
void forgetResource(ID3D12Resource* r)
{
    if (noPurge()) { if (g_check > 0) g_freedSet.insert(r); g_freedTracked++; return; }
    // called under g_mtx with the resource already destroyed: only compare pointers
    for (auto it = g_bufs.begin(); it != g_bufs.end();)
        if (it->second.res == r) it = g_bufs.erase(it); else ++it;
    for (auto it = g_copied.begin(); it != g_copied.end();)
        if (it->second.dst == r) it = g_copied.erase(it); else ++it;
    for (auto it = g_mapped.begin(); it != g_mapped.end();)
        if (it->second.res == r) it = g_mapped.erase(it); else ++it;
    VelaOnResourceFreed(r);
    g_freedTracked++;
    if (g_check > 0) g_freedSet.insert(r);
}
// The public reference count reaching 0 = the game freed it (vkd3d-proton destroys it then). Pointers to it in our
// tables would otherwise outlive it: a level load frees and recreates the render targets and constant buffers.
ULONG STDMETHODCALLTYPE h_ResRelease(ID3D12Resource* r)
{
    ULONG n = o_ResRelease(r);
    if (n == 0) { Lock g(g_mtx); forgetResource(r); }
    return n;
}
ULONG STDMETHODCALLTYPE h_TexRelease(ID3D12Resource* r)
{
    ULONG n = o_TexRelease(r);
    if (n == 0) { Lock g(g_mtx); forgetResource(r); }
    return n;
}
PFN_Unmap o_Unmap;

// Compute pipelines reach us four ways, RE4: after the player's exit RE4 rebuilt its pipeline
// cache and 1439 of 1441 compute PSOs came from ID3D12PipelineLibrary::LoadComputePipeline -- with only
// CreateComputePipelineState hooked, FSR 2 was not recognised and Vela drew 0 frames). The identity is the same on
// every path: the DXIL container's hash of the CS bytecode (a library load must be given the identical bytecode).
int g_psoVia[4] = {}; // CreateComputePipelineState, CreatePipelineState (stream), library LoadComputePipeline, LoadPipeline
uint64_t g_eiSkipped = 0, g_eiCalls = 0; // ExecuteIndirect calls / skipped (VELA_SKIP_SDF, VELA_SKIP_EI)
void noteComputePso(ID3D12PipelineState* pso, const void* bc, size_t n, int via)
{
    Lock g(g_mtx);
    g_psoVia[via]++;
    PsoInfo pi;
    pi.id = g_nextPso++;
    pi.bytes = n;
    pi.hash = dxil::hashHex(bc, n);
    auto ss = dxil::strings(bc, n, 5);
    pi.nameHits = fsr2NameHits(ss);
    pi.names = fsr2Names(ss);
    pi.binds = dxil::psvBindings(bc, n, &pi.psvOk);
    g_pso[pso] = pi;
    static int logAll = -1;
    if (logAll < 0) { char e[4] = {}; logAll = GetEnvironmentVariableA("RECON_LOGPSO", e, 4) > 0 ? 1 : 0; }
    static const char* viaName[] = {"CreateComputePipelineState", "CreatePipelineState", "LoadComputePipeline", "LoadPipeline"};
    if ((pi.nameHits >= 1 || logAll) && logOk())
        LOGI("CPSO #%d via %s hash=%s bytes=%zu fsr2NameHits=%d names: %s", pi.id, viaName[via], pi.hash.c_str(),
             pi.bytes, pi.nameHits, pi.names.c_str());
}

HRESULT STDMETHODCALLTYPE h_CPSO(ID3D12Device* d, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID riid,
                                 void** pp)
{
    HRESULT r = o_CPSO(d, desc, riid, pp);
    if (SUCCEEDED(r) && pp && *pp && desc && desc->CS.pShaderBytecode)
        noteComputePso((ID3D12PipelineState*) *pp, desc->CS.pShaderBytecode, desc->CS.BytecodeLength, 0);
    return r;
}

// The CS of a pipeline state stream (ID3D12Device2::CreatePipelineState, ID3D12PipelineLibrary1::LoadPipeline):
// subobjects are pointer-aligned {type, payload}; walk past the ones a stream can carry, stop at the CS.
const D3D12_SHADER_BYTECODE* streamCS(const D3D12_PIPELINE_STATE_STREAM_DESC* sd)
{
    if (!sd || !sd->pPipelineStateSubobjectStream) return nullptr;
    const uint8_t* b = (const uint8_t*) sd->pPipelineStateSubobjectStream;
    size_t o = 0;
    while (o + 8 <= sd->SizeInBytes)
    {
        UINT t = *(const UINT*) (b + o);
        size_t pay, al = 8; // payload size and alignment
        switch (t)
        {
        case 0: pay = 8; break;                                                           // root signature
        case 1: case 2: case 3: case 4: case 5: case 6: case 24: case 25: pay = 16; break; // shaders (6 = CS)
        case 7: pay = sizeof(D3D12_STREAM_OUTPUT_DESC); break;
        case 8: pay = sizeof(D3D12_BLEND_DESC); al = 4; break;
        case 9: pay = 4; al = 4; break;
        case 10: pay = sizeof(D3D12_RASTERIZER_DESC); al = 4; break;
        case 11: pay = sizeof(D3D12_DEPTH_STENCIL_DESC); al = 4; break;
        case 12: pay = sizeof(D3D12_INPUT_LAYOUT_DESC); break;
        case 13: case 14: case 16: case 18: case 20: pay = 4; al = 4; break;
        case 15: pay = sizeof(D3D12_RT_FORMAT_ARRAY); al = 4; break;
        case 17: pay = sizeof(DXGI_SAMPLE_DESC); al = 4; break;
        case 19: pay = sizeof(D3D12_CACHED_PIPELINE_STATE); break;
        case 21: pay = sizeof(D3D12_DEPTH_STENCIL_DESC1); al = 4; break;
        case 22: pay = sizeof(D3D12_VIEW_INSTANCING_DESC); break;
        default: return nullptr; // unknown subobject: give up rather than misparse
        }
        size_t po = (o + 4 + al - 1) / al * al;
        if (t == 6) return (const D3D12_SHADER_BYTECODE*) (b + po);
        o = (po + pay + 7) / 8 * 8;
    }
    return nullptr;
}
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreatePS)(ID3D12Device*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_LoadC)(ID3D12PipelineLibrary*, LPCWSTR, const D3D12_COMPUTE_PIPELINE_STATE_DESC*,
                                              REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_LoadP)(ID3D12PipelineLibrary*, LPCWSTR, const D3D12_PIPELINE_STATE_STREAM_DESC*,
                                              REFIID, void**);
PFN_CreatePS o_CreatePS = nullptr;
PFN_LoadC o_LoadC = nullptr;
PFN_LoadP o_LoadP = nullptr;
HRESULT STDMETHODCALLTYPE h_CreatePS(ID3D12Device* d, const D3D12_PIPELINE_STATE_STREAM_DESC* sd, REFIID riid, void** pp)
{
    HRESULT r = o_CreatePS(d, sd, riid, pp);
    if (SUCCEEDED(r) && pp && *pp)
        if (auto cs = streamCS(sd))
            if (cs->pShaderBytecode && cs->BytecodeLength)
                noteComputePso((ID3D12PipelineState*) *pp, cs->pShaderBytecode, cs->BytecodeLength, 1);
    return r;
}
HRESULT STDMETHODCALLTYPE h_LoadC(ID3D12PipelineLibrary* l, LPCWSTR n, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
                                  REFIID riid, void** pp)
{
    HRESULT r = o_LoadC(l, n, desc, riid, pp);
    if (SUCCEEDED(r) && pp && *pp && desc && desc->CS.pShaderBytecode)
        noteComputePso((ID3D12PipelineState*) *pp, desc->CS.pShaderBytecode, desc->CS.BytecodeLength, 2);
    return r;
}
HRESULT STDMETHODCALLTYPE h_LoadP(ID3D12PipelineLibrary* l, LPCWSTR n, const D3D12_PIPELINE_STATE_STREAM_DESC* sd,
                                  REFIID riid, void** pp)
{
    HRESULT r = o_LoadP(l, n, sd, riid, pp);
    if (SUCCEEDED(r) && pp && *pp)
        if (auto cs = streamCS(sd))
            if (cs->pShaderBytecode && cs->BytecodeLength)
                noteComputePso((ID3D12PipelineState*) *pp, cs->pShaderBytecode, cs->BytecodeLength, 3);
    return r;
}

HRESULT STDMETHODCALLTYPE h_RS(ID3D12Device* d, UINT mask, const void* blob, SIZE_T size, REFIID riid, void** pp)
{
    HRESULT r = o_RS(d, mask, blob, size, riid, pp);
    if (SUCCEEDED(r) && pp && *pp && blob)
    {
        RootSig rs;
        if (parseRootSig(blob, size, rs))
        {
            Lock g(g_mtx);
            g_rs[(ID3D12RootSignature*) *pp] = std::move(rs);
        }
    }
    return r;
}

HRESULT STDMETHODCALLTYPE h_Heap(ID3D12Device* d, const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID riid, void** pp)
{
    HRESULT r = o_Heap(d, desc, riid, pp);
    if (SUCCEEDED(r) && pp && *pp && desc && desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
    {
        ID3D12DescriptorHeap* heap = (ID3D12DescriptorHeap*) *pp; // riid is the heap interface
        Lock g(g_mtx);
        Heap h;
        h.cpu0 = heap->GetCPUDescriptorHandleForHeapStart().ptr;
        if (desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
            h.gpu0 = heap->GetGPUDescriptorHandleForHeapStart().ptr;
        h.inc = g_inc;
        h.count = desc->NumDescriptors;
        h.slots.resize(desc->NumDescriptors);
        // a new heap over (part of) an old one's CPU or GPU range: the old heap is gone, drop its mirror
        const SIZE_T c0 = h.cpu0, c1 = h.cpu0 + (SIZE_T) h.count * h.inc;
        const UINT64 g0 = h.gpu0, g1 = h.gpu0 + (UINT64) h.count * h.inc;
        for (auto it = g_heaps.begin(); it != g_heaps.end();)
        {
            const Heap& o = it->second;
            const SIZE_T oc1 = o.cpu0 + (SIZE_T) o.count * o.inc;
            const UINT64 og1 = o.gpu0 + (UINT64) o.count * o.inc;
            const bool cpuOv = o.cpu0 < c1 && c0 < oc1;
            const bool gpuOv = g0 && o.gpu0 && o.gpu0 < g1 && g0 < og1;
            if ((cpuOv || gpuOv) && !noPurge()) { g_staleHeaps++; it = g_heaps.erase(it); }
            else ++it;
        }
        g_heaps[h.cpu0] = std::move(h);
    }
    return r;
}

void STDMETHODCALLTYPE h_SRV(ID3D12Device* d, ID3D12Resource* res, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
                             D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    o_SRV(d, res, desc, h);
    Lock g(g_mtx);
    if (Slot* s = slotByCpu(h.ptr)) *s = Slot{1, res, desc ? desc->Format : DXGI_FORMAT_UNKNOWN, 0, 0};
}
void STDMETHODCALLTYPE h_UAV(ID3D12Device* d, ID3D12Resource* res, ID3D12Resource* counter,
                             const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    o_UAV(d, res, counter, desc, h);
    Lock g(g_mtx);
    if (Slot* s = slotByCpu(h.ptr)) *s = Slot{2, res, desc ? desc->Format : DXGI_FORMAT_UNKNOWN, 0, 0};
}
// RECON_VACHECK (below, at the command-list hooks)
int g_vaCheck = -1;
enum VaKind { VA_CCBV, VA_GCBV, VA_CSRV, VA_GSRV, VA_CUAV, VA_GUAV, VA_IB, VA_VB, VA_CBVDESC, VA_N };
void vaNote(VaKind k, ID3D12GraphicsCommandList* cl, UINT slot, UINT64 va);
void STDMETHODCALLTYPE h_CBV(ID3D12Device* d, const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    o_CBV(d, desc, h);
    Lock g(g_mtx);
    if (Slot* s = slotByCpu(h.ptr))
        *s = Slot{3, nullptr, DXGI_FORMAT_UNKNOWN, desc ? desc->BufferLocation : 0, desc ? desc->SizeInBytes : 0};
    if (g_vaCheck > 0 && desc) vaNote(VA_CBVDESC, nullptr, 0, desc->BufferLocation);
}
void STDMETHODCALLTYPE h_CopyS(ID3D12Device* d, UINT n, D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src,
                               D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    o_CopyS(d, n, dst, src, type);
    if (type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) return;
    Lock g(g_mtx);
    for (UINT i = 0; i < n; i++)
    {
        Slot* s = slotByCpu(src.ptr + (SIZE_T) i * g_inc);
        Slot* t = slotByCpu(dst.ptr + (SIZE_T) i * g_inc);
        if (t) *t = s ? *s : Slot{};
    }
}
void STDMETHODCALLTYPE h_Copy(ID3D12Device* d, UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE* ds, const UINT* dsz, UINT ns,
                              const D3D12_CPU_DESCRIPTOR_HANDLE* ss, const UINT* ssz, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    o_Copy(d, nd, ds, dsz, ns, ss, ssz, type);
    if (type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) return;
    Lock g(g_mtx);
    UINT di = 0, dk = 0, si = 0, sk = 0;
    while (di < nd && si < ns)
    {
        UINT dn = dsz ? dsz[di] : 1, sn = ssz ? ssz[si] : 1;
        Slot* s = slotByCpu(ss[si].ptr + (SIZE_T) sk * g_inc);
        Slot* t = slotByCpu(ds[di].ptr + (SIZE_T) dk * g_inc);
        if (t) *t = s ? *s : Slot{};
        if (++dk >= dn) { dk = 0; di++; }
        if (++sk >= sn) { sk = 0; si++; }
    }
}
HRESULT STDMETHODCALLTYPE h_Map(ID3D12Resource* res, UINT sub, const D3D12_RANGE* range, void** pp)
{
    HRESULT r = o_Map(res, sub, range, pp);
    if (SUCCEEDED(r) && pp && *pp && sub == 0)
    {
        auto d = res->GetDesc();
        if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        {
            Lock g(g_mtx);
            g_mapped[res->GetGPUVirtualAddress()] = Mapped{d.Width, (uint8_t*) *pp, res};
        }
    }
    return r;
}
void STDMETHODCALLTYPE h_Unmap(ID3D12Resource* res, UINT sub, const D3D12_RANGE* range)
{
    {
        Lock g(g_mtx);
        if (sub == 0)
            for (auto it = g_mapped.begin(); it != g_mapped.end(); ++it)
                if (it->second.res == res) { g_mapped.erase(it); break; }
    }
    o_Unmap(res, sub, range);
}

// ---- command list ---------------------------------------------------------------------------------
typedef void(STDMETHODCALLTYPE* PFN_SetPSO)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
typedef void(STDMETHODCALLTYPE* PFN_SetCRS)(ID3D12GraphicsCommandList*, ID3D12RootSignature*);
typedef void(STDMETHODCALLTYPE* PFN_SetHeaps)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);
typedef void(STDMETHODCALLTYPE* PFN_SetCTable)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
typedef void(STDMETHODCALLTYPE* PFN_SetCCBV)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
typedef void(STDMETHODCALLTYPE* PFN_SetC32)(ID3D12GraphicsCommandList*, UINT, UINT, const void*, UINT);
typedef void(STDMETHODCALLTYPE* PFN_SetC1)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
typedef void(STDMETHODCALLTYPE* PFN_Dispatch)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
typedef void(STDMETHODCALLTYPE* PFN_CopyBuf)(ID3D12GraphicsCommandList*, ID3D12Resource*, UINT64, ID3D12Resource*,
                                             UINT64, UINT64);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Reset)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*,
                                              ID3D12PipelineState*);

PFN_SetPSO o_SetPSO;
PFN_SetCRS o_SetCRS;
PFN_SetHeaps o_SetHeaps;
PFN_SetCTable o_SetCTable;
PFN_SetCCBV o_SetCCBV;
PFN_SetC32 o_SetC32;
PFN_SetC1 o_SetC1;
PFN_Dispatch o_Dispatch;
PFN_Reset o_Reset;
PFN_CopyBuf o_CopyBuf;

void STDMETHODCALLTYPE h_CopyBuf(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst, UINT64 dOff, ID3D12Resource* src,
                                 UINT64 sOff, UINT64 n)
{
    o_CopyBuf(cl, dst, dOff, src, sOff, n);
    if (!dst || !src || !n || n > (1u << 20)) return; // constants are small; skip bulk uploads
    Lock g(g_mtx);
    UINT64 dva = dst->GetGPUVirtualAddress() + dOff;
    g_copied[dva] = Copied{n, dst, dOff, src->GetGPUVirtualAddress() + sOff};
    if (g_copied.size() > 20000) g_copied.clear(); // bounded (purged per freed resource); refilled every frame
}

void STDMETHODCALLTYPE h_SetPSO(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* pso)
{
    o_SetPSO(cl, pso);
    if (Ext::OwnWork()) return; // FSR 3.1's own state is not the game's (RestoreGameState)
    Lock g(g_mtx);
    g_cl[cl].pso = pso;
}
void STDMETHODCALLTYPE h_SetHeaps(ID3D12GraphicsCommandList* cl, UINT n, ID3D12DescriptorHeap* const* h)
{
    o_SetHeaps(cl, n, h);
    if (Ext::OwnWork()) return; // FSR 3.1's own state is not the game's (RestoreGameState)
    Lock g(g_mtx);
    auto& st = g_cl[cl];
    st.nHeaps = (h && n <= 2) ? n : 0;
    for (UINT k = 0; k < 2; k++) st.heaps[k] = k < st.nHeaps ? h[k] : nullptr;
}
void STDMETHODCALLTYPE h_SetCRS(ID3D12GraphicsCommandList* cl, ID3D12RootSignature* rs)
{
    o_SetCRS(cl, rs);
    if (Ext::OwnWork()) return; // FSR 3.1's own state is not the game's (RestoreGameState)
    Lock g(g_mtx);
    auto& st = g_cl[cl];
    if (st.rs != rs)
        for (auto& a : st.root) a.type = 0;
    st.rs = rs;
}
void STDMETHODCALLTYPE h_SetCTable(ID3D12GraphicsCommandList* cl, UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h)
{
    o_SetCTable(cl, i, h);
    if (Ext::OwnWork()) return; // FSR 3.1's own state is not the game's (RestoreGameState)
    Lock g(g_mtx);
    if (i < 64) { auto& a = g_cl[cl].root[i]; a.type = 1; a.v = h.ptr; }
}
void STDMETHODCALLTYPE h_SetCCBV(ID3D12GraphicsCommandList* cl, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va)
{
    o_SetCCBV(cl, i, va);
    if (Ext::OwnWork()) return; // FSR 3.1's own state is not the game's (RestoreGameState)
    Lock g(g_mtx);
    if (i < 64) { auto& a = g_cl[cl].root[i]; a.type = 2; a.v = va; }
    if (g_vaCheck > 0) vaNote(VA_CCBV, cl, i, va);
}
void STDMETHODCALLTYPE h_SetC32(ID3D12GraphicsCommandList* cl, UINT i, UINT n, const void* src, UINT off)
{
    o_SetC32(cl, i, n, src, off);
    if (Ext::OwnWork()) return; // FSR 3.1's own state is not the game's (RestoreGameState)
    Lock g(g_mtx);
    if (i < 64 && src)
    {
        auto& a = g_cl[cl].root[i];
        a.type = 3;
        for (UINT k = 0; k < n && off + k < 64; k++) a.c[off + k] = ((const uint32_t*) src)[k];
        if (off + n > a.nc) a.nc = off + n > 64 ? 64 : off + n;
    }
}
void STDMETHODCALLTYPE h_SetC1(ID3D12GraphicsCommandList* cl, UINT i, UINT v, UINT off)
{
    o_SetC1(cl, i, v, off);
    if (Ext::OwnWork()) return; // FSR 3.1's own state is not the game's (RestoreGameState)
    Lock g(g_mtx);
    if (i < 64 && off < 64)
    {
        auto& a = g_cl[cl].root[i];
        a.type = 3;
        a.c[off] = v;
        if (off + 1 > a.nc) a.nc = off + 1;
    }
}
HRESULT STDMETHODCALLTYPE h_Reset(ID3D12GraphicsCommandList* cl, ID3D12CommandAllocator* a, ID3D12PipelineState* p)
{
    {
        Lock g(g_mtx);
        auto& st = g_cl[cl];
        st.pso = p;
        st.rs = nullptr;
        for (auto& r : st.root) r.type = 0;
        st.ring.clear();
        st.afterAcc = false;
        st.nHeaps = 0;
        st.heaps[0] = st.heaps[1] = nullptr;
    }
    return o_Reset(cl, a, p);
}

void logPass(ID3D12GraphicsCommandList* cl, CLState& st, PsoInfo& pi, UINT x, UINT y, UINT z)
{
    std::vector<Binding> bs;
    std::vector<std::pair<UINT, const RootArg*>> cs;
    bindingsOf(st, bs, cs);
    if (logOk())
        LOGI("PASS pso#%d%s hash=%s grp=%ux%ux%u bindings=%zu (PSV0 %s, %zu ranges) rs=%p names: %s", pi.id,
             pi.accumulate ? " (ACCUMULATE)" : "", pi.hash.c_str(), x, y, z, bs.size(), pi.psvOk ? "ok" : "absent",
             pi.binds.size(), (void*) st.rs, pi.names.c_str());
    for (auto& b : bs)
    {
        if (b.kind == 3)
        {
            if (logOk()) LOGI("  b%u space%u CBV va=%p size=%u", b.reg, b.space, (void*) b.cbvVA, b.cbvSize);
            UINT n = b.cbvSize ? (b.cbvSize < 256 ? b.cbvSize : 256) : 256;
            const uint8_t* p = cbBytes(b.cbvVA, n);
            if (!p) { n = 64; p = cbBytes(b.cbvVA, n); }
            if (p)
            {
                char tag[16];
                snprintf(tag, sizeof(tag), "b%u", b.reg);
                logCb(tag, p, n);
            }
            else if (logOk())
            {
                UINT64 base = 0;
                const Copied* c = copiedAt(b.cbvVA, base);
                UINT64 boff = 0;
                ID3D12Resource* br = bufAt(b.cbvVA, boff);
                LOGI("    (CBV not CPU-readable: %zu mapped, %zu copy targets, %zu buffers; filled by copy: %s; owner buffer: %s"
                     " -> GPU readback in the dump)",
                     g_mapped.size(), g_copied.size(), g_bufs.size(), c ? "yes" : "no", br ? "found" : "not found");
            }
        }
        else if (logOk())
            LOGI("  %c%u space%u %s %s", b.kind == 2 ? 'u' : 't', b.reg, b.space, b.kind == 2 ? "UAV" : "SRV",
                 resStr(b.res, b.viewFmt).c_str());
    }
    for (auto& c : cs)
    {
        char tag[16];
        snprintf(tag, sizeof(tag), "rc%u", c.first);
        logCb(tag, (const uint8_t*) c.second->c, c.second->nc * 4);
    }
}

void dumpPass(ID3D12GraphicsCommandList* cl, CLState& st, PsoInfo& pi)
{
    std::vector<Binding> bs;
    std::vector<std::pair<UINT, const RootArg*>> cs;
    bindingsOf(st, bs, cs);
    Pending p;
    char base[128];
    snprintf(base, sizeof(base), "%s\\c%02d_p%04d", kDumpDir, g_capIndex, pi.id);
    p.base = base;
    p.at = GetTickCount64() + 3000;
    for (auto& b : bs)
        if (b.kind == 1 || b.kind == 2) grab(cl, b, p);
    std::string cb;
    for (auto& b : bs)
        if (b.kind == 3)
        {
            // GPU copy of the constant buffer as well (works when the CPU source is gone or unknown)
            UINT64 cbase = 0, boff = 0;
            ID3D12Resource* cres = nullptr;
            if (const Copied* c = copiedAt(b.cbvVA, cbase)) { cres = c->dst; boff = c->dstOff + (b.cbvVA - cbase); }
            else cres = bufAt(b.cbvVA, boff);
            if (cres)
                if (ID3D12Resource* rb = makeReadback(256))
                {
                    UINT64 off = boff;
                    UINT64 avail = cres->GetDesc().Width - off;
                    UINT n = (UINT) (avail < 256 ? avail : 256);
                    cl->CopyBufferRegion(rb, 0, cres, off, n);
                    DumpRes r;
                    r.rb = rb;
                    r.w = n;
                    r.h = 1;
                    r.rowBytes = n;
                    r.fp.Footprint.RowPitch = n;
                    r.name = "b" + std::to_string(b.reg);
                    r.info = "{\"size\": [" + std::to_string(n) + ", 1], \"dxgi\": 0, \"texel_bytes\": 1, \"buffer\": 1}";
                    p.res.push_back(r);
                }
            UINT n = b.cbvSize ? (b.cbvSize < 256 ? b.cbvSize : 256) : 256;
            const uint8_t* q = cbBytes(b.cbvVA, n);
            if (!q) { n = 64; q = cbBytes(b.cbvVA, n); }
            if (q)
            {
                char h[8];
                cb += (cb.empty() ? "" : ",") + std::string("\"b") + std::to_string(b.reg) + "\": \"";
                for (UINT i = 0; i < n; i++) { snprintf(h, sizeof(h), "%02x", q[i]); cb += h; }
                cb += "\"";
            }
        }
    char j[512];
    snprintf(j, sizeof(j), "{\n  \"pso\": %d,\n  \"pso_hash\": \"%s\",\n  \"accumulate\": %d,\n  \"dispatch\": %llu,\n",
             pi.id, pi.hash.c_str(), pi.accumulate ? 1 : 0, (unsigned long long) g_dispatch);
    p.json = std::string(j) + "  \"cbuffers\": {" + cb + "},\n  \"images\": @IMAGES@\n}\n";
    if (logOk()) LOGI("queued dump of pso#%d: %zu textures", pi.id, p.res.size());
    g_pending.push_back(std::move(p));
}

} // namespace
void ReconOnPresent();
namespace
{
void STDMETHODCALLTYPE h_Dispatch(ID3D12GraphicsCommandList* cl, UINT x, UINT y, UINT z)
{
    if (Ext::OwnWork()) { o_Dispatch(cl, x, y, z); return; } // FSR 3.1's own passes in place of FSR 2
    bool callOrig = true, velaPass = false, velaAcc = false, postPass = false, accPso = false;
    {
        Lock g(g_mtx);
        g_dispatch++;
        flushDumps();
        auto& st = g_cl[cl];
        auto pit = st.pso ? g_pso.find(st.pso) : g_pso.end();
        if (pit != g_pso.end())
        {
            PsoInfo& pi = pit->second;
            if (g_check < 0) { char e[4] = {}; g_check = GetEnvironmentVariableA("RECON_CHECK", e, 4) > 0 ? 1 : 0; }
            if (g_check > 0 && pi.fsrSeq)
            {
                std::vector<Binding> bs;
                std::vector<std::pair<UINT, const RootArg*>> cs;
                bindingsOf(st, bs, cs);
                static uint64_t checked = 0, stale = 0;
                for (auto& b : bs)
                    if (b.res)
                    {
                        checked++;
                        if (g_freedSet.count(b.res) && stale++ < 20)
                            LOGI("STALE binding: pso#%d %c%u -> freed resource %p", pi.id, b.kind == 2 ? 'u' : 't', b.reg,
                                 (void*) b.res);
                    }
                static ULONGLONG t = 0;
                if (GetTickCount64() - t > 3000)
                {
                    t = GetTickCount64();
                    LOGI("check: %llu bindings checked, %llu stale; freed %llu, stale heaps dropped %llu, buffers %zu",
                         (unsigned long long) checked, (unsigned long long) stale, (unsigned long long) g_freedTracked,
                         (unsigned long long) g_staleHeaps, g_bufs.size());
                }
            }
            // Learn the sequence once, from the first accumulate-like pass: FSR 2 names in the shader and
            // a dispatch over display-size tiles. Its predecessors on this list are the other FSR 2 passes.
            // Every FSR-named pipeline is an accumulate variant (RE4 has one for the first frames after a reset
            // and one for steady state; PSO numbers shift between runs, hashes do not). Learn from each.
            if (!pi.accumulate && pi.nameHits >= 2)
            {
                g_seqLearned = true;
                pi.accumulate = true;
                pi.fsrSeq = true;
                if (logOk()) LOGI("SEQUENCE before accumulate pso#%d hash=%s (last %zu dispatches on this list):", pi.id,
                                  pi.hash.c_str(), st.ring.size());
                int back = 0;
                for (auto it = st.ring.rbegin(); it != st.ring.rend() && back < 12; ++it, ++back)
                {
                    auto q = g_pso.find(it->pso);
                    if (q == g_pso.end()) continue;
                    if (logOk())
                        LOGI("  -%d pso#%d hash=%s grp=%ux%ux%u hits=%d %s", back + 1, q->second.id, q->second.hash.c_str(),
                             it->x, it->y, it->z, q->second.nameHits, q->second.names.c_str());
                    // FSR 2 passes before accumulate: reconstruct+dilate, depth clip, lock (and luma pyramid).
                    // Take the contiguous run of dispatches right before accumulate.
                    if (back < 6) q->second.fsrSeq = true;
                }
            }
            accPso = pi.accumulate;
            if (pi.fsrSeq && !pi.logged)
            {
                pi.logged = true;
                logPass(cl, st, pi, x, y, z);
            }
            if (pi.fsrSeq && g_level >= 2)
            {
                ULONGLONG now = GetTickCount64();
                if (pi.accumulate && !g_firstAcc) g_firstAcc = now;
                if (g_cap == 0 && now - g_lastPoll > 500)
                {
                    g_lastPoll = now;
                    static char trig[MAX_PATH] = {};
                    static int delay = -1;
                    if (delay < 0)
                    {
                        if (!GetEnvironmentVariableA("RECON_TRIGGER", trig, MAX_PATH))
                            strcpy(trig, "C:\\sgsr3intercept\\dump_now");
                        char d[16] = {};
                        delay = GetEnvironmentVariableA("RECON_DUMP_DELAY", d, 16) ? atoi(d) : 0;
                    }
                    bool fileHit = GetFileAttributesA(trig) != INVALID_FILE_ATTRIBUTES;
                    bool timeHit = delay > 0 && g_capIndex == 0 && g_firstAcc && now - g_firstAcc > (ULONGLONG) delay * 1000;
                    if (fileHit || timeHit)
                    {
                        if (fileHit) DeleteFileA(trig);
                        g_cap = 1;
                        if (VelaEnabled()) VelaRequestCapture(g_capIndex); // shadow mode: FSR's and Vela's results too
                        if (logOk()) LOGI("capture c%02d armed (%s)", g_capIndex, fileHit ? "trigger file" : "delay");
                    }
                }
                if (pi.accumulate && g_cap == 1)
                {
                    g_cap = 2; // the next frame's passes, up to and including its accumulate
                    g_capDone.clear();
                    g_grabbed.clear();
                }
                else if (g_cap == 2 && !g_capDone.count(pi.id))
                {
                    g_capDone.insert(pi.id);
                    logPass(cl, st, pi, x, y, z);
                    if (!VelaEnabled()) dumpPass(cl, st, pi);
                    if (pi.accumulate)
                    {
                        if (logOk()) LOGI("capture c%02d complete: %zu passes in one frame", g_capIndex, g_capDone.size());
                        g_cap = 0;
                        g_capIndex++;
                    }
                }
            }
            // census: which compute passes run, every 10 s (FSR-named or display-grid ones), to see variants
            {
                static std::map<int, std::pair<uint64_t, uint64_t>> census; // id -> (count, x|y<<32)
                static ULONGLONG tick = 0;
                static int rounds = 0;
                auto& e = census[pi.id];
                e.first++;
                e.second = (uint64_t) x | ((uint64_t) y << 32);
                ULONGLONG now = GetTickCount64();
                if (!tick) tick = now;
                if (now - tick > 10000 && rounds < 30)
                {
                    tick = now;
                    rounds++;
                    std::string line;
                    for (auto& kv : census)
                    {
                        PsoInfo* q = nullptr;
                        for (auto& pp : g_pso) if (pp.second.id == kv.first) { q = &pp.second; break; }
                        uint32_t gx = (uint32_t) kv.second.second, gy = (uint32_t) (kv.second.second >> 32);
                        if (!q || !(q->nameHits >= 1 || q->fsrSeq || (uint64_t) gx * gy >= 20000)) continue;
                        char t[96];
                        snprintf(t, sizeof(t), " #%d%s x%llu %ux%u h%d;", kv.first, q->fsrSeq ? "*" : "",
                                 (unsigned long long) kv.second.first, gx, gy, q->nameHits);
                        if (line.size() < 900) line += t;
                    }
                    if (logOk()) LOGI("census 10s:%s  (compute PSOs via CPSO/stream/libC/libP %d/%d/%d/%d; EI %llu, skipped %llu)",
                                      line.c_str(), g_psoVia[0], g_psoVia[1], g_psoVia[2], g_psoVia[3],
                                      (unsigned long long) g_eiCalls, (unsigned long long) g_eiSkipped);
                    census.clear();
                }
            }
            // Vela decides whether this FSR pass runs (and runs itself at the accumulate)
            if (pi.fsrSeq && VelaEnabled())
            {
                std::vector<Binding> bs;
                std::vector<std::pair<UINT, const RootArg*>> cs;
                bindingsOf(st, bs, cs);
                std::vector<VelaTex> vt;
                UINT64 cb = 0;
                for (auto& b : bs)
                {
                    if (b.kind == 3) { if (!cb) cb = b.cbvVA; }
                    else vt.push_back({b.kind, b.reg, b.res});
                }
                const RootArg* rc = nullptr; // the largest root-constant block (cbFSR2 as root constants: DRG)
                for (auto& c : cs)
                    if (!rc || c.second->nc > rc->nc) rc = c.second;
                velaPass = true;
                velaAcc = pi.accumulate;
                callOrig = VelaOnFsrPass(cl, pi.hash, pi.accumulate, vt, cb, rc && rc->nc >= 32 ? (const uint8_t*) rc->c : nullptr,
                                         rc ? rc->nc * 4 : 0);
                if (pi.accumulate) st.afterAcc = true;
            }
            else if (st.afterAcc && VelaEnabled())
            {
                // the dispatch right after an accumulate: RCAS when the game sharpens
                st.afterAcc = false;
                std::vector<Binding> bs;
                std::vector<std::pair<UINT, const RootArg*>> cs;
                bindingsOf(st, bs, cs);
                std::vector<VelaTex> vt;
                UINT64 cb = 0;
                for (auto& b : bs)
                {
                    if (b.kind == 3) { if (!cb) cb = b.cbvVA; }
                    else vt.push_back({b.kind, b.reg, b.res});
                }
                postPass = true;
                callOrig = VelaOnPostPass(cl, vt, cb);
            }
            st.ring.push_back({st.pso, x, y, z});
            if (st.ring.size() > 32) st.ring.erase(st.ring.begin());
        }
    }
    if (callOrig) o_Dispatch(cl, x, y, z);
    if (velaPass && callOrig)
    {
        Lock g(g_mtx);
        VelaAfterFsrPass(cl, velaAcc);
    }
    // PC stand without a swap chain: RECON_FAKE_PRESENT=1 makes each accumulate a frame boundary
    static int fakePresent = -1;
    if (fakePresent < 0) { char e[4] = {}; fakePresent = GetEnvironmentVariableA("RECON_FAKE_PRESENT", e, 4) > 0 ? 1 : 0; }
    if (fakePresent && accPso) ReconOnPresent(); // also with Vela off (VELA_FAULT_CYCLE phase A)
    if (postPass && callOrig)
    {
        Lock g(g_mtx);
        VelaAfterPostPass(cl);
    }
}

//: PERF_SKIP_EI=<DXIL hash prefix>[,...] drops ExecuteIndirect calls while such a compute pipeline is
// bound (RE4: the SDF pass 1b96d027 costs ~40 ms on the console; title 14.5 -> 35.5 fps without it). An experiment /
// a per-game option: the pass's output is then missing. Off unless the variable is set.
typedef void(STDMETHODCALLTYPE* PFN_EI)(ID3D12GraphicsCommandList*, ID3D12CommandSignature*, UINT, ID3D12Resource*,
                                        UINT64, ID3D12Resource*, UINT64);
PFN_EI o_EI = nullptr;
std::vector<std::string> g_skipEi;
int g_skipEiInit = 0;
void STDMETHODCALLTYPE h_EI(ID3D12GraphicsCommandList* cl, ID3D12CommandSignature* sig, UINT maxCount, ID3D12Resource* args,
                            UINT64 argOff, ID3D12Resource* cnt, UINT64 cntOff)
{
    bool skip = false;
    {
        Lock g(g_mtx);
        g_eiCalls++;
        if (!g_skipEiInit)
        {
            g_skipEiInit = 1;
            // VELA_SKIP_SDF=1: RE4's SDF pass; VELA_SKIP_EI / PERF_SKIP_EI): any hash prefixes
            char e[512] = {};
            if (GetEnvironmentVariableA("VELA_SKIP_SDF", e, sizeof(e)) > 0 && e[0] == '1')
                g_skipEi.push_back("1b96d027d0a0da34b5d20049f06a038f");
            for (const char* key : {"VELA_SKIP_EI", "PERF_SKIP_EI"})
            {
                memset(e, 0, sizeof(e));
                if (GetEnvironmentVariableA(key, e, sizeof(e)) > 0)
                    for (char* t = strtok(e, ","); t; t = strtok(nullptr, ",")) g_skipEi.push_back(t);
            }
            if (!g_skipEi.empty())
            {
                std::string l;
                for (auto& h : g_skipEi) l += h + " ";
                LOGI("skip ExecuteIndirect of compute pipelines: %s", l.c_str());
            }
        }
        if (!g_skipEi.empty())
        {
            auto& st = g_cl[cl];
            auto pit = st.pso ? g_pso.find(st.pso) : g_pso.end();
            if (pit != g_pso.end())
                for (auto& pre : g_skipEi)
                    if (!pre.empty() && pit->second.hash.rfind(pre, 0) == 0) { skip = true; break; }
            if (skip && g_eiSkipped++ == 0) LOGI("skip ExecuteIndirect: first call skipped (pso hash %s)", pit->second.hash.c_str());
        }
    }
    if (!skip) o_EI(cl, sig, maxCount, args, argOff, cnt, cntOff);
}

// ---- RECON_VACHECK=1: GPU addresses the game gives the GPU that point at no live buffer ----------------------
// RE4 in a level: dmesg "gpu fault iova=0x6006880 READ TRANSLATION UCHE", 15-64 a minute, none on the title.
// Every buffer VA under vkd3d-proton on this kernel is above 4 GB (0x1_xxxxxxxx, ours included), so an address below
// 4 GB is not a buffer at all: a null root descriptor plus an offset, or a cut-down 64-bit address. Root CBV / SRV / UAV
// (raw VAs, no bounds check), index / vertex buffers, CBV descriptors and indirect signatures with root arguments are
// counted; the first hits are logged with the pipeline. Off by default: the hooks below are attached only with the key.
bool vaCheckOn()
{
    if (g_vaCheck < 0)
    {
        char e[4] = {};
        g_vaCheck = GetEnvironmentVariableA("RECON_VACHECK", e, 4) > 0 && e[0] == '1' ? 1 : 0;
    }
    return g_vaCheck > 0;
}
const char* kVaName[VA_N] = {"compute root CBV", "graphics root CBV", "compute root SRV", "graphics root SRV",
                             "compute root UAV", "graphics root UAV", "index buffer", "vertex buffer", "CBV descriptor"};
struct VaStat
{
    uint64_t total = 0, zero = 0, low = 0, unknown = 0;
};
VaStat g_vaStat[VA_N];
uint64_t g_vaLogged = 0, g_vaZeroLogged = 0, g_vaSigs = 0, g_vaSigRoot = 0;
std::string psoOf(ID3D12GraphicsCommandList* cl);
// under g_mtx
void vaNote(VaKind k, ID3D12GraphicsCommandList* cl, UINT slot, UINT64 va)
{
    auto& s = g_vaStat[k];
    s.total++;
    if (va == 0)
    {
        s.zero++; // legal when the shader does not read the slot; a read of it faults at "0 + offset"
        if (k <= VA_GUAV && g_vaZeroLogged++ < 24)
            LOGI("VACHECK %s slot %u = 0 (null), pso %s", kVaName[k], slot, psoOf(cl).c_str());
        return;
    }
    UINT64 off = 0;
    const bool low = va < (1ull << 32);
    if (!low && bufAt(va, off)) return;
    if (low) s.low++; else s.unknown++;
    if (g_vaLogged++ < 64)
        LOGI("VACHECK %s slot %u va=0x%llx: %s, pso %s", kVaName[k], slot, (unsigned long long) va,
             low ? "below 4 GB, not a buffer" : "outside every live buffer", psoOf(cl).c_str());
}
void vaSummary()
{
    std::string l;
    char b[160];
    for (int k = 0; k < VA_N; k++)
        if (g_vaStat[k].total)
        {
            snprintf(b, sizeof(b), " | %s %llu (null %llu, <4GB %llu, unknown %llu)", kVaName[k],
                     (unsigned long long) g_vaStat[k].total, (unsigned long long) g_vaStat[k].zero,
                     (unsigned long long) g_vaStat[k].low, (unsigned long long) g_vaStat[k].unknown);
            l += b;
        }
    LOGI("VACHECK totals: signatures %llu (with root descriptors %llu)%s", (unsigned long long) g_vaSigs,
         (unsigned long long) g_vaSigRoot, l.c_str());
}
typedef void(STDMETHODCALLTYPE* PFN_SetRootVA)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
typedef void(STDMETHODCALLTYPE* PFN_SetIB)(ID3D12GraphicsCommandList*, const D3D12_INDEX_BUFFER_VIEW*);
typedef void(STDMETHODCALLTYPE* PFN_SetVB)(ID3D12GraphicsCommandList*, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSig)(ID3D12Device*, const D3D12_COMMAND_SIGNATURE_DESC*,
                                                  ID3D12RootSignature*, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateReserved)(ID3D12Device*, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                                       const D3D12_CLEAR_VALUE*, REFIID, void**);
PFN_SetRootVA o_VaRoot[5]; // list slots 38..42: graphics CBV, compute SRV, graphics SRV, compute UAV, graphics UAV
PFN_SetIB o_SetIB;
PFN_SetVB o_SetVB;
PFN_CreateSig o_CreateSig;
PFN_CreateReserved o_Reserved;
template <int I>
void STDMETHODCALLTYPE h_VaRoot(ID3D12GraphicsCommandList* cl, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va)
{
    o_VaRoot[I](cl, i, va);
    if (Ext::OwnWork()) return;
    Lock g(g_mtx);
    // compute root SRV / UAV (slots 39, 41) are recorded too, for RestoreGameState after FSR 3.1
    if ((I == 1 || I == 3) && i < 64) { auto& a = g_cl[cl].root[i]; a.type = I == 1 ? 4 : 5; a.v = va; }
    if (g_vaCheck > 0) vaNote((VaKind) (VA_GCBV + I), cl, i, va);
}
void STDMETHODCALLTYPE h_SetIB(ID3D12GraphicsCommandList* cl, const D3D12_INDEX_BUFFER_VIEW* v)
{
    o_SetIB(cl, v);
    if (v) { Lock g(g_mtx); vaNote(VA_IB, cl, 0, v->BufferLocation); }
}
void STDMETHODCALLTYPE h_SetVB(ID3D12GraphicsCommandList* cl, UINT start, UINT n, const D3D12_VERTEX_BUFFER_VIEW* v)
{
    o_SetVB(cl, start, n, v);
    if (!v) return;
    Lock g(g_mtx);
    for (UINT i = 0; i < n; i++)
        if (v[i].SizeInBytes) vaNote(VA_VB, cl, start + i, v[i].BufferLocation);
}
HRESULT STDMETHODCALLTYPE h_CreateSig(ID3D12Device* d, const D3D12_COMMAND_SIGNATURE_DESC* desc, ID3D12RootSignature* rs,
                                      REFIID riid, void** pp)
{
    HRESULT r = o_CreateSig(d, desc, rs, riid, pp);
    if (!desc) return r;
    Lock g(g_mtx);
    g_vaSigs++;
    std::string a;
    bool root = false;
    for (UINT i = 0; i < desc->NumArgumentDescs; i++)
    {
        const auto t = desc->pArgumentDescs[i].Type;
        root |= t >= D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW && t <= D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW;
        a += " " + std::to_string((int) t);
    }
    if (root)
    {
        g_vaSigRoot++; // the GPU reads these addresses from the argument buffer: not visible here
        LOGI("VACHECK command signature %p with root descriptors / buffer views from GPU memory: stride %u, args%s",
             pp ? *pp : nullptr, desc->ByteStride, a.c_str());
    }
    return r;
}
void noteBuffer(const D3D12_RESOURCE_DESC* rd, void** pp);
HRESULT STDMETHODCALLTYPE h_Reserved(ID3D12Device* d, const D3D12_RESOURCE_DESC* rd, D3D12_RESOURCE_STATES st,
                                     const D3D12_CLEAR_VALUE* cv, REFIID riid, void** pp)
{
    HRESULT r = o_Reserved(d, rd, st, cv, riid, pp);
    if (SUCCEEDED(r)) noteBuffer(rd, pp); // tiled buffers have VAs too (unknown to the map otherwise)
    return r;
}
std::string psoOf(ID3D12GraphicsCommandList* cl)
{
    if (!cl) return "-";
    auto& st = g_cl[cl];
    if (!st.pso) return "none";
    auto it = g_pso.find(st.pso);
    if (it != g_pso.end()) return "compute #" + std::to_string(it->second.id) + " " + it->second.hash;
    char b[32];
    snprintf(b, sizeof(b), "graphics %p", (void*) st.pso);
    return b;
}

void* vt(void* obj, int i) { return (*(void***) obj)[i]; }

// Probe for a later switch from the Release hook to ID3DDestructionNotifier (the idea of OptiScaler upstream 6f0d1fdd,
// own code): does this D3D12 (vkd3d-proton on the console) hand it out, and does the callback fire on destruction?
bool g_notifyFired = false;
void WINAPI notifyProbe(void* p) { *(bool*) p = true; }

void hookVtables()
{
    ID3D12Device* dev = g_dev;
    g_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    g_deser = (PFN_Deser) GetProcAddress(d3d12, "D3D12CreateVersionedRootSignatureDeserializer");

    o_CPSO = (PFN_CreateCPSO) vt(dev, 11);
    o_Heap = (PFN_CreateHeap) vt(dev, 14);
    o_RS = (PFN_CreateRS) vt(dev, 16);
    o_CBV = (PFN_CreateCBV) vt(dev, 17);
    o_SRV = (PFN_CreateSRV) vt(dev, 18);
    o_UAV = (PFN_CreateUAV) vt(dev, 19);
    o_Copy = (PFN_CopyDesc) vt(dev, 23);
    o_CopyS = (PFN_CopyDescS) vt(dev, 24);
    ID3D12Device2* dev2 = nullptr; // CreatePipelineState (slot 47)
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dev2))) && dev2) { o_CreatePS = (PFN_CreatePS) vt(dev2, 47); dev2->Release(); }
    ID3D12Device1* dev1 = nullptr; // a throwaway pipeline library for its vtable: LoadComputePipeline 10, LoadPipeline 13
    ID3D12PipelineLibrary1* lib = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dev1))) && dev1)
    {
        dev1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&lib));
        dev1->Release();
    }
    if (lib) { o_LoadC = (PFN_LoadC) vt(lib, 10); o_LoadP = (PFN_LoadP) vt(lib, 13); }
    o_Committed = (PFN_CreateCommitted) vt(dev, 27);
    o_Placed = (PFN_CreatePlaced) vt(dev, 29);

    // a throwaway upload buffer and command list just to reach the shared resource / list vtables
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = 256;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* buf = nullptr;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                 IID_PPV_ARGS(&buf));
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    if (alloc) dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list));
    ID3D12Resource* tex = nullptr;
    {
        D3D12_HEAP_PROPERTIES th{};
        th.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = 4;
        td.Height = 4;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        dev->CreateCommittedResource(&th, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                     IID_PPV_ARGS(&tex));
    }
    if (buf)
    {
        o_Map = (PFN_Map) vt(buf, 8);
        o_Unmap = (PFN_Unmap) vt(buf, 9);
        o_ResRelease = (PFN_Release) vt(buf, 2);
    }
    if (tex && vt(tex, 2) != (void*) o_ResRelease) o_TexRelease = (PFN_Release) vt(tex, 2);
    if (list)
    {
        o_Reset = (PFN_Reset) vt(list, 10);
        o_Dispatch = (PFN_Dispatch) vt(list, 14);
        o_SetPSO = (PFN_SetPSO) vt(list, 25);
        o_SetHeaps = (PFN_SetHeaps) vt(list, 28);
        o_SetCRS = (PFN_SetCRS) vt(list, 29);
        o_SetCTable = (PFN_SetCTable) vt(list, 31);
        o_SetC1 = (PFN_SetC1) vt(list, 33);
        o_SetC32 = (PFN_SetC32) vt(list, 35);
        o_SetCCBV = (PFN_SetCCBV) vt(list, 37);
        o_CopyBuf = (PFN_CopyBuf) vt(list, 15);
        o_EI = (PFN_EI) vt(list, 59);
        o_VaRoot[1] = (PFN_SetRootVA) vt(list, 39); // compute root SRV / UAV, recorded for RestoreGameState
        o_VaRoot[3] = (PFN_SetRootVA) vt(list, 41);
        if (vaCheckOn())
        {
            for (int k = 0; k < 5; k++) o_VaRoot[k] = (PFN_SetRootVA) vt(list, 38 + k);
            o_SetIB = (PFN_SetIB) vt(list, 43);
            o_SetVB = (PFN_SetVB) vt(list, 44);
        }
    }
    if (vaCheckOn())
    {
        o_CreateSig = (PFN_CreateSig) vt(dev, 41);
        o_Reserved = (PFN_CreateReserved) vt(dev, 30);
    }
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&) o_CPSO, (PVOID) h_CPSO);
    DetourAttach(&(PVOID&) o_Heap, (PVOID) h_Heap);
    DetourAttach(&(PVOID&) o_RS, (PVOID) h_RS);
    DetourAttach(&(PVOID&) o_CBV, (PVOID) h_CBV);
    DetourAttach(&(PVOID&) o_SRV, (PVOID) h_SRV);
    DetourAttach(&(PVOID&) o_UAV, (PVOID) h_UAV);
    DetourAttach(&(PVOID&) o_Copy, (PVOID) h_Copy);
    DetourAttach(&(PVOID&) o_CopyS, (PVOID) h_CopyS);
    if (o_CreatePS) DetourAttach(&(PVOID&) o_CreatePS, (PVOID) h_CreatePS);
    if (o_LoadC) DetourAttach(&(PVOID&) o_LoadC, (PVOID) h_LoadC);
    if (o_LoadP) DetourAttach(&(PVOID&) o_LoadP, (PVOID) h_LoadP);
    DetourAttach(&(PVOID&) o_Committed, (PVOID) h_Committed);
    DetourAttach(&(PVOID&) o_Placed, (PVOID) h_Placed);
    if (o_Map) { DetourAttach(&(PVOID&) o_Map, (PVOID) h_Map); DetourAttach(&(PVOID&) o_Unmap, (PVOID) h_Unmap); }
    if (o_ResRelease) DetourAttach(&(PVOID&) o_ResRelease, (PVOID) h_ResRelease);
    if (o_TexRelease) DetourAttach(&(PVOID&) o_TexRelease, (PVOID) h_TexRelease);
    if (list)
    {
        DetourAttach(&(PVOID&) o_Reset, (PVOID) h_Reset);
        DetourAttach(&(PVOID&) o_Dispatch, (PVOID) h_Dispatch);
        DetourAttach(&(PVOID&) o_SetPSO, (PVOID) h_SetPSO);
        DetourAttach(&(PVOID&) o_SetHeaps, (PVOID) h_SetHeaps);
        DetourAttach(&(PVOID&) o_SetCRS, (PVOID) h_SetCRS);
        DetourAttach(&(PVOID&) o_SetCTable, (PVOID) h_SetCTable);
        DetourAttach(&(PVOID&) o_SetC1, (PVOID) h_SetC1);
        DetourAttach(&(PVOID&) o_SetC32, (PVOID) h_SetC32);
        DetourAttach(&(PVOID&) o_SetCCBV, (PVOID) h_SetCCBV);
        DetourAttach(&(PVOID&) o_CopyBuf, (PVOID) h_CopyBuf);
        DetourAttach(&(PVOID&) o_EI, (PVOID) h_EI);
        DetourAttach(&(PVOID&) o_VaRoot[1], (PVOID) h_VaRoot<1>);
        DetourAttach(&(PVOID&) o_VaRoot[3], (PVOID) h_VaRoot<3>);
        if (o_SetIB)
        {
            DetourAttach(&(PVOID&) o_VaRoot[0], (PVOID) h_VaRoot<0>);
            DetourAttach(&(PVOID&) o_VaRoot[2], (PVOID) h_VaRoot<2>);
            DetourAttach(&(PVOID&) o_VaRoot[4], (PVOID) h_VaRoot<4>);
            DetourAttach(&(PVOID&) o_SetIB, (PVOID) h_SetIB);
            DetourAttach(&(PVOID&) o_SetVB, (PVOID) h_SetVB);
        }
    }
    if (o_CreateSig) DetourAttach(&(PVOID&) o_CreateSig, (PVOID) h_CreateSig);
    if (o_Reserved) DetourAttach(&(PVOID&) o_Reserved, (PVOID) h_Reserved);
    LONG r = DetourTransactionCommit();
    LOGI("hooks: device(CPSO RS Heap SRV UAV CBV Copy CopyS) resource(Map Unmap)=%d list=%d deserializer=%p inc=%u "
         "commit=%ld",
         o_Map ? 1 : 0, list ? 1 : 0, (void*) g_deser, g_inc, r);
    if (list) { list->Close(); list->Release(); }
    if (alloc) alloc->Release();
    HRESULT nq = E_NOINTERFACE, nr = E_FAIL;
    if (buf)
    {
        ID3DDestructionNotifier* n = nullptr;
        nq = buf->QueryInterface(__uuidof(ID3DDestructionNotifier), (void**) &n);
        UINT id = 0;
        if (SUCCEEDED(nq) && n)
        {
            nr = n->RegisterDestructionCallback(&notifyProbe, &g_notifyFired, &id);
            n->Release();
        }
    }
    if (buf) buf->Release();
    LOGI("lifetime: ID3DDestructionNotifier QueryInterface 0x%08x, RegisterDestructionCallback 0x%08x, callback on the "
         "probe buffer's release: %s", (unsigned) nq, (unsigned) nr, g_notifyFired ? "fired" : "not fired");
    if (tex) tex->Release();
    if (lib) lib->Release();
    LOGI("pso paths hooked: CreatePipelineState %p, library LoadComputePipeline %p LoadPipeline %p; ExecuteIndirect %p",
         (void*) o_CreatePS, (void*) o_LoadC, (void*) o_LoadP, (void*) o_EI);
    LOGI("lifetime: resource Release hooked (buffer vtable %p, texture vtable %s)", (void*) o_ResRelease,
         o_TexRelease ? "separate" : "same");
}

typedef HRESULT(WINAPI* PFN_D3D12CreateDevice)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
PFN_D3D12CreateDevice o_D3D12CreateDevice = nullptr;

HRESULT WINAPI h_D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL fl, REFIID riid, void** ppDevice)
{
    HRESULT r = o_D3D12CreateDevice(adapter, fl, riid, ppDevice);
    if (SUCCEEDED(r) && ppDevice && *ppDevice && !g_dev)
    {
        ID3D12Device* dev = nullptr;
        if (SUCCEEDED(((IUnknown*) *ppDevice)->QueryInterface(IID_PPV_ARGS(&dev))) && dev)
        {
            g_dev = dev;
            LOGI("D3D12CreateDevice -> %p (fl 0x%x), installing recon hooks (level %d)", (void*) dev, (unsigned) fl,
                 g_level);
            hookVtables();
            VelaInit(dev, [](UINT64 va, UINT64& off) -> ID3D12Resource* {
                Lock g(g_mtx);
                return bufAt(va, off);
            });
        }
    }
    return r;
}
} // namespace

void InstallReconHooks()
{
    static bool done = false;
    if (done) return;
    char lvl[8] = {};
    if (GetEnvironmentVariableA("RECON_LEVEL", lvl, sizeof(lvl)) > 0) g_level = atoi(lvl);
    if (g_level == 0)
    {
        done = true;
        LOGI("recon: level 0 -- no D3D12 hooks, only the present counter (baseline fps)");
        return;
    }
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    if (!d3d12) d3d12 = LoadLibraryW(L"d3d12.dll");
    if (!d3d12) { LOGI("recon: d3d12.dll not present yet"); return; }
    o_D3D12CreateDevice = (PFN_D3D12CreateDevice) GetProcAddress(d3d12, "D3D12CreateDevice");
    if (!o_D3D12CreateDevice) { LOGI("recon: D3D12CreateDevice not found"); return; }
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&) o_D3D12CreateDevice, (PVOID) h_D3D12CreateDevice);
    LONG r = DetourTransactionCommit();
    done = true;
    LOGI("recon: hooked D3D12CreateDevice=%p level=%d commit=%ld", (void*) o_D3D12CreateDevice, g_level, r);
}

// Once per present, from the dxgi proxy's swap-chain hook: fps log, pending readbacks of the Vela module.
void ReconOnPresent()
{
    Lock g(g_mtx);
    static uint64_t presents = 0;
    if (g_vaCheck > 0 && ++presents % 600 == 0) vaSummary(); // ~30 s at 20 FPS
    VelaOnPresent();
}

namespace Ext
{
// the heaps the game last bound on this command list (0 when unknown)
UINT ListHeaps(ID3D12GraphicsCommandList* cl, ID3D12DescriptorHeap* out[2])
{
    Lock g(g_mtx);
    auto it = g_cl.find(cl);
    if (it == g_cl.end()) return 0;
    out[0] = it->second.heaps[0];
    out[1] = it->second.heaps[1];
    return it->second.nHeaps;
}

// The RE4 crash at the first FSR 3.1 frame: the FidelityFX DX12 backend records its passes on the game's
// list with its OWN descriptor heaps, compute root signature, pipelines and root arguments and leaves them bound. RE4
// sets its (single, shared) compute root signature and heaps once per list, so every game call after the replaced
// accumulate addressed root parameters of FFX's root signature: vkd3d-proton indexes root_signature->parameters[i]
// without a bounds check (d3d12_command_list_set_root_constants / _set_descriptor_table / _set_root_descriptor),
// which writes outside its bindings on the CPU. The recording hooks skip FSR 3.1's calls (Ext::OwnWork) so the list
// still holds the game's state, and this replays it through the original functions: heaps, root signature, the
// pipeline, then every root argument the game set since its last SetComputeRootSignature.
UINT RestoreGameState(ID3D12GraphicsCommandList* cl)
{
    CLState st;
    {
        Lock g(g_mtx);
        auto it = g_cl.find(cl);
        if (it == g_cl.end()) return 0;
        st.pso = it->second.pso;
        st.rs = it->second.rs;
        st.nHeaps = it->second.nHeaps;
        st.heaps[0] = it->second.heaps[0];
        st.heaps[1] = it->second.heaps[1];
        memcpy(st.root, it->second.root, sizeof(st.root));
    }
    UINT n = 0;
    if (st.nHeaps) { o_SetHeaps(cl, st.nHeaps, st.heaps); n++; }
    if (st.rs) { o_SetCRS(cl, st.rs); n++; }
    if (st.pso) { o_SetPSO(cl, st.pso); n++; }
    if (!st.rs) return n;
    for (UINT i = 0; i < 64; i++)
    {
        const RootArg& a = st.root[i];
        switch (a.type)
        {
        case 1: { D3D12_GPU_DESCRIPTOR_HANDLE h{a.v}; o_SetCTable(cl, i, h); n++; break; }
        case 2: o_SetCCBV(cl, i, a.v); n++; break;
        case 3: if (a.nc) { o_SetC32(cl, i, a.nc, a.c, 0); n++; } break;
        case 4: if (o_VaRoot[1]) { o_VaRoot[1](cl, i, a.v); n++; } break;
        case 5: if (o_VaRoot[3]) { o_VaRoot[3](cl, i, a.v); n++; } break;
        default: break;
        }
    }
    return n;
}
} // namespace Ext
