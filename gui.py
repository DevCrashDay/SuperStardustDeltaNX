#!/usr/bin/env python3
"""Tkinter GUI for building Super Stardust Delta NX (.nro + .nsp) from a Vita dump."""
from __future__ import annotations

import os
import queue
import subprocess
import sys
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, scrolledtext, ttk

import builder


class App(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("Super Stardust Delta NX — Switch Builder")
        self.geometry("760x540")
        self.minsize(640, 440)

        self.log_queue: queue.Queue[str] = queue.Queue()
        self.last_out_dir: Path | None = None
        self.building = False

        self.dump_var = tk.StringVar()
        self.out_var = tk.StringVar()
        detected_keys = builder.find_prod_keys()
        self.keys_var = tk.StringVar(value=str(detected_keys) if detected_keys else "")
        try:
            detected_dkp = str(builder.find_devkitpro())
        except RuntimeError:
            detected_dkp = r"C:\devkitPro" if os.name == "nt" else "/opt/devkitpro"
        self.dkp_var = tk.StringVar(value=detected_dkp)
        self.skip_nsp_var = tk.BooleanVar(value=False)

        self._build_ui()
        self.after(100, self._poll_logs)

    def _build_ui(self) -> None:
        pad = {"padx": 8, "pady": 4}
        frm = ttk.Frame(self, padding=10)
        frm.pack(fill=tk.BOTH, expand=True)
        frm.columnconfigure(1, weight=1)

        # Row 0: Game Dump Folder
        ttk.Label(frm, text="Vita Game Folder:").grid(row=0, column=0, sticky="w", **pad)
        ttk.Entry(frm, textvariable=self.dump_var).grid(row=0, column=1, sticky="ew", **pad)
        ttk.Button(frm, text="Browse...", command=self._browse_dump).grid(row=0, column=2, **pad)

        # Row 1: Output Parent Folder
        ttk.Label(frm, text="Output Folder:").grid(row=1, column=0, sticky="w", **pad)
        ttk.Entry(frm, textvariable=self.out_var).grid(row=1, column=1, sticky="ew", **pad)
        ttk.Button(frm, text="Browse...", command=self._browse_out).grid(row=1, column=2, **pad)

        # Row 2: Switch prod.keys
        ttk.Label(frm, text="Switch prod.keys:").grid(row=2, column=0, sticky="w", **pad)
        ttk.Entry(frm, textvariable=self.keys_var).grid(row=2, column=1, sticky="ew", **pad)
        ttk.Button(frm, text="Browse...", command=self._browse_keys).grid(row=2, column=2, **pad)

        # Row 3: devkitPro
        ttk.Label(frm, text="devkitPro Path:").grid(row=3, column=0, sticky="w", **pad)
        ttk.Entry(frm, textvariable=self.dkp_var).grid(row=3, column=1, sticky="ew", **pad)
        ttk.Button(frm, text="Browse...", command=self._browse_dkp).grid(row=3, column=2, **pad)

        # Row 4: Options & Build button
        opt_frm = ttk.Frame(frm)
        opt_frm.grid(row=4, column=0, columnspan=3, sticky="ew", pady=(6, 6))
        ttk.Checkbutton(opt_frm, text="Skip .nsp (build .nro only)", variable=self.skip_nsp_var).pack(side=tk.LEFT, padx=4)
        self.open_btn = ttk.Button(opt_frm, text="Open Output Folder", command=self._open_out, state=tk.DISABLED)
        self.open_btn.pack(side=tk.RIGHT, padx=4)
        self.build_btn = ttk.Button(opt_frm, text="Build (.nro + .nsp)", command=self._start_build)
        self.build_btn.pack(side=tk.RIGHT, padx=4)

        # Row 5: Status
        self.status_var = tk.StringVar(value='Select your "Super Stardust Delta [PCSA00006] [USA] [1xDLC] [NoNpDrm]" folder.')
        ttk.Label(frm, textvariable=self.status_var).grid(row=5, column=0, columnspan=3, sticky="w", padx=8, pady=(0, 4))

        # Row 6: Log box
        self.log_box = scrolledtext.ScrolledText(frm, state=tk.DISABLED, wrap=tk.WORD, font=("Consolas", 9) if os.name == "nt" else ("Monospace", 9))
        self.log_box.grid(row=6, column=0, columnspan=3, sticky="nsew", padx=8, pady=4)
        frm.rowconfigure(6, weight=1)

    def _browse_dump(self) -> None:
        d = filedialog.askdirectory(title='Select "Super Stardust Delta [PCSA00006]..." folder')
        if d:
            self.dump_var.set(d)
            if not self.out_var.get().strip():
                self.out_var.set(str(Path(d).parent))

    def _browse_out(self) -> None:
        d = filedialog.askdirectory(title='Select parent folder for "Super Stardust Delta"')
        if d:
            self.out_var.set(d)

    def _browse_keys(self) -> None:
        f = filedialog.askopenfilename(title="Select Switch prod.keys", filetypes=[("Key files", "*.keys *.dat"), ("All files", "*.*")])
        if f:
            self.keys_var.set(f)

    def _browse_dkp(self) -> None:
        d = filedialog.askdirectory(title="Select devkitPro directory")
        if d:
            self.dkp_var.set(d)

    def _open_out(self) -> None:
        if not self.last_out_dir or not self.last_out_dir.is_dir():
            return
        if os.name == "nt":
            os.startfile(str(self.last_out_dir))
        elif sys.platform == "darwin":
            subprocess.Popen(["open", str(self.last_out_dir)])
        else:
            subprocess.Popen(["xdg-open", str(self.last_out_dir)])

    def _append_log(self, text: str) -> None:
        self.log_box.configure(state=tk.NORMAL)
        self.log_box.insert(tk.END, text + "\n")
        self.log_box.see(tk.END)
        self.log_box.configure(state=tk.DISABLED)

    def _poll_logs(self) -> None:
        try:
            while True:
                msg = self.log_queue.get_nowait()
                self._append_log(msg)
        except queue.Empty:
            pass
        self.after(100, self._poll_logs)

    def _start_build(self) -> None:
        if self.building:
            return
        dump = self.dump_var.get().strip()
        if not dump:
            messagebox.showerror("Missing Folder", "Please select your Vita game folder.")
            return
        out_parent = self.out_var.get().strip() or None
        keys = self.keys_var.get().strip() or None
        dkp = self.dkp_var.get().strip() or None
        skip_nsp = self.skip_nsp_var.get()

        self.building = True
        self.build_btn.configure(state=tk.DISABLED)
        self.open_btn.configure(state=tk.DISABLED)
        self.status_var.set("Building... see log below.")
        self.log_box.configure(state=tk.NORMAL)
        self.log_box.delete("1.0", tk.END)
        self.log_box.configure(state=tk.DISABLED)

        def worker() -> None:
            try:
                out_dir = builder.build_port(
                    dump_dir=dump,
                    out_parent=out_parent,
                    keys_path=keys,
                    devkitpro_path=dkp,
                    skip_nsp=skip_nsp,
                    log=self.log_queue.put,
                )
                self.after(0, lambda: self._on_done(out_dir, None))
            except Exception as exc:
                self.after(0, lambda: self._on_done(None, exc))

        threading.Thread(target=worker, daemon=True).start()

    def _on_done(self, out_dir: Path | None, err: Exception | None) -> None:
        self.building = False
        self.build_btn.configure(state=tk.NORMAL)
        if err:
            self.status_var.set("Build failed.")
            self._append_log(f"ERROR: {err}")
            messagebox.showerror("Build Failed", str(err))
        else:
            self.last_out_dir = out_dir
            self.open_btn.configure(state=tk.NORMAL)
            self.status_var.set(f"Done! Output ready at: {out_dir}")
            messagebox.showinfo("Build Complete", f'Created "Super Stardust Delta" folder:\n{out_dir}')


if __name__ == "__main__":
    App().mainloop()
