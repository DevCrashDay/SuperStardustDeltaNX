#!/usr/bin/env python3
"""The values dev/android-install.sh needs, as shell assignments: where the game's build and files
are, its entry address, its module images and its pack's RUN_ENV. One source of truth -- the pack and
the pipeline's own Game -- so the script never second-guesses either.

  eval "$(dev/android_launch_info.py PCSE00397)"
"""
import importlib.util
import os
import sys

R = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location("vitarecomp", os.path.join(R, "pipeline", "vitarecomp.py"))
vr = importlib.util.module_from_spec(spec)
sys.modules["vitarecomp"] = vr
spec.loader.exec_module(vr)


def q(s):
    return "'" + str(s).replace("'", "'\\''") + "'"


def main(title):
    g = vr.Game(title, "android")
    print("OUT=" + q(g.OUT))
    print("IMAGE=" + q(g.BASE + ".img"))
    print("ENTRY=" + q(g.entry()))
    print("RUNENV=" + q(g.RUN_ENV))
    print("MODS=" + q(" ".join(m + ".img" for m in g.MODS)))
    print("GAMENAME=" + q(g.NAME))
    print("TITLEID=" + q(g.TITLE_ID))
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2: sys.exit(__doc__)
    sys.exit(main(sys.argv[1]))
