#pragma once
#include <cstdint>
#include <vector>
#include <cstdlib>
#include <string>
#include <fstream>
#include <filesystem>
#include <cstdio>
// Prepass with the pass-01 conv weights + bias (initializer bytes 0..1087) baked in as constants, per model.
// VELA_R3=<q3k|d3|plk> loads (VELA_R3_DIR or <shader_dir>/r3)/<name>_<FNV-1a64 of this model's initializer bytes 0..1087>.spv and
// takes it only if (a) its FNV is in vela_r3_tab, (b) the table's weight hash equals the live initializer bytes (guard: a changed
// blob or another preset keeps the generic shader), (c) the active pass-01 is the generic shader it was baked from (q3 / pl).
struct VelaR3 { uint64_t sh, w, gen; };
static const VelaR3 vela_r3_tab[] = {
    {0xad0c67cd0187bab1ull, 0xdeb1a5d3154826e9ull, 0xa70bcaf24dd51d7dull},   // q3k_p
    {0x68cf980d009d8ec9ull, 0x28a88789e7babd4full, 0xa70bcaf24dd51d7dull},   // q3k_u
    {0x3b823b1969dbf2d5ull, 0xdeb1a5d3154826e9ull, 0xa70bcaf24dd51d7dull},   // d3_p
    {0xd8848d3f949cde2eull, 0x28a88789e7babd4full, 0xa70bcaf24dd51d7dull},   // d3_u
    {0xcd30792ce168501aull, 0xdeb1a5d3154826e9ull, 0xa70bcaf24dd51d7dull},   // q3kb_p (q3k + bias from the SSBO, q3's add order)
    {0x33fb64dddcf39f05ull, 0x28a88789e7babd4full, 0xa70bcaf24dd51d7dull},   // q3kb_u
    {0x8c93275eafe1535cull, 0xdeb1a5d3154826e9ull, 0x4d243005c298aaf8ull},   // plk_p
    {0xd8c9c63690c44ba5ull, 0x28a88789e7babd4full, 0x4d243005c298aaf8ull},   // plk_u
    {0xf634b13c166f27a8ull, 0xdeb1a5d3154826e9ull, 0xd1e2954b2abdb6e6ull},   // q1mkb_p (q1m + baked weights, bias from the SSBO)
    {0x6023dcd05ba7f812ull, 0x28a88789e7babd4full, 0xd1e2954b2abdb6e6ull}   // q1mkb_u
};
inline uint64_t vela_r3_fnv(const void* d, size_t n) { uint64_t h = 0xcbf29ce484222325ull; const unsigned char* p = static_cast<const unsigned char*>(d); for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull; return h; }
inline void vela_r3_select(std::vector<uint32_t>& code, const std::filesystem::path& dir, const std::filesystem::path& init) {
    const char* v = std::getenv("VELA_R3"); if (!v || !*v || std::string(v) == "0") return;
    unsigned char b[1088]; std::ifstream f(init, std::ios::binary);
    if (!f.read(reinterpret_cast<char*>(b), sizeof b)) { std::fprintf(stderr, "Baked-weight guard: cannot read 1088 B of %s: generic kept\n", init.string().c_str()); return; }
    const uint64_t wh = vela_r3_fnv(b, sizeof b); char hx[17]; std::snprintf(hx, sizeof hx, "%016llx", (unsigned long long)wh);
    const char* d = std::getenv("VELA_R3_DIR"); const auto p = (d ? std::filesystem::path(d) : dir / "r3") / (std::string(v) + "_" + hx + ".spv");
    std::ifstream s(p, std::ios::binary); if (!s) { std::fprintf(stderr, "Baked weights: no baked %s for weights %s: generic kept\n", v, hx); return; }
    std::vector<char> raw((std::istreambuf_iterator<char>(s)), std::istreambuf_iterator<char>());
    if (raw.empty() || raw.size() % 4) { std::fprintf(stderr, "Baked-weight guard: bad file %s: generic kept\n", p.string().c_str()); return; }
    const uint64_t sh = vela_r3_fnv(raw.data(), raw.size()), gh = vela_r3_fnv(code.data(), code.size() * 4);
    for (const auto& e : vela_r3_tab) if (e.sh == sh) {
        if (e.w != wh || e.gen != gh) { std::fprintf(stderr, "Baked-weight guard: %s %016llx bakes weights %016llx / generic %016llx, live %s / %016llx: generic kept\n", v, (unsigned long long)sh, (unsigned long long)e.w, (unsigned long long)e.gen, hx, (unsigned long long)gh); return; }
        code.assign(reinterpret_cast<const uint32_t*>(raw.data()), reinterpret_cast<const uint32_t*>(raw.data()) + raw.size() / 4);
        std::fprintf(stderr, "Baked weights: baked %s %016llx (weights %s) replaces generic %016llx\n", v, (unsigned long long)sh, hx, (unsigned long long)gh); return; }
    std::fprintf(stderr, "Baked-weight guard: %016llx not in the baked table: generic kept\n", (unsigned long long)sh);
}
// Lossy (prepass only): cheaper history reprojection variants of pl: cr5, cr5nd, bil, bilnd (FNV below).
// They keep pl's fused HTP-input store byte for byte, so they count as fused / baked. Selected only by VELA_LR=<variant> (lr_pre_override) or a root that holds them.
// Vela 0.8.16, lossy "FSR 4 Fast": FNV-1a64 of the Fast prepass variants (fp_gen.py: f1 g0 gb gbc). Same fused HTP store as pl; the f1* ones
// store X0 only when bit 0 of the HTP address word is 0 (vela_fp_x0f).
// network-grid-thread prepass q0..q3 (m2_gen.py; same fused HTP store + X0 flag as f1); VELA_M2_HASH=<hex,...> adds more (experimental, default empty)
static bool vela_m2_env(uint64_t h) { static const std::string e = [] { const char* v = std::getenv("VELA_M2_HASH"); return std::string(v ? v : ""); }(); size_t i = 0; while (i < e.size()) { size_t j = e.find(',', i); if (j == std::string::npos) j = e.size(); if (j > i && std::strtoull(e.substr(i, j - i).c_str(), nullptr, 16) == h) return true; i = j + 1; } return false; }
inline bool vela_fp_pre_hash(uint64_t h) { static const uint64_t k[] = {0xad0c67cd0187bab1ull, 0x68cf980d009d8ec9ull, 0xcd30792ce168501aull, 0x33fb64dddcf39f05ull, 0x3b823b1969dbf2d5ull, 0xd8848d3f949cde2eull, 0x1d4b0aa64b608b77ull, 0x207df664fb045742ull, 0x51a54b5a2c817253ull, 0xa70bcaf24dd51d7dull, 0x86f4cfa0aab97150ull, 0xb5a2a6cb20c28043ull, 0x9faa637664ef3457ull, 0xffd64122e1c611ebull, 0x3e0eaf8461afe870ull, 0x91beebe86a7e9ec7ull, 0xd1e2954b2abdb6e6ull, 0xf634b13c166f27a8ull, 0x6023dcd05ba7f812ull}; for (uint64_t x : k) if (x == h) return true; return vela_m2_env(h); }   // + cn (conv on the HTP, later step)
// fx1 = f1 without the reprojected store (X0 flag kept); VELA_FX bits read once
inline bool vela_fx_pre(uint64_t h) { return h == 0x86f4cfa0aab97150ull; }
inline int vela_fx_bits() { return fv::fx_bits(); }   // Per context (Fast sets 2)
inline bool vela_fp_x0f(uint64_t h) { static const uint64_t k[] = {0xd1e2954b2abdb6e6ull, 0xf634b13c166f27a8ull, 0x6023dcd05ba7f812ull, 0xad0c67cd0187bab1ull, 0x68cf980d009d8ec9ull, 0xcd30792ce168501aull, 0x33fb64dddcf39f05ull, 0x3b823b1969dbf2d5ull, 0xd8848d3f949cde2eull, 0x1d4b0aa64b608b77ull, 0x207df664fb045742ull, 0x51a54b5a2c817253ull, 0xa70bcaf24dd51d7dull, 0x86f4cfa0aab97150ull, 0xb5a2a6cb20c28043ull}; for (uint64_t x : k) if (x == h) return true; return false; }
inline bool vela_lr_pre_hash(uint64_t h) { if (vela_fp_pre_hash(h)) return true; static const uint64_t k[] = {0x8c93275eafe1535cull, 0xd8c9c63690c44ba5ull, 0x7996c05b3eb62885ull, 0xc781ac590291613full, 0xebb0273e94d3374dull, 0x2d76182a5d77764full}; for (uint64_t x : k) if (x == h) return true; return false; }
// Vela: FNV-1a64 of the regenerated pass-01 (pre) shaders that bake the even quad mask; guard_fsr411_prepass_quads must not run on them.
// FNV-1a64 of the fused pre f1g (pass-01 of the general 1080 performance bundle in Vela 0.8.4); Vela 0.8.6: or variant d
inline bool vela_fused_pre(const std::vector<uint32_t>& w) {
    uint64_t h = 0xcbf29ce484222325ull; const unsigned char* p = reinterpret_cast<const unsigned char*>(w.data());
    for (size_t i = 0, n = w.size() * 4; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h == 0xa62a14415b4caeb9ull || h == 0xcc403297d7247000ull   // + prepass variant d (f1g + depth 3x3 max from 4 fetches)
        || h == 0x8dc49206f89578e2ull
        || h == 0x4d243005c298aaf8ull || vela_lr_pre_hash(h);   // 0.8.15: dlbp + root-CBV loads issued once at the top (ir3 preamble): same fp ops, same HTP store   // 0.8.8: + a further dlbp variant (a4 + conv rewrite; same fp ops/order, fused HTP-input store unchanged)
}
// FNV-1a64 of K2P's k1x (pass-27: k1a, InterBuf store ^0x80808080) and k2lu (pass-28: K2 reading K1 as int8^0x80 = the HTP k1 tensor)
inline uint64_t vela_fnv(const std::vector<uint32_t>& w) { uint64_t h = 0xcbf29ce484222325ull; const unsigned char* p = reinterpret_cast<const unsigned char*>(w.data());
    for (size_t i = 0, n = w.size() * 4; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull; return h; }
inline bool vela_k1x(const std::vector<uint32_t>& w) { const uint64_t h = vela_fnv(w); return h == 0x17de83328b2e6065ull || h == 0x161689a8c07d3f75ull; }   // + UP k1x
inline bool vela_k2u(const std::vector<uint32_t>& w) { const uint64_t h = vela_fnv(w); return h == 0xe28d58d95e8cdca6ull || h == 0x72169abf0b2d80c1ull; }   // + UP k2lu
inline bool vela_baked_pre(const std::vector<uint32_t>& w) {
    static const uint64_t k[] = {0x067a3d152a1be205ull, 0xa62a14415b4caeb9ull,   // + f1g (fused pre, guarded HTP-input store)
                                 0xcc403297d7247000ull,   // Variant d (f1g's guard and HTP-input store, byte for byte)
                                 0x8dc49206f89578e2ull,   // dlbp (guard and HTP-input store as d)
                                 0x4d243005c298aaf8ull};  // dlbp + preloaded root-CBV (pl)
    uint64_t h = 0xcbf29ce484222325ull;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(w.data());
    for (size_t i = 0, n = w.size() * 4; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    for (uint64_t x : k) if (x == h) return true;
    return vela_lr_pre_hash(h);
}

// Vela 0.8.16: embedded pass-01 / K2 for the product (no files, no env): Fast -> q3kb (P / UP weights) or q3 (other weights);
// exact -> pl (plk only with VELA_PLK=1: plk + wave64 measured slower). Guards as for the baked-weight prepass: the live initializer bytes 0..1087 must hash to the baked
// weights and the active pass-01 must be the shipped pl. K2 rha replaces k2lu (P or UP by k2lu's hash) in Fast only.
#include "vela-fast-spv.hpp"
inline void vela_ff_pre(std::vector<uint32_t>& code, const std::vector<std::uint8_t>& init, int fast) {
    const uint64_t gh = vela_r3_fnv(code.data(), code.size() * 4);
    if (gh != 0x4d243005c298aaf8ull) { if (fast) std::fprintf(stderr, "Fast: pass-01 %016llx is not pl: Fast prepass not used\n", (unsigned long long)gh); return; }
    const uint64_t wh = init.size() >= 1088 ? vela_r3_fnv(init.data(), 1088) : 0;
    const bool P = wh == 0xdeb1a5d3154826e9ull, U = wh == 0x28a88789e7babd4full;
    const auto put = [&](const uint32_t* b, size_t n, const char* what) { code.assign(b, b + n); std::fprintf(stderr, "Fast: pass-01 %s (weights %016llx)\n", what, (unsigned long long)wh); };
    if (fast == 1) { if (P) put(kFfQ1mkbP, std::size(kFfQ1mkbP), "q1mkb_p (Fast)"); else if (U) put(kFfQ1mkbU, std::size(kFfQ1mkbU), "q1mkb_u (Fast)"); else put(kFfQ1m, std::size(kFfQ1m), "q1m generic (Fast)"); return; }
    if (fast) { if (P) put(kFfQ3kbP, std::size(kFfQ3kbP), "q3kb_p (Fast)"); else if (U) put(kFfQ3kbU, std::size(kFfQ3kbU), "q3kb_u (Fast)"); else put(kFfQ3, std::size(kFfQ3), "q3 generic (Fast)"); return; }
    const char* e = std::getenv("VELA_PLK"); if (!(e && e[0] == '1')) return;   // Measured: plk + wave64 = +0.014 ms vs wave64 alone -> plk off by default
    if (P) put(kFfPlkP, std::size(kFfPlkP), "plk_p (exact)"); else if (U) put(kFfPlkU, std::size(kFfPlkU), "plk_u (exact)");
}
inline bool vela_ff_k2(std::vector<uint32_t>& code) {   // Fast only; true = replaced (keeps k2lu's HTP K1 read: NPU path stays)
    const uint64_t h = vela_r3_fnv(code.data(), code.size() * 4);
    if (h == 0x72169abf0b2d80c1ull) code.assign(kFfRhaU, kFfRhaU + std::size(kFfRhaU)); else if (h == 0xe28d58d95e8cdca6ull) code.assign(kFfRhaP, kFfRhaP + std::size(kFfRhaP)); else return false;
    std::fprintf(stderr, "Fast: K2 rha_%c\n", h == 0x72169abf0b2d80c1ull ? 'u' : 'p'); return true;
}
