"""0.8.16: "Report a problem" -- anonymisation, bundle contents, the bad-run prompt (crash / hang / fallback
detection), the client rate limit and the upload to a local fake relay. The console fixtures are real lines logs and
the console's journal): the hangcheck / gpu fault kernel lines, a kept run without "Unloading OptiScaler".
Run: python -I tests/test_report.py"""
import http.server
import json
import os
import sys
import tempfile
import threading
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "py_modules"))
import report  # noqa: E402

tmp = tempfile.mkdtemp(prefix="vela_rep_")
HOME = "/var/home/armada"
SID = "76561198012345678"


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(data)


# 1) anonymisation
A = report.Anon(HOME, "armada", {"armada", "deckbox"}, {"SuperPlayer", "zrlogin"}, {SID, str(int(SID) - report.STEAM64_BASE)})
src = ("[08:31:24.1] path /var/home/armada/.local/share/Steam/steamapps/common/Control/Control_DX12.exe\n"
       "Z:\\var\\home\\armada\\Games\\x.exe and S:\\x5cvar\\x5chome\\x5carmada\\x5cfoo\n"
       "2026-11-02T13:37:17+01:00 armada kernel: wlan0 ether 3c:7a:aa:01:02:ff ip 192.168.1.201 fe80::1c2b:3cff:fe4d:5e6f\n"
       "user SuperPlayer (superplayer) zrlogin id 76561198012345678 [U:1:52079950] userdata/52079950/config 52079950\n"
       "TELEGRAM_BOT_TOKEN=123456789:AAHdqTcvCH1vGWJxfSeofSAs0K5PALDsawQ RUNPOD_API_KEY='abc' password: hunter2 Bearer abcdefghijkl\n"
       "keep: Mesa 26.2.4 Proton 11.0-100 127.0.0.1 ffx::Context std::vector 08:31:24 Armada 20261010 deckboxes host deckbox\n"
       "mail me@example.org\n")
out = A(src)
for bad in ("/var/home/armada", "\\var\\home\\armada", "3c:7a:aa", "192.168.1.201", "fe80::", "SuperPlayer", "superplayer",
            "zrlogin", SID, "52079950", "AAHdqTcv", "abc'", "hunter2", "abcdefghijkl", "me@example.org", " armada kernel",
            "host deckbox\n"):
    assert bad not in out, (bad, out)
for good in ("~/.local/share/Steam/steamapps/common/Control/Control_DX12.exe", "Mesa 26.2.4", "Proton 11.0-100", "127.0.0.1",
             "ffx::Context", "std::vector", "08:31:24", "Armada 20261010", "deckboxes", "<mac>", "<ip>", "<ip6>",
             "<steamid>", "[U:1:<id>]", "userdata/<id>/config", "<steam-name>", "<user> kernel", "host <host>", "RUNPOD_API_KEY=<redacted>"):
    assert good in out, (good, out)
assert A.obj({"/var/home/armada/x": ["armada", 3]}) == {"~/x": ["<user>", 3]}

# loginusers.vdf -> names and ids
write(os.path.join(tmp, "home", ".local/share/Steam/config/loginusers.vdf"),
      '"users"\n{\n\t"%s"\n\t{\n\t\t"AccountName"\t\t"zrlogin"\n\t\t"PersonaName"\t\t"Super Player"\n\t}\n}\n' % SID)
names, ids = report.steam_identities(os.path.join(tmp, "home"))
assert names == {"zrlogin", "Super Player"} and SID in ids and str(int(SID) - report.STEAM64_BASE) in ids, (names, ids)

# 2) crash / hang / fallback detection
HANG = ["2026-11-02T12:00:46+01:00 armada kernel: msm_dpu ae01000.display-controller: [drm:recover_worker] *ERROR* "
        "43050a01: hangcheck recover!",
        "2026-11-02T12:00:53+01:00 armada kernel: msm_dpu ae01000.display-controller: [drm:hangcheck_handler] *ERROR* "
        "43050a01: hangcheck detected gpu lockup rb 2!"]
FAULT = ["2026-11-02T13:09:37+01:00 armada kernel: *** gpu fault: ttbr0=0000000aa69f1000 iova=00000001309f08c0 dir=WRITE "
         "type=TRANSLATION source=CCU (3736059565,3736059565,3736059565,3736059565)"]
LONG = "[11:41:35.809168] [W]  OptiScaler v10.0.0-dev (f80e4da3) (2026-11-01) loaded\n[11:52:00.1] [I] frame\n"
SHORT = "[11:41:35.809168] [W]  OptiScaler v10.0.0-dev loaded\n[11:42:10.1] [I] frame\n"
CLEAN = LONG + "[11:52:01.437861] [I]  DLL_PROCESS_DETACH\n[11:52:01.437886] [I]  Unloading OptiScaler\n"
assert report.log_duration(LONG) == 625
f = report.assess(CLEAN, {"mode": "fsr4"}, [])
assert not f["bad"] and not f["no_clean_exit"], f
f = report.assess(LONG, {"mode": "fsr4"}, [], run_s=900)  # Steam's exit kills the game: no line, a long run -> no prompt
assert f["no_clean_exit"] and not f["short_run"] and not f["bad"], f
f = report.assess(SHORT, {"mode": "fsr4"}, [], run_s=35)
assert f["short_run"] and f["bad"] and f["why"] == ["short_run"], f
f = report.assess(SHORT, {"mode": "fsr4"}, [])  # run length unknown (the log's span is not one): no prompt from it alone
assert not f["short_run"] and not f["bad"] and f["log_span_s"] == 35, f
f = report.assess(CLEAN, {"mode": "fsr4"}, HANG + FAULT)
assert f["gpu_hang"] == 2 and f["gpu_faults"] == 1 and f["why"] == ["gpu_hang", "gpu_faults"], f
f = report.assess(CLEAN, {"mode": "sgsr2", "fsr4_fallback": "failed", "sgsr2_instead": "fsr4"}, [])
assert f["why"] == ["fsr4_fallback", "sgsr2_instead"], f
f = report.assess(CLEAN, {"mode": "fsr4", "npu": {"state": "fallback", "reason": "size", "fallbacks": 11}}, [])
assert f["npu_fallback"] == "size" and f["npu_fallbacks"] == 11 and f["bad"], f
f = report.assess("[22:41:00.639] fsr2 static recon attached\n[22:41:08.9] x\n", {}, [], intercept=True)
assert not f["no_clean_exit"] and not f["bad"], f  # the interceptor's log has no exit line to miss


# 3) the bundle: a fake plugin + game around the Reporter
class S3:
    def __init__(self, root):
        self.logs_dir = os.path.join(root, "settings", "sgsr3-logs")
        self.root = root

    def scan(self, appid):
        d = os.path.join(self.root, "lib", "steamapps", "common", "Control")
        return {"exe_dir": d, "install_dir": d}

    def kit(self, appid):
        return {"log": "OptiScaler.log", "static": None}


class Npu:
    def __init__(self, d):
        self.d = d

    def layer_log(self, appid):
        return os.path.join(self.d, f"layer-{appid}.log")


class Live:
    def get(self, appid):
        return {"model": "fsr4", "npu": 1}


class Done:
    def __init__(self, out):
        self.returncode, self.stdout = 0, out


def runner(args, **kw):
    if args[0] == "journalctl":
        lines = HANG + FAULT + ["2026-11-02T12:00:40+01:00 armada kernel: wlan0: associated 3c:7a:aa:01:02:ff"] if "_TRANSPORT=kernel" in args \
            else ["2026-11-02T12:00:41+01:00 armada gamescope-session-plus[2194]: [gamescope] [Info]  xwm: Screenshot saved "
                  "to /var/home/armada/shots/x.png", "2026-11-02T12:00:41+01:00 armada sshd[1]: Accepted from 192.168.1.5"]
        return Done("\n".join(lines) + "\n")
    if args[0] == "rpm":
        return Done("mesa-vulkan-drivers-26.2.4-1.fc44.armada.aarch64\n")
    raise OSError("no such tool")


T0 = 1793620000.0
clock = [T0]
plugin = os.path.join(tmp, "plugin")
write(os.path.join(plugin, "package.json"), json.dumps({"version": "0.8.16"}))
write(os.path.join(plugin, "assets", "sgsr3", "VERSION.json"), json.dumps({"dll": "vela-0.8.16-mvo"}))
write(os.path.join(plugin, "assets", "sgsr3", "OptiScaler.dll"), "dll")
game = os.path.join(tmp, "lib", "steamapps", "common", "Control")
write(os.path.join(game, "OptiScaler.ini"), "[Log]\nLogFile=/var/home/armada/x.log\n")
write(os.path.join(game, "dxgi.dll"), "dll")
write(os.path.join(game, "savegame.sav"), "SAVE")  # never in the bundle
write(os.path.join(game, "screenshot.png"), "PNG")
write(os.path.join(tmp, "lib", "steamapps", "compatdata", "870780", "version"), "11.0-100\n")
write(os.path.join(tmp, "settings", ".env"), "TELEGRAM_BOT_TOKEN=x\n")
s3 = S3(tmp)
kept = os.path.join(s3.logs_dir, "870780-20261102-120050.log")
write(kept, SHORT + "[11:42:11.0] [I] Vela NPU: fallback size 640x360->1280x720, GPU path\n"
      "[11:42:12.0] [I] LogFile Z:\\var\\home\\armada\\.local\\share\\Steam\\steamapps\\common\\Control\\OptiScaler.log\n")
os.utime(kept, (T0 - 100, T0 - 100))
npud = os.path.join(tmp, "settings", "npu")
write(os.path.join(npud, "layer-870780.log"), "vela npu layer loaded 12; daemon /var/home/armada/x\n")
write(os.path.join(tmp, "livetmp", "870780.ini"), "model=fsr4\nnpu=1\n")
logs_dir = os.path.join(tmp, "decky-logs")
write(os.path.join(logs_dir, "2026-11-02 12.00.00.log"), "[upscale] INFO sgsr3: 870780 on in /var/home/armada/g\n[x] other\n")
os_rel = os.path.join(tmp, "os-release")
write(os_rel, 'NAME="Armada"\nPRETTY_NAME="Armada 20261010.c3d178c"\nBUILD_ID="20261010.c3d178c"\n')
logged = []
R = report.Reporter(os.path.join(tmp, "settings"), HOME, plugin, s3=s3, npu=Npu(npud), live=Live(), log=logged.append,
                    runner=runner, now=lambda: clock[0], plugin_log_dir=logs_dir, anon=A,
                    live_tmp=os.path.join(tmp, "livetmp"), os_release_path=os_rel)
assert R.relay() == report.DEFAULT_RELAY and R.panel()["configured"] == bool(report.DEFAULT_RELAY)  # the deployed relay
report.DEFAULT_RELAY = ""  # the rest of the test never talks to the real relay

# the prompt after the run: once per run, opt-in; the run length from observe()
R.observe([])
R.observe(["870780"])
clock[0] += 40
R.observe([])
clock[0] -= 40
e = R.on_run_end("870780", kept)
assert e["flags"]["duration_s"] == 40, e
assert e and e["state"] == "ask" and "gpu_hang" in e["why"] and "short_run" in e["why"] and "npu_fallback" in e["why"], e
assert R.on_run_end("870780", kept) is None  # one prompt per run
assert R.panel()["pending"]["key"] == "870780-20261102-120050.log"

gs = {"appid": "870780", "scan": {"install_dir": game}, "sgsr3": {"log": None}}
p = R.build("870780", "crash", "\u0432\u044b\u043b\u0435\u0442\u0435\u043b\u0430 \u043d\u0430 \u0441\u0442\u0430\u0440\u0442\u0435, /var/home/armada", "VELA_NPU_LOG_FILE=/var/home/armada/n.log %command%",
            gs, "Control", "870780-20261102-120050.log", False, "prompt")
names = [x["name"] for x in p["files"]]
for n in ("optiscaler.log", "npu_layer.log", "game_state.json", "live.json", "launch_options.txt", "versions.json",
          "kernel_gpu.txt", "gamescope.txt", "plugin.log", "OptiScaler.ini", "meta.json", "README.txt"):
    assert n in names, (n, names)
assert not any(report.DENY_RX.search(n) for n in names)
z = zipfile.ZipFile(p["path"])
assert sorted(z.namelist()) == sorted(names) and p["zip_bytes"] <= report.MAX_ZIP
blob = "\n".join(z.read(n).decode("utf-8") for n in z.namelist())
for bad in ("/var/home/armada", "\\var\\home\\armada", "3c:7a:aa", "192.168.1.5", "SAVE", "TELEGRAM_BOT_TOKEN=x", " armada "):
    assert bad not in blob, bad
meta = json.loads(z.read("meta.json"))
assert meta["reason"] == "crash" and meta["appid"] == "870780" and meta["trigger"] == "prompt", meta
assert meta["flags"]["gpu_hang"] == 2 and meta["flags"]["short_run"] and meta["plugin"] == "0.8.16", meta
assert meta["os"] == "Armada 20261010.c3d178c" and meta["mesa"].startswith("mesa-vulkan-drivers-26.2.4"), meta
assert meta["proton"] == "11.0-100" and meta["text"] == "\u0432\u044b\u043b\u0435\u0442\u0435\u043b\u0430 \u043d\u0430 \u0441\u0442\u0430\u0440\u0442\u0435, ~", meta
assert "kernel: wlan0" not in z.read("kernel_gpu.txt").decode()  # only GPU / drm lines
assert "sshd" not in z.read("gamescope.txt").decode()
v = json.loads(z.read("versions.json"))["versions"]
assert v["optiscaler"] == "vela-0.8.16-mvo" and v["md5"]["sgsr3/OptiScaler.dll"], v
with open(p["path"][:-4] + ".json", encoding="utf-8") as f:
    assert json.load(f)["status"] == "saved"
assert R.saved()[0]["id"] == p["id"]

# not configured: send refuses, the zip stays (the built-in relay is switched off for this check, so a test never
# reaches the real one)
_default_relay, report.DEFAULT_RELAY = report.DEFAULT_RELAY, ""
try:
    R.send(p["id"])
    raise AssertionError("sent without a relay")
except RuntimeError as ex:
    assert str(ex) == "not_configured"
finally:
    report.DEFAULT_RELAY = _default_relay

# 4) upload to a local fake relay + the rate limit (3 / hour)
got = []


class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers["Content-Length"]))
        got.append((self.headers["Content-Type"], body))
        ans = json.dumps({"ok": True, "id": f"VELA-ABC{len(got)}23"}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(ans)

    def log_message(self, *a):
        pass


srv = http.server.HTTPServer(("127.0.0.1", 0), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
R2 = report.Reporter(os.path.join(tmp, "settings"), HOME, plugin, s3=s3, npu=Npu(npud), live=Live(), log=logged.append,
                     runner=runner, now=lambda: clock[0], plugin_log_dir=logs_dir, anon=A,
                     live_tmp=os.path.join(tmp, "livetmp"), os_release_path=os_rel)
R2.set_cfg(relay_url="http://example.org/")  # plain http to another host: refused
assert R2.relay() == ""
R2.set_cfg(relay_url=f"http://127.0.0.1:{srv.server_port}/report")
assert R2.relay()
r = R2.send(p["id"])
assert r == {"report_id": "VELA-ABC123"}, r
ctype, body = got[0]
assert ctype.startswith("multipart/form-data; boundary=") and b'name="meta"' in body and b'filename="vela-870780-' in body
assert body.count(b"PK\x03\x04") >= 1 and R2.send(p["id"])["already"]  # sent once; a second press does not upload again
assert R2.panel()["pending"] is None  # the prompt of that run is answered
assert R2.rate_left() == 2
ids = []
for k in range(2):
    clock[0] += 61
    ids.append(R2.build("870780", "fsr", "", None, None, "Control")["id"])
    R2.send(ids[-1])
assert R2.rate_left() == 0
clock[0] += 61
p4 = R2.build("870780", "slow", "", None, None, "Control")
try:
    R2.send(p4["id"])
    raise AssertionError("4th upload in an hour")
except RuntimeError as ex:
    assert str(ex).startswith("rate_limited:"), ex
assert len(got) == 3
clock[0] += report.RATE_WINDOW
assert R2.send(p4["id"])["report_id"] == "VELA-ABC423"  # the zip was kept: resent later
# bad answers / bad input
try:
    R2.build("870780", "nope")
    raise AssertionError
except ValueError:
    pass
try:
    R2.build("../x", "crash")
    raise AssertionError
except ValueError:
    pass
# "Don't ask again"
R2.dismiss("x", never=True)
assert not R2.auto_prompt() and R2.on_run_end("870780", kept) is None
srv.shutdown()
print("test_report: OK")
