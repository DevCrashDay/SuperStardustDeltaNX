#!/usr/bin/env python3
"""Batch-compile dumped SPIR-V shaders (.spv + .meta) into Switch deko3d .dksh binaries."""
import concurrent.futures
import glob
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SPV2DKSH = os.path.join(HERE, "spv2dksh.exe")
_dkp = os.environ.get("DEVKITPRO", "")
if not _dkp or not os.path.isdir(_dkp):
    _dkp = r"C:\devkitPro" if os.name == "nt" else "/opt/devkitpro"
UAM = os.path.join(_dkp, "tools", "bin", "uam.exe" if os.name == "nt" else "uam")


def compile_one(spv_path, out_dir, glsl_dir):
    stem = os.path.splitext(os.path.basename(spv_path))[0]
    meta_path = os.path.splitext(spv_path)[0] + ".meta"
    if not os.path.isfile(meta_path):
        return (stem, False, "missing .meta")
    with open(meta_path) as f:
        is_frag, nsmp, nsbuf = f.read().split()[:3]
    stage = "frag" if int(is_frag) else "vert"
    glsl_path = os.path.join(glsl_dir, stem + ".glsl")
    dksh_path = os.path.join(out_dir, stem + ".dksh")

    r1 = subprocess.run([SPV2DKSH, spv_path, glsl_path, stage, nsmp, nsbuf], capture_output=True, text=True)
    if r1.returncode != 0:
        return (stem, False, f"spv2dksh: {r1.stderr.strip()}")
    r2 = subprocess.run([UAM, "-s", stage, "-o", dksh_path, glsl_path], capture_output=True, text=True)
    if r2.returncode != 0:
        return (stem, False, f"uam: {r2.stderr.strip()}")
    return (stem, True, "")


def main(argv):
    if len(argv) < 2:
        print("usage: build_dksh.py <spv_dir> <out_dksh_dir>")
        return 1
    spv_dir = argv[0]
    out_dir = argv[1]
    glsl_dir = os.path.join(out_dir, "_glsl")
    os.makedirs(out_dir, exist_ok=True)
    os.makedirs(glsl_dir, exist_ok=True)
    spvs = sorted(glob.glob(os.path.join(spv_dir, "*.spv")))
    ok, fail = 0, 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
        for stem, good, err in ex.map(lambda p: compile_one(p, out_dir, glsl_dir), spvs):
            if good:
                ok += 1
            else:
                fail += 1
                print(f"FAIL {stem}: {err}")
    print(f"compiled {ok}/{len(spvs)} shaders to .dksh ({fail} failed)")
    return 0 if fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
