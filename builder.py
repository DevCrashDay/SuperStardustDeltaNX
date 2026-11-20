#!/usr/bin/env python3
"""Build a self-contained "Super Stardust Delta" Switch folder (.nro + .nsp)
from a user's own PS Vita dump (e.g. "Super Stardust Delta [PCSA00006] [USA] [1xDLC] [NoNpDrm]").
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path
from typing import Callable

HERE = Path(__file__).resolve().parent
ENGINE = HERE / "engine"
PACK_SRC = HERE / "pack" / "pack.toml"
FORWARDER = HERE / "forwarder"
TITLE_ID = "PCSA00006"
SWITCH_TITLE_ID = "0100535344000001"
OUT_FOLDER_NAME = "Super Stardust Delta"
PUP_LIST_URL = "http://dus01.psp2.update.playstation.net/update/psp2/list/us/psp2-updatelist.xml"

LogFn = Callable[[str], None]


def _exe(name: str) -> str:
    return f"{name}.exe" if os.name == "nt" else name


def find_devkitpro(explicit: str | None = None) -> Path:
    candidates = [
        explicit,
        os.environ.get("DEVKITPRO"),
        r"C:\devkitPro" if os.name == "nt" else "/opt/devkitpro",
    ]
    for c in candidates:
        if c and Path(c).is_dir():
            return Path(c)
    raise RuntimeError("devkitPro not found. Install devkitPro (devkitA64 + libnx + devkitpro-pkgbuild/switch-tools) or set DEVKITPRO.")


def find_prod_keys(explicit: str | None = None) -> Path | None:
    if explicit:
        p = Path(explicit)
        return p if p.is_file() else None
    candidates = [
        Path(os.environ.get("APPDATA", "")) / "Ryujinx" / "system" / "prod.keys",
        Path.home() / ".config" / "Ryujinx" / "system" / "prod.keys",
        Path.home() / ".switch" / "prod.keys",
        Path(os.environ.get("USERPROFILE", "")) / ".switch" / "prod.keys",
    ]
    for c in candidates:
        if c.is_file():
            return c
    return None


def find_library_dir() -> Path:
    if os.environ.get("ATIV_DUMP"):
        return Path(os.environ["ATIV_DUMP"])
    sys.path.insert(0, str(ENGINE / "pipeline"))
    try:
        import datadir
        return Path(datadir.library_dir())
    except Exception:
        if os.name == "nt":
            return Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local")) / "ProjectNativ" / "Nativ" / "library"
        return Path.home() / ".local" / "share" / "ProjectNativ" / "Nativ" / "library"


def ensure_venv(log: LogFn) -> Path:
    venv_dir = ENGINE / ".venv"
    py = venv_dir / ("Scripts" if os.name == "nt" else "bin") / _exe("python")
    if not py.is_file():
        log("Creating engine Python virtual environment...")
        subprocess.run([sys.executable, "-m", "venv", str(venv_dir)], check=True)
        log("Installing Python dependencies (capstone, pycryptodome, Pillow)...")
        subprocess.run([str(py), "-m", "pip", "install", "--quiet", "-r", str(ENGINE / "requirements.txt")], check=True)
    else:
        # Ensure Pillow is installed in existing venv
        chk = subprocess.run([str(py), "-c", "import PIL"], capture_output=True)
        if chk.returncode != 0:
            log("Installing Pillow into engine venv...")
            subprocess.run([str(py), "-m", "pip", "install", "--quiet", "Pillow>=10.0.0"], check=True)
    return py


def run_logged(cmd: list[str], env: dict[str, str], log: LogFn, cwd: Path | None = None) -> None:
    log(f"$ {' '.join(str(x) for x in cmd)}")
    proc = subprocess.Popen(
        [str(x) for x in cmd],
        cwd=str(cwd) if cwd else None,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
        bufsize=1,
    )
    assert proc.stdout is not None
    for line in proc.stdout:
        line = line.rstrip("\r\n")
        if line:
            log(line)
    rc = proc.wait()
    if rc != 0:
        raise RuntimeError(f"Command failed with exit code {rc}: {' '.join(str(x) for x in cmd)}")


def ensure_firmware(py: Path, env: dict[str, str], lib_dir: Path, pup_path: str | None, log: LogFn) -> None:
    fw_glob = list((lib_dir / "firmware").glob("*/os0/us/libgxm_es4.suprx.elf"))
    if fw_glob:
        log(f"PS Vita firmware already imported: {fw_glob[0].parent.parent.parent.name}")
        return
    if pup_path:
        pup = Path(pup_path)
        if not pup.is_file():
            raise RuntimeError(f"Firmware PUP not found: {pup}")
    else:
        log("Fetching PS Vita firmware update URL...")
        with urllib.request.urlopen(PUP_LIST_URL, timeout=30) as resp:
            xml = resp.read().decode("utf-8", errors="replace")
        m = re.search(r'https?://[^"<\s]+\.PUP', xml)
        if not m:
            raise RuntimeError("Could not find PS Vita firmware URL; pass --pup <PSP2UPDAT.PUP>.")
        url = m.group(0)
        cache_dir = HERE / ".cache"
        cache_dir.mkdir(parents=True, exist_ok=True)
        pup = cache_dir / Path(url).name
        if not pup.is_file():
            log(f"Downloading {url} -> {pup}")
            urllib.request.urlretrieve(url, pup)
    run_logged([str(py), str(ENGINE / "pipeline" / "vitarecomp.py"), "firmware", str(pup)], env, log)


def import_game_dump(py: Path, env: dict[str, str], dump_path: Path, log: LogFn) -> None:
    vr = str(ENGINE / "pipeline" / "vitarecomp.py")
    lic_py = str(ENGINE / "pipeline" / "licenses.py")

    if dump_path.is_file():
        run_logged([str(py), vr, "add", str(dump_path), "--no-build"], env, log)
        run_logged([str(py), vr, "import", TITLE_ID], env, log)
        return

    if not dump_path.is_dir():
        raise RuntimeError(f"Input path does not exist: {dump_path}")

    # NoNpDrm bundle layout: <bundle>/app/PCSA00006, optional addcont/PCSA00006, optional license/
    app_dir = dump_path / "app" / TITLE_ID
    if not app_dir.is_dir() and (dump_path / "app").is_dir():
        subs = [p for p in (dump_path / "app").iterdir() if p.is_dir()]
        if len(subs) == 1:
            app_dir = subs[0]

    if app_dir.is_dir():
        lic_dir = dump_path / "license"
        if lic_dir.is_dir():
            log("Importing NoNpDrm licenses...")
            run_logged([str(py), lic_py, "import", str(lic_dir)], env, log)

        log(f"Decrypting game from {app_dir}...")
        run_logged([str(py), vr, "add", str(app_dir), "--no-build"], env, log)

        addcont_dir = dump_path / "addcont" / TITLE_ID
        if addcont_dir.is_dir():
            addon_subs = [
                p for p in sorted(addcont_dir.iterdir())
                if p.is_dir() and p.name not in ("sce_pfs", "sce_sys")
            ]
            if addon_subs:
                for sub in addon_subs:
                    log(f"Decrypting DLC from {sub}...")
                    run_logged([str(py), vr, "add", str(sub)], env, log)
            else:
                log(f"Decrypting DLC from {addcont_dir}...")
                run_logged([str(py), vr, "add", str(addcont_dir)], env, log)

        run_logged([str(py), vr, "import", TITLE_ID], env, log)
        return

    # Direct title folder (e.g. ux0:app/PCSA00006)
    log(f"Decrypting game from {dump_path}...")
    run_logged([str(py), vr, "add", str(dump_path), "--no-build"], env, log)
    run_logged([str(py), vr, "import", TITLE_ID], env, log)


def write_minimal_keys(prod_keys: Path, dest_keys: Path) -> None:
    wanted_lines = []
    for raw in prod_keys.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k = line.split("=", 1)[0].strip()
        if k == "header_key" or k.startswith("key_area_key_application_"):
            wanted_lines.append(line)
    if not any(l.startswith("header_key") for l in wanted_lines):
        raise RuntimeError(f"header_key not found in {prod_keys}")
    if not any(l.startswith("key_area_key_application_") for l in wanted_lines):
        raise RuntimeError(f"key_area_key_application_* not found in {prod_keys}")
    dest_keys.write_text("\n".join(wanted_lines) + "\n", encoding="utf-8", newline="\n")


def make_switch_icon(app0_dir: Path, out_jpg: Path, py: Path) -> None:
    candidates = [
        app0_dir / "sce_sys" / "icon0.png",
        app0_dir / "sce_sys" / "pic0.png",
        app0_dir / "overlay.png",
    ]
    src = next((c for c in candidates if c.is_file()), None)
    if not src:
        raise RuntimeError(f"Could not find game icon in {app0_dir / 'sce_sys'}")
    script = (
        "from PIL import Image; import sys; "
        "im = Image.open(sys.argv[1]).convert('RGB'); "
        "w, h = im.size; s = min(w, h); "
        "im = im.crop(((w - s)//2, (h - s)//2, (w + s)//2, (h + s)//2)).resize((256, 256), Image.Resampling.LANCZOS); "
        "im.save(sys.argv[2], 'JPEG', quality=90, optimize=True)"
    )
    subprocess.run([str(py), "-c", script, str(src), str(out_jpg)], check=True)


def build_nsp(
    py: Path,
    dkp: Path,
    elf_path: Path,
    app0_dir: Path,
    prod_keys: Path,
    out_nsp: Path,
    env: dict[str, str],
    log: LogFn,
) -> None:
    hacbrewpack = FORWARDER / _exe("hacbrewpack")
    if not hacbrewpack.is_file():
        found = shutil.which(_exe("hacbrewpack"))
        if not found:
            raise RuntimeError(f"hacbrewpack not found at {hacbrewpack} or in PATH.")
        hacbrewpack = Path(found)

    dkp_tools = dkp / "tools" / "bin"
    elf2nso = dkp_tools / _exe("elf2nso")
    npdmtool = dkp_tools / _exe("npdmtool")
    nacptool = dkp_tools / _exe("nacptool")
    for tool in (elf2nso, npdmtool, nacptool):
        if not tool.is_file():
            raise RuntimeError(f"Required devkitPro tool missing: {tool}")

    with tempfile.TemporaryDirectory(prefix="ssd_nsp_") as tmp_str:
        tmp = Path(tmp_str)
        exefs = tmp / "exefs"
        control = tmp / "control"
        nsp_out = tmp / "nsp_out"
        hbp_tmp = tmp / "hbp_tmp"
        exefs.mkdir()
        control.mkdir()
        nsp_out.mkdir()
        hbp_tmp.mkdir()

        keys_dat = tmp / "keys.dat"
        write_minimal_keys(prod_keys, keys_dat)

        log("Converting game_host.elf -> ExeFS main (NSO)...")
        run_logged([str(elf2nso), str(elf_path), str(exefs / "main")], env, log)

        log("Generating ExeFS main.npdm...")
        run_logged([str(npdmtool), str(FORWARDER / "npdm.json"), str(exefs / "main.npdm")], env, log)

        log("Generating Control NACP + Switch icon...")
        run_logged(
            [
                str(nacptool),
                "--create",
                "Super Stardust Delta",
                "Project Nativ",
                "1.0",
                str(control / "control.nacp"),
                f"--titleid={SWITCH_TITLE_ID}",
            ],
            env,
            log,
        )
        make_switch_icon(app0_dir, control / "icon_AmericanEnglish.dat", py)

        romfs_dir = ENGINE / "runtime" / "platform" / "switch" / "romfs"
        cmd = [
            str(hacbrewpack),
            "-k", str(keys_dat),
            "--exefsdir", str(exefs),
            "--controldir", str(control),
            "--nspdir", str(nsp_out),
            "--tempdir", str(hbp_tmp),
            "--nologo",
        ]
        if romfs_dir.is_dir():
            cmd += ["--romfsdir", str(romfs_dir)]

        log("Packing SuperStardustDelta.nsp with hacbrewpack...")
        run_logged(cmd, env, log, cwd=tmp)

        built_nsps = list(nsp_out.glob("*.nsp"))
        if not built_nsps:
            raise RuntimeError("hacbrewpack succeeded but produced no .nsp file.")
        shutil.copy2(built_nsps[0], out_nsp)
        log(f"Created NSP: {out_nsp}")


def build_port(
    dump_dir: str,
    out_parent: str | None = None,
    keys_path: str | None = None,
    pup_path: str | None = None,
    devkitpro_path: str | None = None,
    skip_nsp: bool = False,
    log: LogFn = print,
) -> Path:
    dump = Path(dump_dir).resolve()
    if not dump.exists():
        raise RuntimeError(f"Game folder not found: {dump}")

    dkp = find_devkitpro(devkitpro_path)
    prod_keys = None if skip_nsp else find_prod_keys(keys_path)
    if not skip_nsp and not prod_keys:
        raise RuntimeError(
            "Switch prod.keys / keys.dat not found. Select your prod.keys file to build the .nsp "
            "(or pass --skip-nsp for .nro only)."
        )

    py = ensure_venv(log)
    lib_dir = find_library_dir()

    # Sync pack/pack.toml -> engine/packs/PCSA00006/pack.toml
    pack_dst = ENGINE / "packs" / TITLE_ID
    pack_dst.mkdir(parents=True, exist_ok=True)
    shutil.copy2(PACK_SRC, pack_dst / "pack.toml")

    env = os.environ.copy()
    env["TARGET"] = "switch"
    env["DEVKITPRO"] = str(dkp)

    ensure_firmware(py, env, lib_dir, pup_path, log)
    import_game_dump(py, env, dump, log)

    vr = str(ENGINE / "pipeline" / "vitarecomp.py")
    log("Verifying game version...")
    run_logged([str(py), vr, "versions", TITLE_ID], env, log)

    log("Analyzing, recompiling, and building Switch NRO (this may take a few minutes on first run)...")
    run_logged([str(py), vr, "ready", TITLE_ID], env, log)

    # Query Game paths from vitarecomp
    sys.path.insert(0, str(ENGINE / "pipeline"))
    os.environ["TARGET"] = "switch"
    os.environ["DEVKITPRO"] = str(dkp)
    import vitarecomp as vr_mod

    g = vr_mod.Game(TITLE_ID, "run")
    recomp_out = Path(g.OUT)
    nro_src = recomp_out / g.EXE
    elf_src = recomp_out / (Path(g.EXE).stem + ".elf")
    if not nro_src.is_file() or not elf_src.is_file():
        raise RuntimeError(f"Expected Switch build outputs missing in {recomp_out}")

    app0_src = Path(os.path.realpath(recomp_out / "fs" / "app0"))
    if not app0_src.is_dir():
        patched = Path(g.W) / "app0_patched"
        app0_src = patched if patched.is_dir() else Path(g.W) / "app0"
    addcont_src = Path(g.W) / "addcont0"

    parent = Path(out_parent).resolve() if out_parent else dump.parent
    out_dir = parent / OUT_FOLDER_NAME
    out_dir.mkdir(parents=True, exist_ok=True)

    log(f"Staging output folder: {out_dir}")
    shutil.copy2(nro_src, out_dir / "SuperStardustDelta.nro")

    recomp_dst = out_dir / "recomp"
    recomp_dst.mkdir(exist_ok=True)
    imgs = [g.BASE + ".img"] + [m + ".img" for m in g.MODS]
    for img in imgs:
        shutil.copy2(recomp_out / img, recomp_dst / img)

    log("Copying decrypted game data (app0)...")
    shutil.copytree(app0_src, out_dir / "app0", dirs_exist_ok=True)
    if addcont_src.is_dir():
        log("Copying decrypted DLC data (addcont0)...")
        shutil.copytree(addcont_src, out_dir / "addcont0", dirs_exist_ok=True)

    (out_dir / "savedata0").mkdir(exist_ok=True)
    (out_dir / "shaders").mkdir(exist_ok=True)

    dev_root = "sdmc:/switch/SuperStardustDelta"
    launch_lines = [
        f"image = {dev_root}/recomp/{g.BASE}.img",
        f"entry = {g.entry()}",
        "RECOMP_MODULES = " + ",".join(f"{dev_root}/recomp/{m}.img" for m in g.MODS),
        f"RECOMP_FS = {dev_root}",
        f"RECOMP_DKSH_DIR = {dev_root}/shaders",
        f"RECOMP_PRECOMPILE_SPV = {dev_root}/shaders_missing",
    ]
    (out_dir / "launch.ini").write_text("\n".join(launch_lines) + "\n", encoding="utf-8", newline="\n")

    if not skip_nsp:
        assert prod_keys is not None
        build_nsp(
            py=py,
            dkp=dkp,
            elf_path=elf_src,
            app0_dir=app0_src,
            prod_keys=prod_keys,
            out_nsp=out_dir / "SuperStardustDelta.nsp",
            env=env,
            log=log,
        )

    log(f"\nDone! Output folder ready: {out_dir}")
    return out_dir


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description="Build Super Stardust Delta Switch (.nro + .nsp) from your Vita dump.")
    p.add_argument("dump_dir", help='Path to your Vita dump folder (e.g. "Super Stardust Delta [PCSA00006] [USA] [1xDLC] [NoNpDrm]")')
    p.add_argument("--out", help='Parent directory for the "Super Stardust Delta" output folder (default: next to dump_dir)')
    p.add_argument("--keys", help="Path to Switch prod.keys or keys.dat (auto-detected from Ryujinx / ~/.switch if omitted)")
    p.add_argument("--pup", help="Path to PS Vita firmware PSP2UPDAT.PUP (auto-downloaded if omitted)")
    p.add_argument("--devkitpro", help="Path to devkitPro (default: DEVKITPRO env or C:/devkitPro)")
    p.add_argument("--skip-nsp", action="store_true", help="Build .nro only and skip .nsp packaging")
    args = p.parse_args(argv)
    try:
        build_port(
            dump_dir=args.dump_dir,
            out_parent=args.out,
            keys_path=args.keys,
            pup_path=args.pup,
            devkitpro_path=args.devkitpro,
            skip_nsp=args.skip_nsp,
        )
        return 0
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
