// Tiny DXIL/DXBC container reader. A compiled D3D12 shader blob ("DXBC" magic, used for both DXBC and
// DXIL) starts with: magic(4) + container hash(16) + version(4) + sizeInBytes(4) + partCount(4) + a
// uint32 offset per part. Each part is fourCC(4) + size(4) + data. We need two cheap things from it:
//   1) a STABLE identity for the shader independent of the vkd3d version -- the 16-byte container hash,
//      which is baked into the game's exe/pak at build time (SPIR-V hashes would shift with vkd3d);
//   2) any ASCII strings the compiler left in, especially in the RDEF (resource definitions) part --
//      if the game did not strip reflection, these are the FSR 2 resource and cbuffer field names
//      (r_input_color_jittered, r_input_depth, r_input_motion_vectors, cbFSR2.renderSize, jitterOffset
//      ...), which identify a pass and its inputs by name with no guessing.
// Pure parsing, no D3D dependency; safe to call on any blob (bounds-checked).
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace dxil
{
inline bool isContainer(const void* data, size_t size)
{
    return data && size >= 32 && memcmp(data, "DXBC", 4) == 0;
}

// 32 hex chars of the container hash (bytes 4..19), or "" if not a container.
inline std::string hashHex(const void* data, size_t size)
{
    if (!isContainer(data, size)) return {};
    const uint8_t* b = (const uint8_t*) data + 4;
    static const char* H = "0123456789abcdef";
    std::string s(32, '0');
    for (int i = 0; i < 16; i++) { s[i * 2] = H[b[i] >> 4]; s[i * 2 + 1] = H[b[i] & 0xf]; }
    return s;
}

// Call `fn(fourCC, partData, partSize)` for each part. fourCC is a 4-char string (not NUL-terminated
// in the blob; we pass a std::string copy).
template <typename F> void forEachPart(const void* data, size_t size, F fn)
{
    if (!isContainer(data, size)) return;
    const uint8_t* b = (const uint8_t*) data;
    uint32_t total = *(const uint32_t*) (b + 24);
    uint32_t parts = *(const uint32_t*) (b + 28);
    if (total > size) total = (uint32_t) size;
    const uint32_t* offs = (const uint32_t*) (b + 32);
    if (32 + (size_t) parts * 4 > total) return;
    for (uint32_t i = 0; i < parts; i++)
    {
        uint32_t off = offs[i];
        if (off + 8 > total) continue;
        char cc[5] = {};
        memcpy(cc, b + off, 4);
        uint32_t psz = *(const uint32_t*) (b + off + 4);
        if (off + 8 + psz > total) psz = total - off - 8;
        fn(std::string(cc, 4), b + off + 8, (size_t) psz);
    }
}

// The resources a shader actually declares, from the PSV0 (pipeline state validation) part, which DXIL keeps
// even when reflection names are stripped. Layout: u32 runtimeInfoSize, runtimeInfo, u32 resourceCount, then
// (if count > 0) u32 bindInfoSize and count records {u32 resType, space, lowerBound, upperBound, ...}.
// resType: 1 sampler, 2 CBV, 3..5 SRV (typed/raw/structured), 6..9 UAV. kind: 1 SRV, 2 UAV, 3 CBV.
struct Bind
{
    int kind;
    uint32_t space, lo, hi;
};
inline std::vector<Bind> psvBindings(const void* data, size_t size, bool* ok = nullptr)
{
    std::vector<Bind> out;
    bool found = false;
    forEachPart(data, size, [&](const std::string& cc, const uint8_t* p, size_t n) {
        if (cc != "PSV0" || n < 8) return;
        uint32_t rtSize = *(const uint32_t*) p;
        size_t o = 4 + (size_t) rtSize;
        if (o + 4 > n) return;
        uint32_t count = *(const uint32_t*) (p + o);
        o += 4;
        found = true;
        if (!count) return;
        if (o + 4 > n) return;
        uint32_t rec = *(const uint32_t*) (p + o);
        o += 4;
        if (rec < 16) return;
        for (uint32_t i = 0; i < count && o + rec <= n; i++, o += rec)
        {
            const uint32_t* r = (const uint32_t*) (p + o);
            int kind = (r[0] == 2) ? 3 : (r[0] >= 3 && r[0] <= 5) ? 1 : (r[0] >= 6 && r[0] <= 9) ? 2 : 0;
            if (kind) out.push_back({kind, r[1], r[2], r[3]});
        }
    });
    if (ok) *ok = found;
    return out;
}

// Printable ASCII runs of length >= minLen found anywhere in [data, data+size). De-duplicated, order
// preserved. Used to surface resource/cbuffer names from RDEF (or the whole blob as a fallback).
inline std::vector<std::string> strings(const void* data, size_t size, size_t minLen = 4)
{
    std::vector<std::string> out;
    const uint8_t* b = (const uint8_t*) data;
    std::string cur;
    auto flush = [&] {
        if (cur.size() >= minLen)
        {
            for (auto& s : out) if (s == cur) { cur.clear(); return; }
            out.push_back(cur);
        }
        cur.clear();
    };
    for (size_t i = 0; i < size; i++)
    {
        uint8_t c = b[i];
        if (c >= 0x20 && c < 0x7f) cur.push_back((char) c);
        else flush();
    }
    flush();
    return out;
}
} // namespace dxil
