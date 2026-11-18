"""A folder dialog on this PC (tkinter, part of the Windows Python installer)."""
from __future__ import annotations


def pick_folder(title: str, initial: str = ""):
    """The chosen folder, or None when cancelled or when tkinter is missing."""
    try:
        import tkinter
        from tkinter import filedialog
    except ImportError:
        return None
    root = tkinter.Tk()
    root.withdraw()
    root.attributes("-topmost", True)
    d = filedialog.askdirectory(title=title, initialdir=initial or None, mustexist=True)
    root.destroy()
    return d or None
