# Super Stardust Delta NX

Nintendo Switch port of Housemarque's PS Vita twin-stick shooter **Super Stardust Delta** (`PCSA00006`), powered by the [Nativ](https://github.com/CrownParkComputing/nativ) static recompiler and `deko3d` Switch runtime.

**No copyrighted code or assets are included.** You provide your own copy of the game (and your Switch `prod.keys` for `.nsp` packaging); everything is decrypted, recompiled, and packaged locally on your machine.

## Requirements

- **Your own copy of Super Stardust Delta (`PCSA00006`, US v1.00)** — e.g. a NoNpDrm dump folder (`Super Stardust Delta [PCSA00006] [USA] [1xDLC] [NoNpDrm]`) or a `ux0:app/PCSA00006` folder.
- **Python 3.10+** (with `tkinter` for the GUI).
- **devkitPro** with `devkitA64`, `libnx`, and `switch-tools` (`elf2nso`, `npdmtool`, `nacptool`).
- **Switch `prod.keys`** (for packaging the installable `.nsp` forwarder; auto-detected from Ryujinx or `~/.switch/prod.keys` if present, or selectable in the GUI).

## Usage (GUI)

1. Launch the GUI:
   - **Windows:** double-click `run_gui.bat` (or run `python gui.py`).
   - **Linux / macOS:** run `./run_gui.sh` (or `python3 gui.py`).
2. Select your **`Super Stardust Delta [PCSA00006] [USA] [1xDLC] [NoNpDrm]`** folder (or `PCSA00006` folder).
3. Verify your `prod.keys` and `devkitPro` paths, then click **Build (.nro + .nsp)**.

The builder creates a **`Super Stardust Delta`** output folder containing:
- `SuperStardustDelta.nro` — Homebrew NRO
- `SuperStardustDelta.nsp` — Installable HOME-menu forwarder NSP (Title ID `0100535344000001`)
- `recomp/`, `app0/`, `addcont0/`, `savedata0/`, `shaders/`, `launch.ini` — SD card data files

## Usage (CLI)

```bash
python builder.py "/path/to/Super Stardust Delta [PCSA00006] [USA] [1xDLC] [NoNpDrm]"
```

Optional flags: `--out <parent_dir>`, `--keys <prod.keys>`, `--pup <PSP2UPDAT.PUP>`, `--devkitpro <path>`, `--skip-nsp`.

## Installing on Nintendo Switch / Ryujinx

1. Copy the data folders (`recomp/`, `app0/`, `addcont0/`, `savedata0/`, `shaders/`, `launch.ini`, and optionally `SuperStardustDelta.nro`) into `sdmc:/switch/SuperStardustDelta/` on your Switch SD card (or Ryujinx `sdcard/switch/SuperStardustDelta/`).
2. Install `SuperStardustDelta.nsp` via DBI / Awoo / Tinfoil / Goldleaf (or launch `SuperStardustDelta.nro` directly).

## Licence

GPL-2.0-or-later, matching the Nativ engine. See [LICENSE](LICENSE).
