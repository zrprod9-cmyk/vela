// SGSR3Constants: the 144-byte UBO the v1f shaders read (same layout as the bench's SGSR3_FORK_PARAMS
// and the fork's SGSR3Feature.h, so a capture replays to the same image in).
// Our own definition; filling follows the input contract sorted out across phases 2..2c (jitter sign,
// mvToUv by low/full-res, preExposure out of history, FOV, reset, flags).
#pragma once
#include <cstdint>

struct alignas(16) SGSR3Constants
{
    float renderSize[2];
    float displaySize[2];
    float renderSizeRcp[2];
    float displaySizeRcp[2];
    float jitter[2];
    float mvTexSize[2];
    float mvToUv[2];
    float scaleRatio[2];
    float preExposure;
    float exposure;
    float cameraFovAngleHor;
    float minLerpContribution;
    float reset;
    float reactiveScale;
    float tcScale;
    uint32_t flags;
    uint32_t debugMode;
    uint32_t pad0;
    float depthSizeRcp[2];
    float tcUvScale[2];
    uint32_t renderSizeU[2];
    uint32_t mvTexSizeU[2];
    float colorSizeRcp[2];
};
static_assert(sizeof(SGSR3Constants) == 144, "SGSR3Constants must match the shader cbuffer layout");

namespace SGSR3Flags
{
constexpr uint32_t HasDepth = 1u;
constexpr uint32_t DepthInverted = 2u;
constexpr uint32_t HasReactive = 4u;
constexpr uint32_t HasTc = 8u;
constexpr uint32_t CatmullRom = 16u;
constexpr uint32_t DilateMv = 32u;
} // namespace SGSR3Flags

// Inputs the feature gathered from the game this frame, to fill the constants.
struct FrameInputs
{
    uint32_t renderW, renderH, displayW, displayH;
    float jitterX, jitterY;
    float mvScaleX, mvScaleY;
    uint32_t mvTexW, mvTexH;    // full-res MV texture size, or render size for low-res MV
    uint32_t depthW, depthH;    // 0 if no depth
    uint32_t tcW, tcH;          // 0 if no T&C mask
    float preExposure;          // 1 if unset
    float verticalFovRad;       // <=0 -> default
    bool lowResMV;
    bool depthInverted;
    bool hasReactive;
    bool reset;
    // NGX-style create flags, normalised from whichever input set them (NGX or FSR): bit0 HDR,
    // bit2 jittered MV, bit3 depth inverted, bit6 auto-exposure. Only the dumper's JSON reads these.
    uint32_t createFlags;
};
