#!/usr/bin/env python3
"""Scan knob combos against all cached probe data."""
import itertools, random, sys
import sim, fit


def run_all(lim3x2=120):
    ok = bad = 0
    fails = []
    for perm in itertools.permutations(range(4)):
        if fit.compare(2, 2, perm, verbose=False):
            ok += 1
        else:
            bad += 1
            fails.append(("2x2", perm))
    perms = list(itertools.permutations(range(6)))
    random.seed(42)
    random.shuffle(perms)
    for perm in perms[:lim3x2]:
        if fit.compare(3, 2, perm, verbose=False):
            ok += 1
        else:
            bad += 1
            fails.append(("3x2", perm))
    return ok, bad, fails


for mf, wge, fge in itertools.product([True, False], repeat=3):
    sim.P.maxpoints_minfirst = mf
    sim.P.msx_within_ge = wge
    sim.P.msx_fallback_ge = fge
    ok, bad, fails = run_all()
    print(f"minfirst={mf} within_ge={wge} fallback_ge={fge}: "
          f"{ok} ok, {bad} bad", fails[:3] if bad else "")
