// The input contract: turn the per-frame inputs into SGSR3Constants, exactly as the offline replay
//, sgsr_replay --s3-fast) and the fork's SGSR3Feature::UpdateSharedConstants
// expect, so a capture of this interceptor's inputs reproduces offline. Conventions chosen by
// measurement against the DLSS teacher): jitter kept as +j (render pixels,
// "pixel p shows the scene at p - jitter"); prev UV = UV + mv*mvToUv, mvToUv = MV_Scale / (render for
// low-res MV, display for full-res); preExposure taken out so the history is in colour/preExposure
// units; exposure texture not read.
#pragma once
#include "sgsr3_constants.h"
#include <algorithm>
#include <cmath>

inline void FillConstants(SGSR3Constants& c, const FrameInputs& in, uint32_t debugMode = 0)
{
    const float rw = (float) in.renderW, rh = (float) in.renderH;
    const float dw = (float) in.displayW, dh = (float) in.displayH;

    c.renderSize[0] = rw; c.renderSize[1] = rh;
    c.displaySize[0] = dw; c.displaySize[1] = dh;
    c.renderSizeRcp[0] = 1.0f / rw; c.renderSizeRcp[1] = 1.0f / rh;
    c.displaySizeRcp[0] = 1.0f / dw; c.displaySizeRcp[1] = 1.0f / dh;
    c.renderSizeU[0] = in.renderW; c.renderSizeU[1] = in.renderH;

    c.jitter[0] = in.jitterX; c.jitter[1] = in.jitterY;

    const float mvw = in.lowResMV ? rw : (float) in.displayW;
    const float mvh = in.lowResMV ? rh : (float) in.displayH;
    c.mvToUv[0] = in.mvScaleX / mvw;
    c.mvToUv[1] = in.mvScaleY / mvh;

    c.mvTexSize[0] = (float) in.mvTexW; c.mvTexSize[1] = (float) in.mvTexH;
    c.mvTexSizeU[0] = in.mvTexW; c.mvTexSizeU[1] = in.mvTexH;

    const float sx = dw / rw, sy = dh / rh;
    c.scaleRatio[0] = (std::min)(sx, 1.99f);
    c.scaleRatio[1] = (std::min)(20.0f, std::pow(sx * sy, 3.0f));

    c.preExposure = (in.preExposure == 0.0f) ? 1.0f : in.preExposure;
    c.exposure = 1.0f;

    const float fov = in.verticalFovRad > 0.0f ? in.verticalFovRad : (59.0f * 3.14159265f / 180.0f);
    c.cameraFovAngleHor = std::tan(fov / 2.0f) * rw / rh;
    c.minLerpContribution = 0.3f;

    c.reactiveScale = 1.0f;
    c.tcScale = 1.0f;

    c.flags = SGSR3Flags::CatmullRom | SGSR3Flags::DilateMv;
    if (in.depthInverted) c.flags |= SGSR3Flags::DepthInverted;
    if (in.depthW) c.flags |= SGSR3Flags::HasDepth;
    if (in.hasReactive) c.flags |= SGSR3Flags::HasReactive;

    c.debugMode = debugMode;
    c.pad0 = 0;

    if (in.depthW) { c.depthSizeRcp[0] = 1.0f / (float) in.depthW; c.depthSizeRcp[1] = 1.0f / (float) in.depthH; }
    else { c.depthSizeRcp[0] = c.renderSizeRcp[0]; c.depthSizeRcp[1] = c.renderSizeRcp[1]; }

    c.tcUvScale[0] = 1.0f; c.tcUvScale[1] = 1.0f;
    if (in.tcW) { c.tcUvScale[0] = rw / (float) in.tcW; c.tcUvScale[1] = rh / (float) in.tcH; c.flags |= SGSR3Flags::HasTc; }

    c.colorSizeRcp[0] = c.renderSizeRcp[0]; c.colorSizeRcp[1] = c.renderSizeRcp[1];

    c.reset = in.reset ? 1.0f : 0.0f;
}
