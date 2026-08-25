#!/usr/bin/env python3
"""Differential harness: run oracle and candidate on the same argv in twin
sandboxes; compare stdout/stderr/exit code and produced file trees.

Usage: python3 tests/run_diff.py [casefile ...]
Case file format: one case per line, shell-lexed argv. Lines starting with '#'
are comments. Directives:
  #setup: <shell command>   run in each sandbox before the case that follows
  #norm: ptr                normalize 0x... pointers in outputs (sticky;
                            '#norm:' alone resets)
  #stdin: <text>            feed <text> (backslash escapes decoded) on
                            stdin (sticky; '#stdin:' alone resets to
                            /dev/null)
  #stdinfile: <path>        feed the bytes of <path> (relative to the
                            sandbox, created by #setup) on stdin; takes
                            precedence over #stdin (sticky;
                            '#stdinfile:' alone resets)
  #env: K=V [K=V ...]       override environment variables for both
                            binaries (sticky; '#env:' alone resets)
Every case runs in a fresh pair of sandboxes.
"""
import hashlib
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ORACLE = os.environ.get("ORACLE", "/home/agent/oracle/executable")
MINE = os.path.join(ROOT, "executable")


def tree_digest(d, canon=None):
    entries = []
    for dirpath, dirnames, filenames in os.walk(d):
        dirnames.sort()
        for f in sorted(filenames):
            full = os.path.join(dirpath, f)
            rel = os.path.relpath(full, d)
            if os.path.islink(full):
                entries.append((rel, "link:" + os.readlink(full)))
                continue
            if not os.path.exists(full):
                entries.append((rel, "missing"))
                continue
            h = hashlib.sha256()
            with open(full, "rb") as fp:
                data = fp.read()
            if canon:
                data = canon(data)
            h.update(data)
            entries.append((rel, h.hexdigest()))
    return entries


def run_case(args, setup, norm=None, merge=False, stdin_data=None,
             env_over=None, stdin_file=None):
    def canon(b):
        if b is None:
            b = b""
        if norm == "ptr":
            return re.sub(rb"0x[0-9a-fA-F]+", b"0xPTR", b)
        if norm == "rztmp":
            return re.sub(rb"_rasterize\.tif_[0-9]+_",
                          b"_rasterize.tif_PID_", b)
        return b

    tree_canon = canon if norm == "rztmp" else None
    env = None
    if env_over:
        env = os.environ.copy()
        env.update(env_over)
    boxes = {}
    for name, exe in (("oracle", ORACLE), ("mine", MINE)):
        d = tempfile.mkdtemp(prefix=f"diff_{name}_")
        if setup:
            subprocess.run(setup, shell=True, cwd=d, capture_output=True)
        data = stdin_data
        if stdin_file is not None:
            with open(os.path.join(d, stdin_file), "rb") as sf:
                data = sf.read()
        feed = {"input": data} if data is not None else \
            {"stdin": subprocess.DEVNULL}
        if merge:
            r = subprocess.run([exe] + args, cwd=d,
                               stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT,
                               timeout=120, env=env, **feed)
        else:
            r = subprocess.run([exe] + args, cwd=d, capture_output=True,
                               timeout=120, env=env, **feed)
        boxes[name] = (r, tree_digest(d, tree_canon), d)
    ro, to, do = boxes["oracle"]
    rm, tm, dm = boxes["mine"]

    problems = []
    if ro.returncode != rm.returncode:
        problems.append(f"exit: oracle={ro.returncode} mine={rm.returncode}")
    if canon(ro.stdout) != canon(rm.stdout):
        problems.append("stdout differs")
    if canon(ro.stderr) != canon(rm.stderr):
        problems.append("stderr differs")
    if to != tm:
        problems.append(f"files differ: oracle={to} mine={tm}")
    if problems:
        print(f"FAIL: {args}")
        for p in problems:
            print(f"  {p}")
        if canon(ro.stdout) != canon(rm.stdout):
            show_diff("stdout", canon(ro.stdout), canon(rm.stdout))
        if canon(ro.stderr) != canon(rm.stderr):
            show_diff("stderr", canon(ro.stderr), canon(rm.stderr))
    shutil.rmtree(do, ignore_errors=True)
    shutil.rmtree(dm, ignore_errors=True)
    return not problems


def show_diff(label, a, b, maxlines=10):
    import difflib
    al = a.decode(errors="replace").splitlines()
    bl = b.decode(errors="replace").splitlines()
    d = list(difflib.unified_diff(al, bl, "oracle_" + label,
                                  "mine_" + label, lineterm=""))
    for line in d[:maxlines]:
        print("   " + line)
    if len(d) > maxlines:
        print(f"   ... ({len(d) - maxlines} more diff lines)")


def main():
    files = sys.argv[1:]
    if not files:
        files = [os.path.join(ROOT, "tests", "cases.txt")]
    npass = nfail = 0
    for f in files:
        setup = None
        norm = None
        merge = False
        stdin_data = None
        env_over = None
        stdin_file = None
        with open(f) as fp:
            for line in fp:
                line = line.rstrip("\n")
                if line.startswith("#setup:"):
                    setup = line[len("#setup:"):].strip() or None
                    continue
                if line.startswith("#norm:"):
                    norm = line[len("#norm:"):].strip() or None
                    continue
                if line.startswith("#merge:"):
                    merge = line[len("#merge:"):].strip() == "on"
                    continue
                if line.startswith("#stdinfile:"):
                    raw = line[len("#stdinfile:"):].strip()
                    stdin_file = raw or None
                    continue
                if line.startswith("#stdin:"):
                    raw = line[len("#stdin:"):].strip()
                    stdin_data = raw.encode().decode(
                        "unicode_escape").encode() if raw else None
                    continue
                if line.startswith("#env:"):
                    raw = line[len("#env:"):].strip()
                    env_over = dict(kv.split("=", 1)
                                    for kv in shlex.split(raw)) \
                        if raw else None
                    continue
                if not line.strip() or line.startswith("#"):
                    continue
                args = shlex.split(line)
                if run_case(args, setup, norm, merge, stdin_data,
                            env_over, stdin_file):
                    npass += 1
                else:
                    nfail += 1
    print(f"\n{npass} passed, {nfail} failed")
    return 1 if nfail else 0


if __name__ == "__main__":
    sys.exit(main())
