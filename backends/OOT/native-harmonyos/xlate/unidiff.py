"""Unified diffs applied in plain Python, as GNU patch -p1 applies them, so preparing the translator needs no patch program
on the PC (prepare_xlate_ns2.py: the reference's games/totk/tools/totk_xlate/patches on nxnative's Ryujinx). A hunk is
looked for as GNU patch looks for it: its exact lines at the line it names (moved by the offset the file's earlier hunks
were found at), then further away, below first; then with one and two outer context lines left out (fuzz 1, 2). Context
lines are kept as the file has them. Unlike patch, a patch is applied whole or not at all (nothing is written unless
every hunk of every file has its place) and no .orig / .rej files are left. Text is compared and written as bytes, LF.

    apply(root, patch_file, reverse=False, dry_run=False) -> [] when it applies, else what did not apply
"""
import re
from pathlib import Path

MAX_FUZZ = 2   # GNU patch's default
_HUNK = re.compile(rb"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")


class Hunk:
    def __init__(self, old_start, new_start, lines):
        self.old_start, self.new_start, self.lines = old_start, new_start, lines   # lines: (tag, text), tag in b" -+"

    def side(self, reverse):
        """(first line, pattern, [(tag, text)]) with '-' the lines to find and drop; reversed, '+' and '-' swap."""
        swap = {b"-": b"+", b"+": b"-", b" ": b" "}
        lines = [(swap[t], s) for t, s in self.lines] if reverse else self.lines
        return (self.new_start if reverse else self.old_start), [s for t, s in lines if t != b"+"], lines


def parse(data):
    """[(path below the root, -p1), [Hunk]] of a unified diff (git's format: a/ and b/ prefixes)."""
    lines = data.replace(b"\r\n", b"\n").split(b"\n")
    files, i = [], 0
    while i < len(lines):
        line = lines[i]
        if line.startswith(b"--- ") and i + 1 < len(lines) and lines[i + 1].startswith(b"+++ "):
            new = lines[i + 1][4:].split(b"\t")[0].strip()
            old = line[4:].split(b"\t")[0].strip()
            if b"/dev/null" in (old, new):
                raise ValueError("a patch that creates or deletes a file is not supported: " + new.decode("utf-8", "replace"))
            files.append((new.split(b"/", 1)[1].decode("utf-8"), []))
            i += 2
            continue
        m = _HUNK.match(line)
        if m and files:
            n_old = int(m.group(2)) if m.group(2) is not None else 1
            n_new = int(m.group(4)) if m.group(4) is not None else 1
            body, i = [], i + 1
            while (n_old > 0 or n_new > 0) and i < len(lines):
                t, s = lines[i][:1], lines[i][1:]
                if t == b"\\":   # "\ No newline at end of file"
                    raise ValueError("a hunk without a final newline is not supported")
                if t == b"" and lines[i] == b"":
                    t = b" "   # a blank context line whose space was stripped
                if t not in (b" ", b"-", b"+"):
                    raise ValueError(f"malformed hunk line {i + 1}: {lines[i][:60]!r}")
                body.append((t, s))
                n_old -= t != b"+"
                n_new -= t != b"-"
                i += 1
            if n_old or n_new:
                raise ValueError("truncated hunk")
            if not any(t != b" " for t, _ in body) or all(t == b"+" for t, _ in body):
                raise ValueError("a hunk without lines to find is not supported")
            files[-1][1].append(Hunk(int(m.group(1)), int(m.group(3)), body))
            continue
        i += 1
    return files


def _locate(text, first, pattern, prefix, suffix, fuzz, in_offset, frozen):
    """GNU patch's locate_hunk (1-based lines): where the pattern's first line goes, and the offset found; None."""
    n_in, n_pat = len(text), len(pattern)
    guess = first + in_offset
    context = max(prefix, suffix)
    pfuzz, sfuzz = fuzz + prefix - context, fuzz + suffix - context
    max_pos = n_in - (n_pat - sfuzz) + 1 - guess
    max_neg = guess - (frozen + 1 - (prefix - pfuzz))
    max_off = max(max_pos, max_neg)
    if guess <= max_neg:
        max_neg = guess - 1

    def match(where, pf, sf):
        if where < 1 or where - 1 + n_pat - sf > n_in:
            return False
        return all(text[where - 1 + k] == pattern[k] for k in range(pf, n_pat - sf))

    if pfuzz < 0 and first <= 1:   # only the start of the file
        if sfuzz < 0 and (n_pat != n_in or prefix < frozen):
            return None
        off = 1 - guess
        return (guess + off, off) if frozen <= prefix and off <= max_pos and match(guess + off, 0, sfuzz) else None
    pfuzz = max(pfuzz, 0)
    if sfuzz < 0:   # only the end of the file
        off = guess - (n_in - n_pat + 1)
        return (guess - off, -off) if off <= max_neg and match(guess - off, pfuzz, 0) else None
    for off in range(0, max_off + 1):
        if off <= max_pos and match(guess + off, pfuzz, sfuzz):
            return guess + off, off
        if 0 < off <= max_neg and match(guess - off, pfuzz, sfuzz):
            return guess - off, -off
    return None


def _apply_file(data, hunks, reverse):
    """The file's new bytes, or (None, number of the first hunk that found no place)."""
    eol = data.endswith(b"\n")
    text = data.split(b"\n")
    if eol:
        text.pop()
    out, pos, in_offset, frozen = [], 1, 0, 0
    for n, h in enumerate(hunks, 1):
        first, pattern, lines = h.side(reverse)
        prefix = next((k for k, (t, _) in enumerate(lines) if t != b" "), len(lines))
        suffix = next((k for k, (t, _) in enumerate(reversed(lines)) if t != b" "), len(lines))
        found = None
        for fuzz in range(0, min(MAX_FUZZ, max(prefix, suffix)) + 1):
            found = _locate(text, first, pattern, prefix, suffix, fuzz, in_offset, frozen)
            if found:
                break
        if not found:
            return None, n
        where, off = found
        if where < pos:   # found overlapping the hunk before it (the search may reach back that far): GNU patch
            return None, n   # refuses it too ("misordered hunks"), rather than writing those lines twice
        in_offset += off
        out += text[pos - 1:where - 1]
        pos = where
        for t, s in lines:
            if t == b" ":
                out.append(text[pos - 1])   # context as the file has it (fuzz leaves outer lines unchecked)
                pos += 1
            elif t == b"-":
                pos += 1
            else:
                out.append(s)
        frozen = pos - 1
    out += text[pos - 1:]
    return b"\n".join(out) + (b"\n" if eol else b""), 0


def apply(root, patch_file, reverse=False, dry_run=False):
    """Apply patch_file (-p1) below root; [] when every hunk applied (and, unless dry_run, the files were written), else
    ["<file>: hunk <n> ..."] and nothing written."""
    root = Path(root)
    new, failed = {}, []
    for rel, hunks in parse(Path(patch_file).read_bytes()):
        f = root / rel
        if not f.is_file():
            failed.append(f"{rel}: missing")
            continue
        data, bad = _apply_file(new.get(f, f.read_bytes()), hunks, reverse)
        if data is None:
            failed.append(f"{rel}: hunk {bad} of {len(hunks)} found no place")
        else:
            new[f] = data
    if not failed and not dry_run:
        for f, data in new.items():
            f.write_bytes(data)
    return failed
