#!/usr/bin/env python3
"""Extract AMD FSR 4.0.2 ML Frame Generation (mlfi_4k) ONNX models for 1080p.

Reads weights directly from a user-supplied FidelityFX SDK v2.4.0
amd_fidelityfx_framegeneration_vk.dll (verified by SHA-256) using the
tensor layouts observed in the mlfi_4k compute passes:
  - BlockedTensor6h_NbCbHWNC  (FP16 Conv2d)
  - BlockedTensor6h_CbNbHWCN  (FP16 ConvTranspose2d 2x2 s2)
  - BlockedTensor6f8_NbCbHWNC (OCP FP8 E4M3FN Conv2d)
  - BlockedTensor6f8_CbNbHWCN (OCP FP8 E4M3FN ConvTranspose2d 2x2 s2)
  - Tensor4f8_HWCN            (OCP FP8 E4M3FN ConvTranspose2d 2x2 s2, pass176)
  - Tensor1f                  (FP32 bias)

Outputs:
  - mlfi_stage1_1080p.onnx : rgb01 [1, 6, 144, 240]  -> flow_px [1, 4, 144, 240]
  - mlfi_stage3_1080p.onnx : feat24 [1, 24, 272, 480] -> weights5 [1, 5, 272, 480]
"""
import argparse
import hashlib
import pathlib
import sys
import numpy as np
import onnx
from onnx import helper as hp, numpy_helper as nh, TensorProto as TP

EXPECTED_DLL_SHA256 = "d002dabff8c5e71eaa715fc99438cd4dc81230262a7897b16bc402efec3498b4"
WEIGHT_BASE = 0x00DF74B0
WEIGHT_LEN = 10_657_088


def make_fp8_e4m3fn_lut() -> np.ndarray:
    """Exact OCP FP8 E4M3FN -> FP32 lookup table (256 entries, bias=7, NaN at 0x7F/0xFF)."""
    lut = np.zeros(256, dtype=np.float32)
    for b in range(256):
        s = -1.0 if (b & 0x80) else 1.0
        exp = (b >> 3) & 0xF
        man = b & 0x7
        if exp == 0xF and man == 0x7:
            lut[b] = np.nan
        elif exp == 0:
            lut[b] = s * (man / 8.0) * (2.0 ** -6)
        else:
            lut[b] = s * (1.0 + man / 8.0) * (2.0 ** (exp - 7))
    return lut


FP8_LUT = make_fp8_e4m3fn_lut()


def sha256_file(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


class WeightReader:
    def __init__(self, dll_path: pathlib.Path, verify_sha256: bool = True):
        if verify_sha256:
            got = sha256_file(dll_path)
            if got != EXPECTED_DLL_SHA256:
                raise ValueError(
                    f"DLL SHA-256 mismatch for {dll_path}:\n"
                    f"  got:      {got}\n"
                    f"  expected: {EXPECTED_DLL_SHA256} (FidelityFX SDK v2.4.0)"
                )
        with open(dll_path, "rb") as f:
            f.seek(WEIGHT_BASE)
            self.blob = f.read(WEIGHT_LEN)
        if len(self.blob) != WEIGHT_LEN:
            raise ValueError(f"Short read from {dll_path}: got {len(self.blob)}, expected {WEIGHT_LEN}")

    def read_bias(self, off: int, count: int, trim: int = None) -> np.ndarray:
        arr = np.frombuffer(self.blob, dtype="<f4", count=count, offset=off).copy()
        if trim is not None:
            arr = arr[:trim]
        assert np.all(np.isfinite(arr)), f"Non-finite bias at {off}"
        return arr

    def read_conv_w(self, off: int, kh: int, kw: int, cin_p: int, cout_p: int,
                    cin: int, cout: int, fp8: bool = False) -> np.ndarray:
        """BlockedTensor6*_NbCbHWNC: (Nb, Cb, kh, kw, 16_cout, 16_cin) -> ONNX (Cout, Cin, kh, kw)."""
        assert cin_p % 16 == 0 and cout_p % 16 == 0
        nb, cb = cout_p // 16, cin_p // 16
        n = nb * cb * kh * kw * 16 * 16
        if fp8:
            raw = np.frombuffer(self.blob, dtype=np.uint8, count=n, offset=off)
            arr = FP8_LUT[raw]
        else:
            arr = np.frombuffer(self.blob, dtype="<f2", count=n, offset=off).astype(np.float32)
        w = arr.reshape(nb, cb, kh, kw, 16, 16).transpose(0, 4, 1, 5, 2, 3).reshape(cout_p, cin_p, kh, kw)
        w = w[:cout, :cin].copy()
        assert np.all(np.isfinite(w)), f"Non-finite conv weight at {off}"
        return w

    def read_deconv_w(self, off: int, kh: int, kw: int, cout_p: int, cin_p: int,
                      cin: int, cout: int, fp8: bool = False) -> np.ndarray:
        """BlockedTensor6*_CbNbHWCN: (Cout_b, Cin_b, kh, kw, 16_cout, 16_cin) -> ONNX (Cin, Cout, kh, kw)."""
        assert cin_p % 16 == 0 and cout_p % 16 == 0
        cout_b, cin_b = cout_p // 16, cin_p // 16
        n = cout_b * cin_b * kh * kw * 16 * 16
        if fp8:
            raw = np.frombuffer(self.blob, dtype=np.uint8, count=n, offset=off)
            arr = FP8_LUT[raw]
        else:
            arr = np.frombuffer(self.blob, dtype="<f2", count=n, offset=off).astype(np.float32)
        w = arr.reshape(cout_b, cin_b, kh, kw, 16, 16).transpose(1, 5, 0, 4, 2, 3).reshape(cin_p, cout_p, kh, kw)
        w = w[:cin, :cout].copy()
        assert np.all(np.isfinite(w)), f"Non-finite deconv weight at {off}"
        return w

    def read_deconv_hwcn_f8(self, off: int, kh: int, kw: int, cin: int, cout: int) -> np.ndarray:
        """Tensor4f8_HWCN (pass176): (kh, kw, Cout, Cin) -> ONNX (Cin, Cout, kh, kw)."""
        n = kh * kw * cout * cin
        raw = np.frombuffer(self.blob, dtype=np.uint8, count=n, offset=off)
        arr = FP8_LUT[raw]
        w = arr.reshape(kh, kw, cout, cin).transpose(3, 2, 0, 1).copy()
        assert np.all(np.isfinite(w)), f"Non-finite HWCN deconv weight at {off}"
        return w


class GraphBuilder:
    def __init__(self, wr: WeightReader):
        self.wr = wr
        self.inits = []
        self.nodes = []

    def const(self, name: str, arr) -> str:
        self.inits.append(nh.from_array(np.asarray(arr), name))
        return name

    def conv(self, x: str, w_off: int, b_off: int, kh: int, kw: int,
             cin_p: int, cout_p: int, cin: int, cout: int, stride: int, pad: int,
             tag: str, act: str = None, fp8: bool = False, shared_w=None) -> str:
        if shared_w is not None:
            wn, bn = shared_w
        else:
            w = self.wr.read_conv_w(w_off, kh, kw, cin_p, cout_p, cin, cout, fp8=fp8)
            b = self.wr.read_bias(b_off, cout_p, trim=cout)
            wn = self.const(f"{tag}_w", w)
            bn = self.const(f"{tag}_b", b)
        y = f"{tag}_conv"
        self.nodes.append(hp.make_node(
            "Conv", [x, wn, bn], [y],
            kernel_shape=[kh, kw],
            strides=[stride, stride],
            pads=[pad, pad, pad, pad],
        ))
        return self._act(y, tag, act)

    def deconv(self, x: str, w_off: int, b_off: int, cout_p: int, cin_p: int,
               cin: int, cout: int, tag: str, act: str = None, fp8: bool = False,
               hwcn: bool = False, shared_w=None) -> str:
        if shared_w is not None:
            wn, bn = shared_w
        else:
            if hwcn:
                w = self.wr.read_deconv_hwcn_f8(w_off, 2, 2, cin, cout)
            else:
                w = self.wr.read_deconv_w(w_off, 2, 2, cout_p, cin_p, cin, cout, fp8=fp8)
            b = self.wr.read_bias(b_off, cout_p, trim=cout)
            wn = self.const(f"{tag}_w", w)
            bn = self.const(f"{tag}_b", b)
        y = f"{tag}_deconv"
        self.nodes.append(hp.make_node(
            "ConvTranspose", [x, wn, bn], [y],
            kernel_shape=[2, 2],
            strides=[2, 2],
            pads=[0, 0, 0, 0],
        ))
        return self._act(y, tag, act)

    def _act(self, x: str, tag: str, act: str):
        if act is None:
            return x
        y = f"{tag}_{act}"
        if act == "lrelu":
            self.nodes.append(hp.make_node("LeakyRelu", [x], [y], alpha=0.2))
        elif act == "relu":
            self.nodes.append(hp.make_node("Relu", [x], [y]))
        elif act == "softmax":
            self.nodes.append(hp.make_node("Softmax", [x], [y], axis=1))
        else:
            raise ValueError(act)
        return y

    def add(self, a: str, b: str, tag: str) -> str:
        y = f"{tag}_add"
        self.nodes.append(hp.make_node("Add", [a, b], [y]))
        return y

    def mul_const(self, x: str, val: float, tag: str) -> str:
        c = self.const(f"{tag}_c", np.array(val, dtype=np.float32))
        y = f"{tag}_mul"
        self.nodes.append(hp.make_node("Mul", [x, c], [y]))
        return y

    def slice_c(self, x: str, c0: int, c1: int, tag: str) -> str:
        starts = self.const(f"{tag}_st", np.array([c0], dtype=np.int64))
        ends = self.const(f"{tag}_en", np.array([c1], dtype=np.int64))
        axes = self.const(f"{tag}_ax", np.array([1], dtype=np.int64))
        y = f"{tag}_sl"
        self.nodes.append(hp.make_node("Slice", [x, starts, ends, axes], [y]))
        return y

    def concat_c(self, xs, tag: str) -> str:
        y = f"{tag}_cat"
        self.nodes.append(hp.make_node("Concat", xs, [y], axis=1))
        return y

    def resize_linear(self, x: str, h_out: int, w_out: int, c: int, tag: str) -> str:
        roi = self.const(f"{tag}_roi", np.array([], dtype=np.float32))
        scales = self.const(f"{tag}_sc", np.array([], dtype=np.float32))
        sizes = self.const(f"{tag}_sz", np.array([1, c, h_out, w_out], dtype=np.int64))
        y = f"{tag}_res"
        self.nodes.append(hp.make_node(
            "Resize", [x, roi, scales, sizes], [y],
            mode="linear",
            coordinate_transformation_mode="half_pixel",
        ))
        return y

    def avgpool2(self, x: str, tag: str) -> str:
        y = f"{tag}_avg2"
        self.nodes.append(hp.make_node(
            "AveragePool", [x], [y],
            kernel_shape=[2, 2],
            strides=[2, 2],
        ))
        return y

    def warp(self, feat: str, flow2: str, h_pad: int, w_pad: int,
             h_valid: int, w_valid: int, tag: str) -> str:
        """Bilinear warp: src = clamp(coord + flow, 0, valid-1) via GridSample(align_corners=1)."""
        xs = np.arange(w_pad, dtype=np.float32)[None, :].repeat(h_pad, axis=0)
        ys = np.arange(h_pad, dtype=np.float32)[:, None].repeat(w_pad, axis=1)
        base_grid = np.stack([xs, ys], axis=0)[None, :, :, :]
        bg = self.const(f"{tag}_bg", base_grid)
        pos = self.add(bg, flow2, f"{tag}_pos")
        zero = self.const(f"{tag}_z", np.array([[[[0.0]], [[0.0]]]], dtype=np.float32))
        vmax = self.const(f"{tag}_mx", np.array([[[[float(w_valid - 1)]], [[float(h_valid - 1)]]]], dtype=np.float32))
        pos_c = f"{tag}_posc"
        self.nodes.append(hp.make_node("Max", [pos, zero], [f"{tag}_pos0"]))
        self.nodes.append(hp.make_node("Min", [f"{tag}_pos0", vmax], [pos_c]))
        scale = self.const(f"{tag}_ns", np.array([[[[2.0 / (w_pad - 1)]], [[2.0 / (h_pad - 1)]]]], dtype=np.float32))
        m1 = self.const(f"{tag}_m1", np.array(-1.0, dtype=np.float32))
        g_nchw = f"{tag}_gnchw"
        self.nodes.append(hp.make_node("Mul", [pos_c, scale], [f"{tag}_gmul"]))
        self.nodes.append(hp.make_node("Add", [f"{tag}_gmul", m1], [g_nchw]))
        g_nhwc = f"{tag}_gnhwc"
        self.nodes.append(hp.make_node("Transpose", [g_nchw], [g_nhwc], perm=[0, 2, 3, 1]))
        y = f"{tag}_wrp"
        self.nodes.append(hp.make_node(
            "GridSample", [feat, g_nhwc], [y],
            mode="bilinear",
            padding_mode="border",
            align_corners=1,
        ))
        return y


def build_stage1(wr: WeightReader, w_valid=240, h_valid=135, w_pad=240, h_pad=144) -> onnx.ModelProto:
    """Build Stage 1 optical flow pyramid at 1/8 res (padded to multiple of 16)."""
    gb = GraphBuilder(wr)
    x_in = "rgb01"

    rgb0 = gb.slice_c(x_in, 0, 3, "in_rgb0")
    rgb1 = gb.slice_c(x_in, 3, 6, "in_rgb1")

    s_w2 = wr.read_conv_w(0, 2, 2, 16, 32, 3, 32)
    s_b2 = wr.read_bias(4096, 32)
    s_w10 = wr.read_conv_w(45568, 2, 2, 16, 32, 4, 32)[:, 1:4]
    assert np.max(np.abs(s_w2 - s_w10)) == 0.0, "Siamese P2/P10 weight mismatch"

    def siamese(rgb: str, tag: str, share=None):
        if share is None:
            w2 = (gb.const("siam_p2_w", s_w2), gb.const("siam_p2_b", s_b2))
            w4 = (gb.const("siam_p4_w", wr.read_conv_w(4224, 3, 3, 32, 32, 32, 32)),
                  gb.const("siam_p4_b", wr.read_bias(22656, 32)))
            w6 = (gb.const("siam_p6_w", wr.read_conv_w(22784, 3, 3, 32, 32, 32, 32)),
                  gb.const("siam_p6_b", wr.read_bias(41216, 32)))
            w8 = (gb.const("siam_p8_w", wr.read_deconv_w(41344, 2, 2, 16, 32, 32, 6)),
                  gb.const("siam_p8_b", wr.read_bias(45440, 16, trim=6)))
            share = (w2, w4, w6, w8)
        h = gb.conv(rgb, 0, 0, 2, 2, 16, 32, 3, 32, 2, 0, f"{tag}_p2", "lrelu", shared_w=share[0])
        h = gb.conv(h, 0, 0, 3, 3, 32, 32, 32, 32, 1, 1, f"{tag}_p4", "lrelu", shared_w=share[1])
        h = gb.conv(h, 0, 0, 3, 3, 32, 32, 32, 32, 1, 1, f"{tag}_p6", "lrelu", shared_w=share[2])
        h = gb.deconv(h, 0, 0, 16, 32, 32, 6, f"{tag}_p8", None, shared_w=share[3])
        return h, share

    f0, sh = siamese(rgb0, "s0")
    f1, _ = siamese(rgb1, "s1", share=sh)

    base18 = gb.concat_c([rgb0, rgb1, f0, f1], "base18")
    feat0_9 = gb.concat_c([rgb0, f0], "feat0_9")
    feat1_9 = gb.concat_c([rgb1, f1], "feat1_9")

    def unet_level(x: str, tag: str, c_in: int, c_in_p: int, c_mid: int, c_mid_p: int, c_bot: int,
                   enc_specs, res_specs, dec_specs) -> str:
        h = gb.conv(x, enc_specs[0][0], enc_specs[0][1], 2, 2, c_in_p, c_mid_p, c_in, c_mid, 2, 0, f"{tag}_e0", "lrelu")
        h = gb.conv(h, enc_specs[1][0], enc_specs[1][1], 3, 3, c_mid_p, c_mid_p, c_mid, c_mid, 1, 1, f"{tag}_e1", "lrelu")
        h = gb.conv(h, enc_specs[2][0], enc_specs[2][1], 2, 2, c_mid_p, c_bot, c_mid, c_bot, 2, 0, f"{tag}_e2", "lrelu")
        for i, (wo, bo) in enumerate(res_specs):
            c = gb.conv(h, wo, bo, 3, 3, c_bot, c_bot, c_bot, c_bot, 1, 1, f"{tag}_r{i}", None)
            h = gb._act(gb.add(c, h, f"{tag}_r{i}"), f"{tag}_r{i}", "lrelu")
        h = gb.deconv(h, dec_specs[0][0], dec_specs[0][1], c_mid_p, c_bot, c_bot, c_mid, f"{tag}_d0", "lrelu")
        h = gb.deconv(h, dec_specs[1][0], dec_specs[1][1], 16, c_mid_p, c_mid, 13, f"{tag}_d1", None)
        return h

    base_1_32 = gb.resize_linear(base18, h_pad // 4, w_pad // 4, 18, "p18")

    l1_out = unet_level(
        base_1_32, "l1", 18, 32, 96, 96, 192,
        enc_specs=[(91136, 115712), (116096, 281984), (282368, 429824)],
        res_specs=[
            (430592, 1094144), (1094912, 1758464), (1759232, 2422784), (2423552, 3087104),
            (3087872, 3751424), (3752192, 4415744), (4416512, 5080064), (5080832, 5744384),
        ],
        dec_specs=[(5745152, 5892608), (5892992, 5905280)],
    )

    t05_1_8 = gb.const("t05_1_8", np.full((1, 1, h_pad, w_pad), 0.5, dtype=np.float32))

    def warp_and_pack(flow4_1_8: str, ctx9_1_8: str, tag: str, flow_scale_for_pack: float = 1.0, down2: bool = False):
        fl0 = gb.slice_c(flow4_1_8, 0, 2, f"{tag}_fl0")
        fl1 = gb.slice_c(flow4_1_8, 2, 4, f"{tag}_fl1")
        w0 = gb.warp(feat0_9, fl0, h_pad, w_pad, h_valid, w_valid, f"{tag}_w0")
        w1 = gb.warp(feat1_9, fl1, h_pad, w_pad, h_valid, w_valid, f"{tag}_w1")
        w_rgb0 = gb.slice_c(w0, 0, 3, f"{tag}_wrgb0")
        w_f0 = gb.slice_c(w0, 3, 9, f"{tag}_wf0")
        w_rgb1 = gb.slice_c(w1, 0, 3, f"{tag}_wrgb1")
        w_f1 = gb.slice_c(w1, 3, 9, f"{tag}_wf1")
        fl_pack = gb.mul_const(flow4_1_8, flow_scale_for_pack, f"{tag}_fls") if flow_scale_for_pack != 1.0 else flow4_1_8
        p32 = gb.concat_c([w_rgb0, w_rgb1, w_f0, w_f1, t05_1_8, ctx9_1_8, fl_pack], f"{tag}_p32")
        if down2:
            p32 = gb.avgpool2(p32, f"{tag}_d2")
        return p32

    l1_up = gb.resize_linear(l1_out, h_pad, w_pad, 13, "p54_up")
    run_flow = gb.mul_const(gb.slice_c(l1_up, 0, 4, "p54_fl"), 4.0, "p54_fl4")
    l1_ctx = gb.slice_c(l1_up, 4, 13, "p54_ctx")
    l2_in = warp_and_pack(run_flow, l1_ctx, "p54", flow_scale_for_pack=0.5, down2=True)

    l2_out = unet_level(
        l2_in, "l2", 32, 32, 64, 64, 128,
        enc_specs=[(5905408, 5921792), (5922048, 5995776), (5996032, 6061568)],
        res_specs=[
            (6062080, 6356992), (6357504, 6652416), (6652928, 6947840), (6948352, 7243264),
            (7243776, 7538688), (7539200, 7834112), (7834624, 8129536), (8130048, 8424960),
        ],
        dec_specs=[(8425472, 8491008), (8491264, 8499456)],
    )

    l2_up = gb.resize_linear(l2_out, h_pad, w_pad, 13, "p83_up")
    l2_fl = gb.mul_const(gb.slice_c(l2_up, 0, 4, "p83_fl"), 2.0, "p83_fl2")
    run_flow = gb.add(run_flow, l2_fl, "p83_rfl")
    l2_ctx = gb.slice_c(l2_up, 4, 13, "p83_ctx")
    l3_in = warp_and_pack(run_flow, l2_ctx, "p83", flow_scale_for_pack=1.0, down2=False)

    l3_out = unet_level(
        l3_in, "l3", 32, 32, 48, 48, 96,
        enc_specs=[(8499584, 8511872), (8512128, 8553600), (8553856, 8590720)],
        res_specs=[
            (8591104, 8756992), (8757376, 8923264), (8923648, 9089536), (9089920, 9255808),
            (9256192, 9422080), (9422464, 9588352), (9588736, 9754624), (9755008, 9920896),
        ],
        dec_specs=[(9921280, 9958144), (9958400, 9964544)],
    )

    run_flow = gb.add(run_flow, gb.slice_c(l3_out, 0, 4, "p112_fl"), "p112_rfl")
    l3_ctx = gb.slice_c(l3_out, 4, 13, "p112_ctx")
    l4_in = warp_and_pack(run_flow, l3_ctx, "p112", flow_scale_for_pack=1.0, down2=False)

    l4_out = unet_level(
        l4_in, "l4", 32, 32, 24, 32, 48,
        enc_specs=[(9964672, 9972864), (9972992, 9991424), (9991552, 10003840)],
        res_specs=[
            (10004096, 10045568), (10045824, 10087296), (10087552, 10129024), (10129280, 10170752),
            (10171008, 10212480), (10212736, 10254208), (10254464, 10295936), (10296192, 10337664),
        ],
        dec_specs=[(10337920, 10350208), (10350336, 10354432)],
    )

    final_flow = gb.add(run_flow, gb.slice_c(l4_out, 0, 4, "p149_fl"), "p149_out")
    gb.nodes.append(hp.make_node("Identity", [final_flow], ["flow_px"]))

    g = hp.make_graph(
        gb.nodes, "mlfi_stage1_1080p",
        [hp.make_tensor_value_info("rgb01", TP.FLOAT, [1, 6, h_pad, w_pad])],
        [hp.make_tensor_value_info("flow_px", TP.FLOAT, [1, 4, h_pad, w_pad])],
        initializer=gb.inits,
    )
    mdl = hp.make_model(g, opset_imports=[hp.make_opsetid("", 16)], producer_name="mlfi-extract")
    mdl.ir_version = 8
    onnx.checker.check_model(mdl)
    return mdl


def build_stage3(wr: WeightReader, w_pad=480, h_pad=272) -> onnx.ModelProto:
    """Build Stage 3 blending U-Net at 1/4 res (480x272 -> 5ch Softmax weights)."""
    gb = GraphBuilder(wr)
    x = "feat24"

    h = gb.conv(x, 10354560, 10372992, 3, 3, 32, 32, 24, 32, 1, 1, "p154", "relu", fp8=False)
    h = gb.conv(h, 10373120, 10385408, 2, 2, 32, 48, 32, 48, 2, 0, "p156", None, fp8=False)

    def fasternet_block(inp: str, tag: str, c_tot: int, c_pconv: int, c_exp: int,
                        w3_off, b3_off, w1a_off, b1a_off, w1b_off, b1b_off) -> str:
        x_head = gb.slice_c(inp, 0, c_pconv, f"{tag}_hd")
        x_tail = gb.slice_c(inp, c_pconv, c_tot, f"{tag}_tl")
        y0 = gb.conv(x_head, w3_off, b3_off, 3, 3, c_pconv, c_pconv, c_pconv, c_pconv, 1, 1, f"{tag}_c3", None, fp8=True)
        y1 = gb.concat_c([y0, x_tail], f"{tag}_cat")
        y2 = gb.conv(y1, w1a_off, b1a_off, 1, 1, c_tot, c_exp, c_tot, c_exp, 1, 0, f"{tag}_pw1", "relu", fp8=True)
        y3 = gb.conv(y2, w1b_off, b1b_off, 1, 1, c_exp, c_tot, c_exp, c_tot, 1, 0, f"{tag}_pw2", None, fp8=True)
        return gb.add(y3, inp, f"{tag}_res")

    s3_enc48 = [
        ("p158", 10385664, 10387968, 10388096, 10392704, 10393088, 10397696),
        ("p160", 10397952, 10400256, 10400384, 10404992, 10405376, 10409984),
        ("p162", 10410240, 10412544, 10412672, 10417280, 10417664, 10422272),
        ("p164", 10422528, 10424832, 10424960, 10429568, 10429952, 10434560),
    ]
    for tag, w3, b3, w1a, b1a, w1b, b1b in s3_enc48:
        h = fasternet_block(h, tag, 48, 16, 96, w3, b3, w1a, b1a, w1b, b1b)
    skip48 = h

    h = gb.conv(h, 10434816, 10447104, 2, 2, 48, 64, 48, 64, 2, 0, "p166", None, fp8=True)

    s3_bot64 = [
        ("p168", 10447360, 10456576, 10456704, 10464896, 10465408, 10473600),
        ("p170", 10473856, 10483072, 10483200, 10491392, 10491904, 10500096),
        ("p172", 10500352, 10509568, 10509696, 10517888, 10518400, 10526592),
        ("p174", 10526848, 10536064, 10536192, 10544384, 10544896, 10553088),
        ("p176", 10553344, 10562560, 10562688, 10570880, 10571392, 10579584),
    ]
    for tag, w3, b3, w1a, b1a, w1b, b1b in s3_bot64:
        h = fasternet_block(h, tag, 64, 32, 128, w3, b3, w1a, b1a, w1b, b1b)

    h = gb.deconv(h, 10579840, 10592128, 48, 64, 64, 48, "p176_up", None, fp8=True, hwcn=True)
    h = gb.add(h, skip48, "p176_skip")

    s3_dec48 = [
        ("p178", 10592384, 10594688, 10594816, 10599424, 10599808, 10604416),
        ("p180", 10604672, 10606976, 10607104, 10611712, 10612096, 10616704),
        ("p182", 10616960, 10619264, 10619392, 10624000, 10624384, 10628992),
        ("p184", 10629248, 10631552, 10631680, 10636288, 10636672, 10641280),
    ]
    for tag, w3, b3, w1a, b1a, w1b, b1b in s3_dec48:
        h = fasternet_block(h, tag, 48, 16, 96, w3, b3, w1a, b1a, w1b, b1b)

    h = gb.deconv(h, 10641536, 10647680, 32, 48, 48, 32, "p186", None, fp8=True)
    h = gb.conv(h, 10647808, 10657024, 3, 3, 32, 16, 32, 5, 1, 1, "p188", "softmax", fp8=False)

    gb.nodes.append(hp.make_node("Identity", [h], ["weights5"]))
    g = hp.make_graph(
        gb.nodes, "mlfi_stage3_1080p",
        [hp.make_tensor_value_info("feat24", TP.FLOAT, [1, 24, h_pad, w_pad])],
        [hp.make_tensor_value_info("weights5", TP.FLOAT, [1, 5, h_pad, w_pad])],
        initializer=gb.inits,
    )
    mdl = hp.make_model(g, opset_imports=[hp.make_opsetid("", 16)], producer_name="mlfi-extract")
    mdl.ir_version = 8
    onnx.checker.check_model(mdl)
    return mdl


def extract(dll_path: pathlib.Path, out_dir: pathlib.Path, verify_sha256: bool = True,
            stages=("s1", "s3")):
    out_dir.mkdir(parents=True, exist_ok=True)
    wr = WeightReader(dll_path, verify_sha256=verify_sha256)

    assert 10447104 - 10434816 == 2 * 2 * 48 * 64
    assert 10647680 - 10641536 == 2 * 2 * 48 * 32
    assert 10657024 - 10647808 == 3 * 3 * 32 * 16 * 2

    out = {}
    if "s1" in stages:
        m1 = build_stage1(wr)
        p1 = out_dir / "mlfi_stage1_1080p.onnx"
        onnx.save(m1, str(p1))
        print(f"ONNX mlfi_s1 path={p1} size={p1.stat().st_size} nodes={len(m1.graph.node)} sha256={sha256_file(p1)}")
        out["s1"] = p1
    if "s3" in stages:
        m3 = build_stage3(wr)
        p3 = out_dir / "mlfi_stage3_1080p.onnx"
        onnx.save(m3, str(p3))
        print(f"ONNX mlfi_s3 path={p3} size={p3.stat().st_size} nodes={len(m3.graph.node)} sha256={sha256_file(p3)}")
        out["s3"] = p3
    return out


def main():
    ap = argparse.ArgumentParser(description="Extract FSR 4.0.2 ML FG (mlfi_4k) 1080p ONNX models from FidelityFX SDK v2.4.0 DLL.")
    ap.add_argument("--dll", required=True, type=pathlib.Path, help="Path to amd_fidelityfx_framegeneration_vk.dll (FidelityFX SDK v2.4.0)")
    ap.add_argument("--out", required=True, type=pathlib.Path, help="Output directory for ONNX models")
    ap.add_argument("--stage", choices=["all", "s1", "s3"], default="all", help="Which stage(s) to extract")
    ap.add_argument("--skip-sha256", action="store_true", help="Skip SHA-256 check of the input DLL")
    args = ap.parse_args()
    stages = ("s1", "s3") if args.stage == "all" else (args.stage,)
    extract(args.dll, args.out, verify_sha256=not args.skip_sha256, stages=stages)


if __name__ == "__main__":
    main()
