"""0.8.17: Steam launch options, edited token by token. Vela adds its own tokens and later takes out exactly those, and
nothing else: every token that was there before Vela, and every token the player adds later, stays as it was, in the same
order, the same place relative to %command% and with the same spacing and quotes.

Before 0.8.17 the modules cleaned the options by pattern (every WINEDLLOVERRIDES="dxgi=n,b", VKD3D_SHADER_MODEL=6_6,
VKD3D_FEATURE_LEVEL=12_2, OPTI_SGSR3_*, VELA_*, -dx12 / -dx11 ...) and collapsed the spacing. A game that needs some of
those itself (FINAL FANTASY VII REBIRTH: VKD3D_FEATURE_LEVEL=12_2 VKD3D_SHADER_MODEL=6_6) lost them when Vela was set up,
and again on Off when the player had typed them back.

The record of what was added: {"env": [tokens put before everything], "args": [tokens put after everything],
"cmd": True when Vela put %command% in (the options had none)}."""
import collections
import re

CMD = "%command%"
# a token: a run of non-space characters; a space inside "..." or '...' does not end it (an unclosed quote runs to the end)
_RX_TOKEN = re.compile(r"""(?:"[^"]*"?|'[^']*'?|[^\s"'])+""")
_RX_NAME = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=")


def tokens(options):
    """[(text, start, end)] of the options (None: no tokens)."""
    return [(m.group(), m.start(), m.end()) for m in _RX_TOKEN.finditer(options or "")]


def words(options):
    return [t for t, _, _ in tokens(options)]


def env_name(tok):
    """FOO for FOO=bar (an environment assignment), else None."""
    m = _RX_NAME.match(tok)
    return m.group(1) if m else None


def _cmd_index(toks):
    for i, (t, _, _) in enumerate(toks):
        if t == CMD:
            return i
    return None


def empty_record():
    return {"env": [], "args": [], "cmd": False}


def _cut(s, start, end, prefer_after):
    """s without s[start:end] and the one space next to it that add put there (after a token Vela put in front, before
    a token Vela put at the end; the other side when that one has none), so add followed by remove gives back the very
    same text, the player's own spacing included."""
    after = end < len(s) and s[end].isspace()
    before = start > 0 and s[start - 1].isspace()
    if prefer_after and after or not before and after:
        return s[:start] + s[end + 1:]
    if before:
        return s[:start - 1] + s[end:]
    return s[:start] + s[end:]


def add(options, env=(), args=()):
    """(options with Vela's tokens in, the record of what was added). env goes in front of everything (before the player's
    own variables and wrappers, in the given order); a variable the options already set (the same NAME=, whatever value)
    is not added: the player's value wins. args go at the very end; one the options already pass after %command% is not
    added. Options without %command% get it in front of them (Steam appends options without it to the command line, so
    the game gets the same arguments). None counts as no options."""
    s = options or ""
    rec = empty_record()
    if CMD not in words(s):
        s = CMD + (" " + s if s.strip() else "")
        rec["cmd"] = True
    toks = tokens(s)
    ci = _cmd_index(toks)
    before = [t for t, _, _ in toks[:ci]]
    after = [t for t, _, _ in toks[ci + 1:]]
    names = {env_name(t) for t in before} - {None}
    front = []
    for tok in env:
        n = env_name(tok)
        if tok in before or tok in front or (n and n in names):
            continue
        front.append(tok)
        if n:
            names.add(n)
    back = []
    for tok in args:
        if tok in after or tok in back:
            continue
        back.append(tok)
    if front:
        s = " ".join(front) + " " + s
    if back:
        s = s + " " + " ".join(back)
    rec["env"], rec["args"] = front, back
    return s, rec


def remove(options, rec):
    """The options without exactly the tokens the record names (each once: an env token the first time it stands before
    %command%, an arg the last time it stands after it) and without the %command% Vela put in when it is still the first
    token. Everything else stays byte for byte. None stays None."""
    if options is None:
        return None
    rec = rec or {}
    s = options
    for tok in rec.get("env") or ():
        toks = tokens(s)
        ci = _cmd_index(toks)
        for t, a, b in toks[:ci] if ci is not None else toks:
            if t == tok:
                s = _cut(s, a, b, True)
                break
    for tok in rec.get("args") or ():
        toks = tokens(s)
        ci = _cmd_index(toks)
        for t, a, b in reversed(toks[ci + 1:] if ci is not None else toks):
            if t == tok:
                s = _cut(s, a, b, False)
                break
    if rec.get("cmd"):
        toks = tokens(s)
        if toks and toks[0][0] == CMD and [t for t, _, _ in toks].count(CMD) == 1:
            s = _cut(s, toks[0][1], toks[0][2], True)
            if not s.strip():
                s = ""
    return s


def replace(options, rec, env=(), args=()):
    """Vela's earlier tokens (rec) out, the new ones in: (options, new record). The player's tokens are not touched."""
    base = remove(options, rec)
    new, out = add(base, env, args)
    if (rec or {}).get("cmd") and not out["cmd"] and CMD in words(base):
        out["cmd"] = True  # Vela's %command% stayed (the player put a token in front of it): still Vela's
    return new, out


def derive(before, set_by_us, ours, extra_args=()):
    """The record of a setup made before 0.8.17 (no record in its state): the tokens of the options Vela wrote (set_by_us)
    that look like Vela's (ours(token) is True) and that the options before Vela (before) did not have, plus the given args
    (a -dx12 the state says Vela added) when set_by_us has one more of them than before; %command% when before had none.
    before None (not read then): every token of Vela's look counts as Vela's."""
    rec = empty_record()
    if not isinstance(set_by_us, str):
        return rec
    prev = collections.Counter(words(before)) if isinstance(before, str) else None
    toks = words(set_by_us)
    ci = toks.index(CMD) if CMD in toks else None
    left = collections.Counter(prev) if prev is not None else None
    for t in toks[:ci] if ci is not None else toks:
        if not ours(t):
            continue
        if left is not None and left[t] > 0:
            left[t] -= 1
            continue
        rec["env"].append(t)
    tail = toks[ci + 1:] if ci is not None else []
    for a in extra_args:
        if tail.count(a) > (prev or collections.Counter())[a]:
            rec["args"].append(a)
    rec["cmd"] = ci is not None and isinstance(before, str) and CMD not in words(before)
    return rec
