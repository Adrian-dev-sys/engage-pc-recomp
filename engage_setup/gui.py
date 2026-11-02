"""Engage's form on the MIT-licensed TotK installer window and process controller."""
import argparse
import os
import subprocess
import tkinter as tk
from tkinter import filedialog, ttk, messagebox

from installer.gui import App as BaseApp, fix_tcl_paths
from .model import Catalog, FIELDS, Form, layout


class App(BaseApp):
    catalog_type = Catalog
    ui_languages = ()
    icon_name = 'engage-setup.ico'

    def build_form(self):
        for name in FIELDS[3:]:
            self.vars[name] = tk.StringVar()
        frame = super().build_form()
        grid = self.form_rows['base'].master
        for widget in grid.grid_slaves():
            row = int(widget.grid_info()['row'])
            if row >= 3:
                widget.grid_configure(row=row + 1)
        self.label(grid, 'ui.firmware').grid(row=3, column=0, sticky='w', padx=(0, 10), pady=4)
        ttk.Entry(grid, textvariable=self.vars['firmware']).grid(row=3, column=1, sticky='ew', pady=4)
        self.button(grid, 'ui.browse', lambda: self.pick_file('firmware', 'ui.firmware')).grid(row=3, column=2, padx=(8, 0))
        extra = ttk.LabelFrame(frame, text='Optional extras', padding=8)
        extra.pack(fill='x', before=self.update_hint, pady=(10, 0))
        extra.columnconfigure(1, weight=1)
        for row, name in enumerate(FIELDS[4:]):
            self.label(extra, 'ui.' + name).grid(row=row, column=0, sticky='w', padx=(0, 10), pady=4)
            ttk.Entry(extra, textvariable=self.vars[name]).grid(row=row, column=1, sticky='ew', pady=4)
            self.button(extra, 'ui.browse', lambda n=name: self.pick_file(n, 'ui.' + n)).grid(
                row=row, column=2, padx=(8, 0))
        return frame

    def form(self):
        return Form(**{name: self.vars[name].get() for name in (*FIELDS, 'dest')}, lang=self.selected_lang())

    def pick_file(self, name, pick_id):
        if name == 'firmware':
            path = filedialog.askdirectory(parent=self.root, title='Choose the extracted firmware NCA folder')
        elif name == 'title_keys':
            path = filedialog.askopenfilename(parent=self.root, title='Choose title.keys', filetypes=[('Keys', '*.keys')])
        else:
            return super().pick_file(name, pick_id)
        if path:
            self.vars[name].set(os.path.normpath(path))

    def build_finish(self):
        frame = super().build_finish()
        self.play_button = self.button(frame, 'ui.play', self.play)
        self.play_button.pack(anchor='e', pady=(12, 0))
        return frame

    def refresh_finish(self):
        super().refresh_finish()
        self.play_button.configure(state='normal' if self.model.result == 'ok' else 'disabled')

    def play(self):
        from pathlib import Path
        dest = Path(self.model.dest)
        try:
            subprocess.Popen([str(dest / 'engage.exe')], cwd=dest, env=play_environment(dest))
        except OSError as error:
            messagebox.showerror('Engage could not start', str(error), parent=self.root)


def play_environment(dest):
    return dict(os.environ, TOTK_GPU='vulkan', TOTK_VK_RECORDER='0', TOTK_SHARED_FONTS=str(dest / 'fonts'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--selftest', action='store_true')
    args = parser.parse_args()
    fix_tcl_paths()
    root = tk.Tk()
    app = App(root, 'en', layout())
    if args.selftest:
        root.after(1500, root.destroy)
    root.mainloop()


if __name__ == '__main__':
    main()
