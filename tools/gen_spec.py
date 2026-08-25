#!/usr/bin/env python3
"""Dev-time spec generator: probes the reference executable and writes spec/ files.

Not used by compile.sh. Run manually: python3 tools/gen_spec.py
"""
import json
import os
import re
import subprocess
import sys

ORACLE = os.environ.get("ORACLE", "/home/agent/oracle/executable")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SPEC = os.path.join(ROOT, "spec")


def run(args, **kw):
    return subprocess.run([ORACLE] + args, capture_output=True, **kw)


def cmdid(path):
    return "_".join(path) if path else "ROOT"


def main():
    os.makedirs(os.path.join(SPEC, "help"), exist_ok=True)
    os.makedirs(os.path.join(SPEC, "jusage"), exist_ok=True)
    os.makedirs(os.path.join(SPEC, "misc"), exist_ok=True)

    r = run(["--json-usage"])
    full = json.loads(r.stdout)

    paths = []

    def walk(n, path):
        paths.append(path)
        for s in n.get("sub_algorithms", []):
            walk(s, path + [s["name"]])

    walk(full, [])

    # shortcut algorithms present at root but hidden from the sub_algorithms tree
    for extra in (["convert"], ["info"]):
        if extra not in paths:
            paths.append(extra)

    tree = {}
    for p in paths:
        name = cmdid(p)
        h = run(p + ["--help"])
        with open(os.path.join(SPEC, "help", name + ".txt"), "wb") as f:
            f.write(h.stdout)
        with open(os.path.join(SPEC, "help", name + ".err"), "wb") as f:
            f.write(h.stderr)
        j = run(p + ["--json-usage"])
        with open(os.path.join(SPEC, "jusage", name + ".json"), "wb") as f:
            f.write(j.stdout)
        with open(os.path.join(SPEC, "jusage", name + ".err"), "wb") as f:
            f.write(j.stderr)
        ju = json.loads(j.stdout)
        node = {
            "name": p[-1] if p else "gdal",
            "path": p,
            "description": ju.get("description", ""),
            "short_url": ju.get("short_url", ""),
            "url": ju.get("url", ""),
            "sub": [s["name"] for s in ju.get("sub_algorithms", [])],
            "args": [],
        }
        for kind in ("input_arguments", "output_arguments", "input_output_arguments"):
            for a in ju.get(kind, []):
                a2 = dict(a)
                a2["kind"] = kind
                node["args"].append(a2)
        helptxt = h.stdout.decode()
        node["usage_line"] = helptxt.split("\n", 1)[0] if helptxt else ""
        parse_help(helptxt, node)
        if node["sub"]:
            e = run(p + ["__no_such_subcommand__"])
            with open(os.path.join(SPEC, "misc", name + ".errusage"), "wb") as f:
                f.write(e.stderr)
        tree[name] = node

    with open(os.path.join(SPEC, "tree.json"), "w") as f:
        json.dump(tree, f, indent=1, sort_keys=True)

    # fixed global outputs
    for label, args in [
        ("version", ["--version"]),
        ("license", ["--license"]),
        ("formats", ["--formats"]),
        ("drivers", ["--drivers"]),
        ("rootusage", []),
    ]:
        r = run(args)
        with open(os.path.join(SPEC, "misc", label + ".out"), "wb") as f:
            f.write(r.stdout)
        with open(os.path.join(SPEC, "misc", label + ".err"), "wb") as f:
            f.write(r.stderr)
    print("spec written:", len(paths), "commands")


SECTION_RE = re.compile(
    r"^(Positional arguments|Common Options|Options|Advanced Options|Esoteric Options):$"
)


def parse_help(text, node):
    """Extract display strings / aliases / positional order from help text."""
    lines = text.split("\n")
    section = None
    entries = []  # (section, display)
    for ln in lines:
        m = SECTION_RE.match(ln)
        if m:
            section = m.group(1)
            continue
        if section is None:
            continue
        if not ln.strip():
            continue
        if not ln.startswith("  "):
            section = None
            continue
        if ln.startswith("    ") and entries:
            continue  # continuation line (mutex note, etc.)
        body = ln[2:]
        m2 = re.match(r"^(\S+(?: \S+)*?)   +", body)
        if not m2:
            m2 = re.match(r"^(\S+(?: \S+)*?)  +", body)
        if not m2:
            continue
        entries.append((section, m2.group(1)))

    by_name = {a["name"]: a for a in node["args"]}
    pos_index = 0
    for section, disp in entries:
        toks = [t.rstrip(",") for t in disp.split(" ")]
        names = [t for t in toks if t.startswith("-")]
        metavar = " ".join(t for t in toks if not t.startswith("-"))
        longs = [t[2:] for t in names if t.startswith("--")]
        shorts = [t[1:] for t in names if not t.startswith("--")]
        argname = longs[-1] if longs else None
        if argname is None and metavar:
            # bare positional shown as "<INPUT>"
            cand = metavar.strip("<>").lower()
            argname = cand if cand in by_name else None
        a = by_name.get(argname)
        if a is None:
            continue
        a["display"] = disp
        a["aliases"] = longs
        a["shorts"] = shorts
        a["metavar_disp"] = metavar
        a["section"] = section
        if section == "Positional arguments":
            a["positional"] = pos_index
            pos_index += 1


if __name__ == "__main__":
    sys.exit(main())
