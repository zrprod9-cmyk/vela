"""The update check (py_modules/updater.py): the version compare, the 24 h cache and the off switch, the manual check,
the request itself (one URL, a plain User-Agent), and the zip checks before Decky installs anything (sha256 asset,
the .md5 fallback, mismatches, a zip that is not Vela). A fake urlopen stands in for GitHub.
Run: python -I tests/test_updater.py"""
import hashlib
import io
import json
import os
import sys
import tempfile
import urllib.error
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "py_modules"))
import updater  # noqa: E402

U = updater

# ---- the version compare
assert U.parse_version("v0.8.18") == (0, 8, 18) and U.parse_version("0.8.18") == (0, 8, 18)
for bad in ("", None, "v0.8", "0.8.18-rc1", "latest", "v0.8.x", "1.2.3.4"):
    assert U.parse_version(bad) is None, bad
assert U.is_newer("0.8.18", "0.8.17") and U.is_newer("v0.8.10", "0.8.9") and U.is_newer("1.0.0", "0.9.99")
assert U.is_newer("0.10.0", "0.9.0") and U.is_newer("0.8.100", "0.8.99")
assert not U.is_newer("0.8.18", "0.8.18") and not U.is_newer("0.8.9", "0.8.10") and not U.is_newer("0.8.17", "0.8.18")
assert not U.is_newer("garbage", "0.8.1") and not U.is_newer("0.8.20", "") and not U.is_newer(None, "0.8.1")

# ---- checksum files
H = "ab" * 32
assert U.parse_sum(f"{H}  vela-0.8.19.zip\n", "sha256") == H
assert U.parse_sum(H.upper(), "sha256") == H
assert U.parse_sum("6bf61d665bde0bad7434cee4d2db677b  vela-0.8.18.zip\n", "md5") == "6bf61d665bde0bad7434cee4d2db677b"
assert U.parse_sum("nothing here", "sha256") is None and U.parse_sum("", "md5") is None


def make_zip(version="0.8.19", name="Vela", folder="upscale", extra=None):
    b = io.BytesIO()
    with zipfile.ZipFile(b, "w") as z:
        z.writestr(f"{folder}/plugin.json", json.dumps({"name": name}))
        z.writestr(f"{folder}/package.json", json.dumps({"name": "upscale", "version": version}))
        z.writestr(f"{folder}/main.py", "class Plugin: pass\n")
        for n, v in (extra or {}).items():
            z.writestr(n, v)
    return b.getvalue()


BASE = "https://github.com/zrprod9-cmyk/vela/releases/download/"


def release(version="0.8.19", sha=True, md5=True, zip_=True, digest=None, tag=None, prefix=BASE, **kw):
    assets = []
    zn = f"vela-{version}.zip"
    if zip_:
        a = {"name": zn, "size": 1000, "browser_download_url": f"{prefix}v{version}/{zn}"}
        if digest:
            a["digest"] = "sha256:" + digest
        assets.append(a)
    if sha:
        assets.append({"name": zn + ".sha256", "browser_download_url": f"{prefix}v{version}/{zn}.sha256"})
    if md5:
        assets.append({"name": zn + ".md5", "browser_download_url": f"{prefix}v{version}/{zn}.md5"})
    assets.append({"name": "install-vela-npu.sh", "browser_download_url": f"{prefix}v{version}/install-vela-npu.sh"})
    d = {"tag_name": tag or f"v{version}", "draft": False, "prerelease": False, "assets": assets}
    d.update(kw)
    return d


class Resp:
    def __init__(self, data):
        self.b = io.BytesIO(data)

    def read(self, n=-1):
        return self.b.read(n)

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


class Fake:
    """GitHub: url -> bytes, an exception, or missing (404)."""
    def __init__(self, routes):
        self.routes = routes
        self.calls = []

    def __call__(self, req, timeout=None, context=None):
        self.calls.append({"url": req.full_url, "headers": dict(req.header_items()), "method": req.get_method(),
                           "data": req.data, "timeout": timeout, "context": context})
        v = self.routes.get(req.full_url)
        if v is None:
            raise urllib.error.HTTPError(req.full_url, 404, "Not Found", {}, None)
        if isinstance(v, Exception):
            raise v
        return Resp(v)


class Clock:
    def __init__(self, t=1_000_000.0):
        self.t = t

    def __call__(self):
        return self.t


def plugin(version):
    d = tempfile.mkdtemp(prefix="vela_upd_plugin_")
    json.dump({"name": "Vela"}, open(os.path.join(d, "plugin.json"), "w"))
    json.dump({"name": "upscale", "version": version}, open(os.path.join(d, "package.json"), "w"))
    return d


def updater_for(routes, version="0.8.18", settings=None, clock=None):
    fake = Fake(routes)
    s = settings or tempfile.mkdtemp(prefix="vela_upd_set_")
    up = U.Updater(s, plugin(version), log=lambda *_: None, urlopen=fake, now=clock or Clock(), ssl_context=lambda: "CTX")
    return up, fake, s


API = U.RELEASES_LATEST
api = lambda rel: json.dumps(rel).encode()

# ---- release_info
r = U.release_info(release(digest=H))
assert r["version"] == "0.8.19" and r["zip_url"].endswith("/v0.8.19/vela-0.8.19.zip") and r["digest"] == H
assert r["sha256_url"].endswith(".zip.sha256") and r["md5_url"].endswith(".zip.md5")
assert U.release_info(release(draft=True)) is None and U.release_info(release(prerelease=True)) is None
assert U.release_info(release(tag="nightly")) is None and U.release_info([]) is None and U.release_info(None) is None
assert U.release_info(release(zip_=False))["zip_url"] is None
# an asset URL outside the repository's releases is not used
assert U.release_info(release(prefix="https://example.org/x/"))["zip_url"] is None

# ---- the request: one GET to the releases API, a plain User-Agent, no body, a short timeout
up, fake, S = updater_for({API: api(release())})
st = up.check()
assert st["available"] and st["latest"] == "0.8.19" and st["current"] == "0.8.18" and st["error"] is None, st
assert len(fake.calls) == 1
c = fake.calls[0]
assert c["url"] == "https://api.github.com/repos/zrprod9-cmyk/vela/releases/latest" and c["method"] == "GET"
assert c["data"] is None and c["timeout"] == U.CHECK_TIMEOUT <= 15 and c["context"] == "CTX"
hdr = {k.lower(): v for k, v in c["headers"].items()}
assert hdr == {"user-agent": "Vela/0.8.18", "accept": "application/vnd.github+json"}, hdr

# ---- the cache: no second request inside 24 h, one after; the answer survives a new Updater (plugin reload)
clock = Clock()
up, fake, S = updater_for({API: api(release())}, clock=clock)
up.check()
clock.t += U.CHECK_EVERY - 60
assert not up.due()
st = up.check()
assert len(fake.calls) == 1 and st["available"]
up2 = U.Updater(S, up.plugin_dir, log=lambda *_: None, urlopen=fake, now=clock, ssl_context=lambda: "CTX")
assert up2.state()["latest"] == "0.8.19" and up2.state()["available"] and len(fake.calls) == 1
clock.t += 120
assert up.due()
up.check()
assert len(fake.calls) == 2
# a clock that went back does not freeze the check
clock.t -= 10 * U.CHECK_EVERY
assert up.due()
on_disk = json.load(open(os.path.join(S, "update.json")))
assert on_disk["latest"]["version"] == "0.8.19" and on_disk.get("enabled", True) is True

# ---- the same version installed: nothing to show; an older one on GitHub: nothing either
up, fake, S = updater_for({API: api(release("0.8.18"))}, version="0.8.18")
st = up.check()
assert not st["available"] and st["latest"] == "0.8.18"
up, fake, S = updater_for({API: api(release("0.8.17"))}, version="0.8.18")
assert not up.check()["available"]
# a release without the zip asset is not offered
up, fake, S = updater_for({API: api(release(zip_=False))})
assert not up.check()["available"]

# ---- the off switch: no automatic request at all; the manual check still works and does not turn it back on
clock = Clock()
up, fake, S = updater_for({API: api(release())}, clock=clock)
assert up.state()["enabled"] is True
st = up.set_enabled(False)
assert st["enabled"] is False and not up.due()
clock.t += 10 * U.CHECK_EVERY
st = up.check()
assert fake.calls == [] and st["latest"] is None
st = up.check(force=True)
assert len(fake.calls) == 1 and st["available"] and st["enabled"] is False
st = up.set_enabled(True)
assert st["enabled"] and st["available"]

# ---- the manual check ignores the 24 h cache and shows each result in place
clock = Clock()
routes = {API: api(release("0.8.18"))}
up, fake, S = updater_for(routes, version="0.8.18", clock=clock)
st = up.check()
assert not st["available"] and st["latest"] == "0.8.18"
routes[API] = api(release("0.8.19"))
clock.t += 60
assert up.check()["latest"] == "0.8.18" and len(fake.calls) == 1        # automatic: still cached
st = up.check(force=True)                                                 # manual: asks at once
assert len(fake.calls) == 2 and st["available"] and st["latest"] == "0.8.19"
routes[API] = urllib.error.URLError("Name or service not known")
st = up.check(force=True)
assert st["error"].startswith("network:") and len(fake.calls) == 3
assert st["latest"] == "0.8.19" and st["available"]                       # the last good answer stays

# ---- failures never raise; each is one short code in the state, and counts as the day's check
for answer, code in ((urllib.error.HTTPError(API, 403, "rate limited", {}, None), "github:403"),
                     (urllib.error.URLError("timed out"), "network:timed out"),
                     (TimeoutError("timed out"), "network:timed out"),
                     (b"not json", "bad_answer"),
                     (api({"message": "Not Found"}), "bad_answer"),
                     (b"x" * (U.MAX_ANSWER + 10), "bad_answer")):
    clock = Clock()
    up, fake, S = updater_for({API: answer}, clock=clock)
    st = up.check()
    assert st["error"] == code and not st["available"], (code, st)
    assert not up.due()
    clock.t += U.CHECK_EVERY
    assert up.due()
# a broken cache file is a first start
up, fake, S = updater_for({API: api(release())})
open(os.path.join(S, "update.json"), "w").write("{broken")
assert up.due() and up.state()["enabled"] and up.check()["available"]

# ---- install: the sha256 asset
Z = make_zip("0.8.19")
ZS = hashlib.sha256(Z).hexdigest()
ZM = hashlib.md5(Z).hexdigest()
zurl = BASE + "v0.8.19/vela-0.8.19.zip"
routes = {API: api(release(digest=ZS)), zurl: Z, zurl + ".sha256": f"{ZS}  vela-0.8.19.zip\n".encode(),
          zurl + ".md5": f"{ZM}  vela-0.8.19.zip\n".encode()}
up, fake, S = updater_for(dict(routes))
up.check()
p = up.prepare()
assert p == {"url": zurl, "name": "Vela", "version": "0.8.19", "hash": ZS, "install_type": 2}, p
assert [c["url"] for c in fake.calls] == [API, zurl + ".sha256", zurl]
assert all(c["data"] is None and c["method"] == "GET" for c in fake.calls)
assert all({k.lower(): v for k, v in c["headers"].items()}["user-agent"] == "Vela/0.8.18" for c in fake.calls)

# a .sha256 that does not match the zip: refused before Decky sees anything
bad = dict(routes)
bad[zurl + ".sha256"] = ("0" * 64 + "  vela-0.8.19.zip\n").encode()
up, fake, S = updater_for(bad)
up.check()
try:
    up.prepare()
    raise AssertionError("a wrong sha256 was accepted")
except U.UpdateError as e:
    assert str(e) == "hash_mismatch"

# ---- install: an older release without .sha256 -> the .md5 asset is checked, then the sha256 is computed
old = dict(routes)
old[API] = api(release(sha=False))
del old[zurl + ".sha256"]
up, fake, S = updater_for(old)
up.check()
p = up.prepare()
assert p["hash"] == ZS and [c["url"] for c in fake.calls] == [API, zurl + ".md5", zurl]
old_bad = dict(old)
old_bad[zurl + ".md5"] = b"00000000000000000000000000000000  vela-0.8.19.zip\n"
up, fake, S = updater_for(old_bad)
up.check()
try:
    up.prepare()
    raise AssertionError("a wrong md5 was accepted")
except U.UpdateError as e:
    assert str(e) == "hash_mismatch"
# neither checksum asset: no install at all, and the zip is not even downloaded
none = {API: api(release(sha=False, md5=False)), zurl: Z}
up, fake, S = updater_for(none)
up.check()
try:
    up.prepare()
    raise AssertionError("installed without a checksum")
except U.UpdateError as e:
    assert str(e) == "no_checksum" and zurl not in [c["url"] for c in fake.calls]
# GitHub's own digest of the asset disagrees: refused
dg = dict(routes)
dg[API] = api(release(digest="1" * 64))
up, fake, S = updater_for(dg)
up.check()
try:
    up.prepare()
    raise AssertionError("digest mismatch accepted")
except U.UpdateError as e:
    assert str(e) == "hash_mismatch"


# ---- install: a zip that is not this plugin of that version is refused (Decky would remove Vela first)
def refused(zipdata):
    zs = hashlib.sha256(zipdata).hexdigest()
    rt = {API: api(release()), zurl: zipdata, zurl + ".sha256": zs.encode()}
    up, fake, S = updater_for(rt)
    up.check()
    try:
        up.prepare()
    except U.UpdateError as e:
        return str(e)
    return None


assert refused(make_zip("0.8.18")) == "bad_zip"                  # version inside differs from the tag
assert refused(make_zip(name="Other")) == "bad_zip"              # another plugin
assert refused(make_zip(folder="vela")) == "bad_zip"             # another folder on the device
assert refused(make_zip(extra={"../evil.sh": "x"})) == "bad_zip"  # a file outside the plugin folder
assert refused(b"PK not a zip") == "bad_zip"
assert refused(make_zip()) is None

# ---- install with nothing newer: refused
up, fake, S = updater_for({API: api(release("0.8.18"))}, version="0.8.18")
up.check()
try:
    up.prepare()
    raise AssertionError("installed the same version")
except U.UpdateError as e:
    assert str(e) == "not_newer"
up, fake, S = updater_for({})
try:
    up.prepare()
    raise AssertionError("installed without a check")
except U.UpdateError as e:
    assert str(e) == "not_newer"

print("test_updater: ok")
