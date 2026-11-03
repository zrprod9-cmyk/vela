"""Vela 0.8.13: the panel <-> DLL log contract. The lines below are copied verbatim from console runs of
the 0.8.13 DLLs (RE4 interceptor: the format strings of the OptiScaler fork patch 0017; Control never reached a
level, so its fork lines are the LOG_* strings with the run's own sizes). A fresh temp folder per run (left in the
system temp dir, nothing is deleted).
Run: python -I tests/test_log_contract.py  (or python -m unittest discover -s tests)"""
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "py_modules"))
import sgsr3  # noqa: E402

TMP = tempfile.mkdtemp(prefix="vela_contract_")

# ---- RE4 static interceptor (sgsr3intercept.log) -------------------------------------------------------------------
CAL = (r"[20:43:29.080] vela: calibration ok: layout fsr2.0+re16 render 640x360 (texture 640x360) display 1920x1080 "
       r"jitter (-0.1562, 0.4630) mvScale (1.0000, 1.0000) (measured) preExposure 1.000 tan(hfov/2) 0.364 phases 72" "\n")
SGSR2 = (r"[20:43:17.216] vela: live (Z:\tmp\vela-live\2050650.ini): model sgsr2 lossy 1 npu 0" "\n" + CAL +
         r"[20:43:29.287] vela: SGSR 2 context 640x360 -> 1920x1080 (Qualcomm SGSR 2, 2-pass compute, Vulkan interop)" "\n"
         r"[20:43:29.287] vela: SGSR 2: game descriptor heaps re-bound after its passes (2 heaps)" "\n"
         r"[20:43:29.287] vela: SGSR 2: first frame dispatched (colour fmt 97 depth fmt 100 mv fmt 83, depth reverse-Z, MinLerp 0.30)" "\n"
         r"[20:43:29.287] vela: model sgsr2 runs 640x360 -> 1920x1080" "\n"
         r"[20:43:31.662] fps 71.0 (presents 710 in 10.0 s; frames by Vela 229), SGSR 2 GPU 1.61 ms" "\n"
         r"[20:43:41.670] fps 79.6 (presents 797 in 10.0 s; frames by Vela 797), SGSR 2 GPU 1.92 ms" "\n")
FSR3 = (r"[20:44:14.140] vela: live (Z:\tmp\vela-live\2050650.ini): model fsr3 lossy 1 npu 0" "\n"
        r"[20:44:14.166] vela: sgsr2 context retired (released after 16 presents)" "\n"
        r"[20:44:14.301] vela: FSR 3.1 context 640x360 -> 1920x1080 (amd_fidelityfx_dx12.dll)" "\n"
        r"[20:44:14.305] vela: FSR 3.1: the game's compute state set again after FFX's passes (14 calls)" "\n"
        r"[20:44:14.305] vela: FSR 3.1: first frame dispatched" "\n"
        r"[20:44:14.305] vela: model fsr3 runs 640x360 -> 1920x1080" "\n"
        r"[20:44:21.757] fps 35.9 (presents 360 in 10.0 s; frames by Vela 360), FSR 3.1" "\n")
FSR4 = (r"[20:44:43.312] vela: live (Z:\tmp\vela-live\2050650.ini): model fsr4 lossy 1 npu 0" "\n"
        r"[20:44:43.338] vela: fsr3 context retired (released after 16 presents)" "\n"
        r"[20:44:43.366] Vela FSR 4 VK: FSR 4.1.1 VK INT8 context 640x360 -> 1920x1080 (hdr 0, network every 1 frames)" "\n"
        r"[20:44:43.833] vela: model fsr4 runs 640x360 -> 1920x1080" "\n"
        r"[20:44:51.800] fps 23.8 (presents 238 in 10.0 s; frames by Vela 238), FSR 4 GPU 20.70 ms" "\n")
LITE_SIZE = (r"[20:25:44.390] vela: live (Z:\tmp\vela-live\2050650.ini): model fsr4lite lossy 1 npu 2" "\n"
             r"[20:25:44.435] vela: FSR 4.1 Lite runs only from 645x360 to 1920x1080 render -> 1920x1080, not 640x360 -> "
             r"1920x1080; the game's FSR 2 instead" "\n"
             r"[20:25:44.435] vela: fsr4lite did not run this frame -- the game's FSR 2 runs" "\n"
             r"[20:25:44.479] vela: model fsr2 runs 640x360 -> 1920x1080 (the game's own FSR 2)" "\n"
             r"[20:25:49.705] fps 30.9 (presents 309 in 10.0 s; frames by Vela 107), the game's FSR 2" "\n"
             r"[20:25:59.704] fps 39.1 (presents 391 in 10.0 s; frames by Vela 0), the game's FSR 2" "\n")
# the second Lite pick at the same render: no size line again, straight to fsr2 (run C_b3 20:26:59)
LITE_AGAIN = (r"[20:26:59.694] vela: live (Z:\tmp\vela-live\2050650.ini): model fsr4lite lossy 1 npu 2" "\n"
              r"[20:26:59.694] vela: model fsr2 runs 640x360 -> 1920x1080 (the game's own FSR 2)" "\n"
              r"[20:27:09.802] fps 39.6 (presents 397 in 10.0 s; frames by Vela 0), the game's FSR 2" "\n")
# an old live key (run C_b3 20:28:04): the DLL names it, then its live line says fsr2
NOT_OFFERED = (r"[20:28:04.974] vela: model 'ai' is not offered (models: fsr4, fsr4lite, fsr3, sgsr2) -- the game's FSR 2 runs" "\n"
               r"[20:28:04.974] vela: live (Z:\tmp\vela-live\2050650.ini): model fsr2 lossy 1 npu 2" "\n"
               r"[20:28:09.862] fps 37.0 (presents 370 in 10.0 s; frames by Vela 0), the game's FSR 2" "\n")
BACK_FSR4 = (r"[20:28:24.206] vela: live (Z:\tmp\vela-live\2050650.ini): model fsr4 lossy 1 npu 2" "\n"
             r"[20:28:24.206] vela: model fsr4 runs 640x360 -> 1920x1080" "\n")
# vela_fsr2.cpp:549 format ("vela: %s frame failed -- the game's FSR 2 from now on for this size")
FSR4_FAILED = (r"[20:30:00.000] vela: fsr4 frame failed -- the game's FSR 2 from now on for this size" "\n"
               r"[20:30:00.010] vela: model fsr2 runs 640x360 -> 1920x1080 (the game's own FSR 2)" "\n"
               r"[20:30:10.000] fps 37.0 (presents 370 in 10.0 s; frames by Vela 0), the game's FSR 2" "\n")


def static(text):
    p = os.path.join(TMP, "sgsr3intercept.log")
    with open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return sgsr3.static_log_summary(p)


class StaticContract(unittest.TestCase):
    def test_sgsr2_runs(self):
        lg = static(SGSR2)
        self.assertEqual((lg["mode"], lg["sgsr3"], lg["render"], lg["output"], lg["frames"]),
                         ("sgsr2", True, "640x360", "1920x1080", 797), lg)
        self.assertEqual((lg["fps"], lg["fps_model"], lg["gpu_ms"]), (79.6, "sgsr2", 1.92), lg)
        self.assertIsNone(lg["sgsr2_fallback"]); self.assertIsNone(lg["fsr4_fallback"]); self.assertIsNone(lg["not_offered"])

    def test_fsr3_after_sgsr2(self):
        lg = static(SGSR2 + FSR3)
        self.assertEqual((lg["mode"], lg["fsr3_version"], lg["fps_model"], lg["gpu_ms"]), ("fsr3", "3.1", "fsr3", None), lg)
        self.assertIsNone(lg["fsr3_fallback"])

    def test_fsr4_gpu_ms(self):
        lg = static(SGSR2 + FSR3 + FSR4)
        self.assertEqual((lg["mode"], lg["fsr4_version"], lg["fps_model"], lg["gpu_ms"]), ("fsr4", "4.1.1", "fsr4", 20.7), lg)
        self.assertIsNone(lg["fsr4_fallback"])

    # 0.8.15: FSR 4 Lite left Vela; the Lite lines of an older run are still read: a refusal like any other ("failed")
    def test_old_lite_size_line_is_a_failed_fsr4(self):
        lg = static(SGSR2 + LITE_SIZE)
        self.assertEqual((lg["mode"], lg["sgsr3"], lg["fsr4_fallback"], lg["fps_model"]),
                         ("fsr2", False, "failed", "fsr2"), lg)

    def test_old_lite_pick_is_fsr4(self):
        lg = static(SGSR2 + LITE_SIZE + FSR4 + LITE_AGAIN)
        self.assertEqual((lg["mode"], lg["sgsr3"], lg["fsr4_fallback"]), ("fsr2", False, "failed"), lg)
        self.assertNotIn("lossy", lg)

    def test_fsr4_frame_failed(self):
        lg = static(FSR4 + FSR4_FAILED)
        self.assertEqual((lg["mode"], lg["sgsr3"], lg["fsr4_fallback"]), ("fsr2", False, "failed"), lg)

    def test_sgsr2_old_dll_not_built(self):
        # an earlier interceptor build: "SGSR 2 is not built into this interceptor -- the game's FSR 2 runs"
        lg = static(FSR4 + r"[20:27:45.942] vela: live (Z:\tmp\vela-live\2050650.ini): model sgsr2 lossy 1 npu 2" "\n"
                    r"[20:27:45.986] vela: SGSR 2 is not built into this interceptor -- the game's FSR 2 runs" "\n"
                    r"[20:27:45.986] vela: model fsr2 runs 640x360 -> 1920x1080 (the game's own FSR 2)" "\n"
                    r"[20:27:59.854] fps 37.1 (presents 372 in 10.0 s; frames by Vela 0), the game's FSR 2" "\n")
        self.assertEqual((lg["mode"], lg["sgsr2_fallback"]), ("fsr2", "failed"), lg)

    def test_fsr3_fallback(self):
        lg = static(FSR4 + FSR3.replace("vela: model fsr3 runs 640x360 -> 1920x1080",
                                        "vela: model fsr2 runs 640x360 -> 1920x1080 (the game's own FSR 2)")
                    .replace("frames by Vela 360), FSR 3.1", "frames by Vela 0), the game's FSR 2"))
        self.assertEqual((lg["mode"], lg["fsr3_fallback"]), ("fsr2", "failed"), lg)

    def test_not_offered(self):
        lg = static(SGSR2 + NOT_OFFERED)
        self.assertEqual((lg["not_offered"], lg["mode"], lg["sgsr3"], lg["fps_model"]), ("ai", "fsr2", False, "fsr2"), lg)
        self.assertIsNone(lg["fsr4_fallback"]); self.assertIsNone(lg["sgsr2_fallback"])
        lg = static(SGSR2 + NOT_OFFERED + BACK_FSR4)
        self.assertEqual((lg["not_offered"], lg["mode"]), (None, "fsr4"), lg)

    def test_switch_not_there_yet(self):
        # live changed to fsr4, no run / fps line since: SGSR 2 still runs (the panel says "switching")
        lg = static(SGSR2 + r"[20:44:43.312] vela: live (Z:\tmp\vela-live\2050650.ini): model fsr4 lossy 1 npu 0" "\n")
        self.assertEqual((lg["mode"], lg["fsr4_fallback"]), ("sgsr2", None), lg)

    def test_old_dll_wrong_label(self):
        # 0.8.12 DLL: the fps line said "Vela AI GPU 1.51 ms" while FSR 3.1 ran: the run line wins
        lg = static(r"[19:30:00.000] vela: live (Z:\tmp\vela-live\2050650.ini): model fsr3 lossy 1 npu 2" "\n"
                    r"[19:30:00.100] vela: model fsr3 runs 640x360 -> 1920x1080" "\n"
                    r"[19:30:10.000] fps 40.0 (presents 400 in 10.0 s; frames by Vela 400), Vela AI GPU 1.51 ms" "\n")
        self.assertEqual((lg["mode"], lg["sgsr3"]), ("fsr3", True), lg)

    def test_console_log_excerpts(self):
        # lines copied verbatim from two console runs (tests/fixtures): the last model that ran wins
        for name, mode in (("intercept_sgsr2_fsr3.log", "fsr3"), ("intercept_fsr3_fsr4.log", "fsr4")):
            path = os.path.join(HERE, "fixtures", name)
            lg = sgsr3.static_log_summary(path)
            self.assertIsNotNone(lg, path)
            self.assertEqual((lg["mode"], lg["render"], lg["output"]), (mode, "640x360", "1920x1080"), (path, lg))


# ---- OptiScaler fork (OptiScaler.log), patch 0017 ------------------------------------------------------------------
CTL_HEAD = (r"[20:04:50.201242] [I] InitInternal Initializing SGSR3 960x540 -> 1920x1080" "\n"
            r"[20:04:50.226999] [I] Poll Vela live (Z:\tmp\vela-live\870780.ini): model fsr4 ratio -1 amax -1 alphak -1 mk -1 "
            r"fresh -1 sq -1 ncap -1 disocc -1 lossy 1 npu 0" "\n"
            r"[20:04:50.227074] [I] CreatePipelines SGSR3: Vela live model fsr4" "\n")
CTL_DOWN = (r"[20:05:00.000000] [W] Evaluate SGSR3: FSR 4 VK is not available here; SGSR 2 instead" "\n"
            r"[20:05:00.010000] [W] TryEvaluateOptiFeature Vela live: fsr4 cannot run in this game; SGSR 2 instead" "\n"
            r"[20:05:00.010100] [I] TryEvaluateOptiFeature Vela live: handle 1 model fsr4 -> SGSR 2 (the game draws 960x540)" "\n"
            r"[20:05:00.020000] [I] InitInternal Initializing SGSR2 960x540 -> 1920x1080" "\n")


def opti(text):
    p = os.path.join(TMP, "OptiScaler.log")
    with open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return sgsr3.log_summary(p)


class ForkContract(unittest.TestCase):
    def test_fsr4_runs(self):
        lg = opti(CTL_HEAD + r"[20:04:50.300000] [I] Init Vela FSR 4 VK: FSR 4.1.1 VK INT8 context 960x540 -> 1920x1080 (hdr true)" "\n")
        self.assertEqual((lg["mode"], lg["fsr4_fallback"], lg["sgsr2_instead"]), ("fsr4", None, None), lg)

    def test_fsr4_cannot_run_sgsr2_instead(self):
        lg = opti(CTL_HEAD + CTL_DOWN)
        self.assertEqual((lg["mode"], lg["fsr4_fallback"], lg["sgsr2_instead"], lg["render"]),
                         ("sgsr2", "failed", "fsr4", "960x540"), lg)
        self.assertTrue(lg["sgsr3"])

    def test_old_lite_size_sgsr2_instead(self):
        lite = CTL_HEAD.replace("model fsr4", "model fsr4lite")
        lg = opti(lite + r"[20:05:00.000000] [W] Init SGSR3: FSR 4.1 Lite runs only from 645x360 to 1920x1080 render -> "
                  r"1920x1080, not 640x360 -> 1920x1080; SGSR 2 instead" "\n"
                  + CTL_DOWN.replace("fsr4 cannot", "fsr4lite cannot").replace("model fsr4 ->", "model fsr4lite ->")
                  .replace("Evaluate SGSR3: FSR 4 VK is not available here; SGSR 2 instead\n", "Evaluate -\n"))
        self.assertEqual((lg["mode"], lg["fsr4_fallback"], lg["sgsr2_instead"]), ("sgsr2", "failed", "fsr4"), lg)

    def test_sgsr2_picked_after_down(self):
        # the player picks SGSR 2 himself afterwards: no fallback any more
        lg = opti(CTL_HEAD + CTL_DOWN +
                  r"[20:06:00.000000] [I] TryEvaluateOptiFeature Vela live: handle 1 model sgsr2 -> SGSR 2 (the game draws 960x540)" "\n"
                  r"[20:06:00.010000] [I] InitInternal Initializing SGSR2 960x540 -> 1920x1080" "\n")
        self.assertEqual((lg["mode"], lg["fsr4_fallback"], lg["sgsr2_instead"]), ("sgsr2", None, None), lg)

    def test_fsr3_after_down(self):
        lg = opti(CTL_HEAD + CTL_DOWN +
                  r"[20:06:00.000000] [I] TryEvaluateOptiFeature Vela live: handle 1 model fsr3 -> FSR 3.1.4 (the game draws 960x540)" "\n"
                  "Vela: FFX upscaler index 0 (3.1.4) for FSR 3\n")
        self.assertEqual((lg["mode"], lg["fsr4_fallback"], lg["sgsr2_instead"]), ("fsr3", None, None), lg)


if __name__ == "__main__":
    unittest.main()
