#!/usr/bin/env python3
"""Drop Qt includes that the compiler can prove unnecessary.

The text heuristic is not enough: a translation unit uses types declared in
`main_window.h` through member pointers (`table_->setModel(...)`), so the class
name never appears in the .cpp even though the header is required. Removing an
include is only safe when the file still compiles, so each candidate is tried
one at a time against the real compiler and put back on failure.
"""
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
COMMANDS = json.load(open(os.path.join(BUILD, "compile_commands.json")))
BY_FILE = {os.path.realpath(e["file"]): e for e in COMMANDS}


def qt_includes(path):
    text = open(path).read()
    return [
        m.group(1)
        for m in re.finditer(r"^#include\s+<(Q\w+)>$", text, re.M)
    ]


def compile_only(path):
    """Recompile one translation unit without relinking."""
    entry = BY_FILE[os.path.realpath(path)]
    cmd = entry["command"]
    # -fsyntax-only stops before code generation, so no object file is written.
    cmd = re.sub(r"\s-o\s+\S+", " -fsyntax-only", cmd, count=1)
    result = subprocess.run(
        cmd, shell=True, cwd=entry["directory"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    return result.returncode == 0


def drop_include(path, header):
    text = open(path).read()
    return drop_include_text(text, header)


def drop_include_text(text, header):
    return re.sub(r"^#include\s+<" + re.escape(header) + r">\n", "", text, flags=re.M)


def restore_include(path, header):
    """Put the include back in sorted position inside the Qt block."""
    text = open(path).read()
    line = "#include <%s>\n" % header
    if line in text:
        return text
    lines = text.split("\n")
    qt = [i for i, ln in enumerate(lines) if re.match(r"^#include\s+<Q\w+>$", ln)]
    if not qt:
        return text
    target = qt[-1] + 1
    for i in qt:
        name = re.match(r"^#include\s+<(.+)>$", lines[i]).group(1)
        if name > header:
            target = i
            break
    lines.insert(target, line.rstrip("\n"))
    return "\n".join(lines)


def main(paths):
    for path in paths:
        path = os.path.abspath(path)
        if not compile_only(path):
            print("%s: does not compile before the change, skipped" % path)
            continue
        removed, kept = [], []
        for header in qt_includes(path):
            # A type declared in `main_window.h` is used through a member
            # pointer (`table_->setModel(...)`), so the class name never
            # appears here. Only drop an include when the name is absent, and
            # then let the compiler confirm the file still builds.
            original = open(path).read()
            if header in re.sub(r"^#include.*$", "", original, flags=re.M):
                kept.append(header)
                continue
            open(path, "w").write(drop_include_text(original, header))
            if compile_only(path):
                removed.append(header)
            else:
                open(path, "w").write(original)
                kept.append(header)
        print("%s\n  removed (%d): %s\n  kept (%d): %s" % (
            os.path.basename(path), len(removed), ", ".join(removed),
            len(kept), ", ".join(kept)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
