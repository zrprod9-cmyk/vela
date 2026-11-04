"""How Vela gets its kit into a game (0.8.20).

- "wrapper": the Armada build runs Decky plugin launch wrappers (plugin.json "armada": {"launch": {"wrapper": ...}}):
  vela-launch runs before armada-game-launch for every game, and Steam's launch options are not edited at all.
- "launcher": Steam's launch options get one token, the launcher, in front of the command; Vela's variables and game
  arguments are in the kit (launch.json), not in the options.
- "copy": the files are copied into the game's exe folder and Vela's tokens go into the options, as before 0.8.20.
  Used when bwrap or its overlay does not work on the device or for the game's folder.

In the first two the kit is shown to the game by an overlay mount at launch (vela_launch.py) and nothing is written into
the game folder."""
import json
import os
import shutil
import subprocess

import vela_launch

LAUNCHER_REL = ".local/share/vela/bin/vela-launch"
LAUNCHER_TOKEN = "~/" + LAUNCHER_REL  # Steam runs the options through a shell: ~ is the player's home
ARMADA_LAUNCH = "/usr/libexec/armada/armada-game-launch"
MODES = ("wrapper", "launcher", "copy")
_KITS_LINE = 'KITS = os.path.expanduser("~/homebrew/settings/Vela/kits")'


def launcher_path(home):
    return os.path.join(home, LAUNCHER_REL)


def install_launcher(home, kits_dir, src=None):
    """The launcher into ~/.local/share/vela/bin (outside the plugin: a game keeps starting after an uninstall), with
    this plugin's kits folder written in. Atomic; returns its path."""
    src = src or os.path.join(os.path.dirname(os.path.abspath(__file__)), "vela_launch.py")
    with open(src, encoding="utf-8") as f:
        text = f.read()
    if _KITS_LINE not in text:
        raise RuntimeError("vela_launch.py: kits line not found")
    text = text.replace(_KITS_LINE, f"KITS = {json.dumps(os.path.abspath(kits_dir))}")
    dst = launcher_path(home)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    try:
        with open(dst, encoding="utf-8") as f:
            if f.read() == text and os.access(dst, os.X_OK):
                return dst
    except OSError:
        pass
    tmp = dst + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    os.chmod(tmp, 0o755)
    os.replace(tmp, dst)
    return dst


def wrapper_supported(path=ARMADA_LAUNCH):
    """The device's armada-game-launch runs Decky plugin wrappers (Armada PR 839)."""
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError:
        return False
    return "plugin_wrappers" in text or ('"armada"' in text and '"wrapper"' in text)


def plugin_declares_wrapper(plugin_dir):
    try:
        with open(os.path.join(plugin_dir, "plugin.json"), encoding="utf-8") as f:
            data = json.load(f)
        return isinstance(data["armada"]["launch"]["wrapper"], str)
    except (OSError, ValueError, KeyError, TypeError):
        return False


def work_dir(install_dir, exe_dir, appid):
    """The overlay's work folder: next to the game's install folder (same filesystem, outside the game)."""
    base = install_dir or exe_dir
    return os.path.join(os.path.dirname(os.path.normpath(base)), f".vela-{appid}")


def probe_game(exe_dir, work, scratch, run=subprocess.run, bwrap=None):
    """Whether the overlay works over this game's exe folder: the launcher's mounts with a one-file kit and `true`.
    (ok, why)."""
    if not (bwrap or shutil.which("bwrap")):
        return False, "bwrap not found"
    try:
        os.makedirs(scratch, exist_ok=True)
        with open(os.path.join(scratch, ".vela-probe"), "w") as f:
            f.write("probe\n")
        os.makedirs(work, exist_ok=True)
        argv = vela_launch.bwrap_argv(exe_dir, scratch, work, bwrap or "bwrap")
    except (OSError, RuntimeError) as e:
        return False, str(e)
    return vela_launch.probe(argv, run)


def tidy_work(work):
    """The overlay's work folder after the game: overlayfs leaves an empty work/ in it. Only empty folders go."""
    for d in (os.path.join(work, "work"), work):
        try:
            os.rmdir(d)
        except OSError:
            pass


def log_tail(kits_dir, appid, lines=20):
    path = os.path.join(kits_dir, str(appid), "launch.log")
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()[-lines:]
    except OSError:
        return []
