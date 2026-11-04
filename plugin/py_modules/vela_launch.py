#!/usr/bin/python3 -I
"""vela-launch <command> [args...]: starts a game with its Vela kit, without writing into the game folder.

The plugin keeps a kit per game in its settings folder (<kits>/<appid>/: files/ with OptiScaler and the FSR 4 provider,
launch.json with the game folder and Vela's options). This launcher shows the kit's files inside the game's exe folder
in a private mount namespace (bwrap, the same way as Armada's armada-mod-launch), sets Vela's environment and runs the
game. Without a kit for the game, or when anything about the kit or the mount is wrong, it runs the command as it is.

It is installed by the plugin to ~/.local/share/vela/bin/vela-launch and needs nothing from the plugin, so a game keeps
starting after the plugin is uninstalled. It runs from Steam's launch options (vela-launch ... %command%) or, on Armada
builds with plugin launch wrappers, from armada-game-launch (plugin.json "armada": {"launch": {"wrapper": ...}})."""
import json
import os
import shlex
import subprocess
import sys
import time

KITS = os.path.expanduser("~/homebrew/settings/Vela/kits")  # the plugin writes its own path here when it installs this file
BWRAP = "bwrap"
CMD = "%command%"
DONE = "VELA_LAUNCH_DONE"  # set for the game: a second pass (launch options and the Armada wrapper) adds nothing
APPID_KEYS = ("STEAM_COMPAT_APP_ID", "SteamAppId", "SteamGameId")
APPEND = {"WINEDLLOVERRIDES": ";"}  # variables Vela adds to instead of replacing
LOG_MAX = 256 * 1024
MAX_BINDS = 4000  # bwrap takes 9000 arguments at most, the game's command line included


def log(msg, appid=None):
    """One line in the kit's launch.log (Vela's settings folder) and on stderr (Steam's log)."""
    print(f"vela-launch: {msg}", file=sys.stderr, flush=True)
    try:
        d = os.path.join(KITS, appid) if appid else KITS
        if not os.path.isdir(d):
            return
        path = os.path.join(d, "launch.log")
        if os.path.isfile(path) and os.path.getsize(path) > LOG_MAX:
            os.replace(path, path + ".1")
        with open(path, "a", encoding="utf-8") as f:
            f.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} {msg}\n")
    except OSError:
        pass


def app_id(env):
    for key in APPID_KEYS:
        value = env.get(key, "").strip()
        if value.isdigit():
            return value
    return None


def is_assignment(word):
    name, sep, _ = word.partition("=")
    return bool(sep) and bool(name) and (name[0].isalpha() or name[0] == "_") and \
        all(c.isalnum() or c == "_" for c in name)


def read_kit(appid, kits=None):
    """The game's launch.json, or None when the game has no kit that is on."""
    path = os.path.join(kits or KITS, appid, "launch.json")
    try:
        with open(path, encoding="utf-8") as f:
            kit = json.load(f)
    except FileNotFoundError:
        return None
    except (OSError, ValueError) as e:
        raise RuntimeError(f"launch.json unreadable: {e}")
    if not isinstance(kit, dict) or not kit.get("on"):
        return None
    for key in ("game_dir", "layer", "work_dir"):
        if not isinstance(kit.get(key), str) or not os.path.isabs(kit[key]):
            raise RuntimeError(f"launch.json: bad {key}")
    return kit


def split_options(options):
    """Vela's options for the game (env before %command%, game arguments after it) -> (env dict, args)."""
    words = shlex.split(options or "")
    ci = words.index(CMD) if CMD in words else len(words)
    env = {}
    for w in words[:ci]:
        if is_assignment(w):
            name, _, value = w.partition("=")
            env[name] = value
    return env, [w for w in words[ci + 1:]]


def merge_env(env, ours):
    """The environment for the game: the player's own value of a variable wins, WINEDLLOVERRIDES gets Vela's entries
    after his (joined with ';')."""
    out = dict(env)
    for name, value in ours.items():
        sep = APPEND.get(name)
        have = out.get(name)
        if sep and have:
            if value not in have.split(sep):
                out[name] = have + sep + value
        elif not have:
            out[name] = value
    return out


def mounts(game, layer):
    """bwrap arguments that show the kit (layer) in the game folder. The game folder is the overlay's upper layer, so the
    game's own writes land in the install as before. Every kit entry is then mounted read-only over its name: otherwise a
    file of the game with the same name (an older amd_fidelityfx_dx12.dll) would win over the kit's, and a write to a kit
    file would leave a copy in the install. A folder the game does not have is mounted as a whole, read-only."""
    binds = []

    def walk(rel):
        for name in sorted(os.listdir(os.path.join(layer, rel))):
            sub = os.path.join(rel, name)
            top, real = os.path.join(layer, sub), os.path.join(game, sub)
            if os.path.islink(top):
                raise RuntimeError(f"{sub} is a link in the kit")
            if not os.path.isdir(top):
                if os.path.isdir(real):
                    raise RuntimeError(f"{sub} is a file in the kit and a folder in the game")
                binds.extend(["--ro-bind", top, real])
            elif os.path.isdir(real):
                walk(sub)
            elif os.path.lexists(real):
                raise RuntimeError(f"{sub} is a folder in the kit and a file in the game")
            else:
                binds.extend(["--ro-bind", top, real])

    walk("")
    if not binds:
        raise RuntimeError("the kit has no files")
    if len(binds) > MAX_BINDS:
        raise RuntimeError("the kit has more files than can be mounted one by one")
    return binds


def bwrap_argv(game, layer, work, bwrap=BWRAP):
    """bwrap ... -- (the game's command goes after it). The work folder must be on the game folder's filesystem and
    outside it: the plugin puts it next to the game's install folder."""
    return [bwrap, "--dev-bind", "/", "/", "--overlay-src", layer, "--overlay", game, work, game,
            *mounts(game, layer), "--"]


def probe(argv, run=subprocess.run, timeout=30):
    """The same mounts with `true` as the program: (True, "") when they work, else (False, why)."""
    try:
        r = run([*argv, "true"], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                timeout=timeout)
    except (OSError, subprocess.SubprocessError) as e:
        return False, str(e)
    if r.returncode != 0:
        err = r.stderr.decode("utf-8", "replace").strip() if isinstance(r.stderr, bytes) else str(r.stderr or "")
        return False, err.splitlines()[-1] if err else f"exit {r.returncode}"
    return True, ""


def plan(args, env, kits=None, run=subprocess.run):
    """(command, environment) with the game's kit, or None: no kit for this game. Raises when the kit cannot be used."""
    appid = app_id(env)
    if not appid:
        return None
    kit = read_kit(appid, kits)
    if kit is None:
        return None
    game, layer, work = kit["game_dir"], kit["layer"], kit["work_dir"]
    if not os.path.isdir(game):
        raise RuntimeError(f"game folder {game} not found")
    if not os.path.isdir(layer):
        raise RuntimeError("kit folder not found")
    missing = [rel for rel in kit.get("files") or () if not os.path.isfile(os.path.join(layer, *rel.split("/")))]
    if missing:
        raise RuntimeError(f"kit incomplete: {', '.join(missing)}")
    ours, extra = split_options(kit.get("options"))
    os.makedirs(work, exist_ok=True)
    argv = bwrap_argv(game, layer, work, kit.get("bwrap") or BWRAP)
    ok, why = probe(argv, run)
    if not ok:
        raise RuntimeError(f"overlay probe failed: {why}")
    out = merge_env(env, ours)
    out[DONE] = "1"
    extra = [a for a in extra if a not in args]
    log(f"kit on: {game} ({(len(argv) - 11) // 3} entries), env {sorted(ours)}, args {extra}", appid)
    return [*argv, *args, *extra], out


def main(argv=None):
    args = list(sys.argv[1:] if argv is None else argv)
    if not args:
        sys.exit("usage: vela-launch <command> [args...]")
    # NAME=value words in front of the command: the player's own variables typed before %command% in Steam's options
    while args and is_assignment(args[0]):
        name, _, value = args.pop(0).partition("=")
        os.environ[name] = value
    if not args:
        sys.exit("vela-launch: no command")
    planned = None
    if not os.environ.get(DONE):
        try:
            planned = plan(args, dict(os.environ))
        except Exception as e:  # never in the way of the game: it starts without Vela
            log(f"Vela left out of this launch: {e}", app_id(os.environ))
    if planned:
        cmd, env = planned
        try:
            os.execvpe(cmd[0], cmd, env)
        except (OSError, ValueError) as e:
            log(f"overlay launch failed: {e}", app_id(os.environ))
    os.execvp(args[0], args)


if __name__ == "__main__":
    main()
