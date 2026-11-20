#!/usr/bin/env python3
"""Stage a game's Switch build (TARGET=switch vitarecomp build <title>) on an SD card folder -- a real card or
an emulator's (Ryujinx: %APPDATA%/Ryujinx/sdcard, the default).

  dev/switch-install.py <title> [--sd <sdcard root>] [--dir <folder name>] [--shaders <dksh dir>] [KEY=VALUE ...]

Lays out <sd>/switch/<dir>/: <dir>.nro, recomp/*.img, launch.ini (image, entry, modules; KEY=VALUE words are added
to it, RECOMP_* only), app0 (a directory junction to the decrypted game -- the patched one when the pack plays the
update -- so gigabytes of data are not copied; on a real card pass --copy-data), addcont0, savedata0 and shaders/.
The pack's RUN_ENV is compiled into the program and needs no line here. Never run it while the game is running.
"""
import os
import shutil
import subprocess
import sys

R = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.environ["TARGET"] = "switch"   # (before vitarecomp reads its environment: the .nro, not the desktop program)
sys.path.insert(0, os.path.join(R, "pipeline"))
import importlib.util
spec = importlib.util.spec_from_file_location("vitarecomp", os.path.join(R, "pipeline", "vitarecomp.py"))
vr = importlib.util.module_from_spec(spec); sys.modules["vitarecomp"] = vr; spec.loader.exec_module(vr)


def link_dir(src, dst, copy):
    if os.path.lexists(dst):
        if os.path.isdir(dst) and not os.path.islink(dst) and not is_junction(dst): return   # a real folder: kept
        os.unlink(dst) if not os.path.isdir(dst) or os.path.islink(dst) else os.rmdir(dst)   # (rmdir removes a junction, not its target)
    if copy: shutil.copytree(src, dst); return
    if os.name == "nt": subprocess.run(["cmd", "/c", "mklink", "/J", os.path.normpath(dst), os.path.normpath(src)], check=True, stdout=subprocess.DEVNULL)
    else: os.symlink(src, dst)


def is_junction(p):
    try: return os.path.isjunction(p)
    except AttributeError: return False


def main(argv):
    if not argv or argv[0].startswith("-"): sys.exit(__doc__)
    title, sd, name, shaders, copy, extra = argv[0], os.path.join(os.environ.get("APPDATA", ""), "Ryujinx", "sdcard"), None, None, False, []
    it = iter(argv[1:])
    for a in it:
        if a == "--sd": sd = next(it)
        elif a == "--dir": name = next(it)
        elif a == "--shaders": shaders = next(it)
        elif a == "--copy-data": copy = True
        elif "=" in a and a.startswith("RECOMP_"): extra.append(a)
        else: sys.exit(f"unknown argument {a}\n{__doc__}")
    g = vr.Game(title, "run")
    name = name or "".join(ch for ch in g.NAME if ch.isalnum()) or title
    nro = os.path.join(g.OUT, g.EXE)
    if not os.path.isfile(nro): sys.exit(f"no {nro}: build it first (TARGET=switch vitarecomp build {title})")
    home = os.path.join(sd, "switch", name); dev = f"sdmc:/switch/{name}"
    os.makedirs(os.path.join(home, "recomp"), exist_ok=True)
    shutil.copyfile(nro, os.path.join(home, name + ".nro"))
    imgs = [g.BASE + ".img"] + [m + ".img" for m in g.MODS]
    for f in imgs: shutil.copyfile(os.path.join(g.OUT, f), os.path.join(home, "recomp", f))
    # the game's files: what `run` mounts as app0 (fs/app0 -> app0 or app0_patched)
    app0 = os.path.realpath(os.path.join(g.OUT, "fs", "app0"))
    if not os.path.isdir(app0): app0 = os.path.join(g.W, "app0_patched") if os.path.isdir(os.path.join(g.W, "app0_patched")) else os.path.join(g.W, "app0")
    link_dir(app0, os.path.join(home, "app0"), copy)
    if os.path.isdir(os.path.join(g.W, "addcont0")): link_dir(os.path.join(g.W, "addcont0"), os.path.join(home, "addcont0"), copy)
    os.makedirs(os.path.join(home, "savedata0"), exist_ok=True)
    os.makedirs(os.path.join(home, "shaders"), exist_ok=True)
    n = 0
    if shaders:
        for f in os.listdir(shaders):
            if f.endswith(".dksh"): shutil.copyfile(os.path.join(shaders, f), os.path.join(home, "shaders", f)); n += 1
    lines = [f"image = {dev}/recomp/{g.BASE}.img", f"entry = {g.entry()}",
             "RECOMP_MODULES = " + ",".join(f"{dev}/recomp/{m}.img" for m in g.MODS),
             f"RECOMP_FS = {dev}", f"RECOMP_DKSH_DIR = {dev}/shaders"]
    lines += [" = ".join(a.split("=", 1)) for a in extra]
    with open(os.path.join(home, "launch.ini"), "w", newline="\n") as f: f.write("\n".join(lines) + "\n")
    print(f"staged {g.NAME} at {home} ({len(imgs)} images, {n} shaders, app0 -> {app0})")
    print(f"play: Ryujinx.exe \"{os.path.join(home, name + '.nro')}\"")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
