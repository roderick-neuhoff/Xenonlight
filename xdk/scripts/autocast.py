"""Inserts explicit casts where the Xbox 360 SDK's C++ compiler reports
    error C2440: cannot convert from 'void *' (or 'const char *') to 'T'
for C sources that are compiled as C++. Only touches lines of the forms
    T x = expr;   x = expr;   return expr;
and prints anything it can't handle. Feed it the compiler output on stdin:
    python xdk/build.py moonlight | python xdk/scripts/autocast.py
"""
import re
import sys

ERR = re.compile(r"^\s*(?P<file>[A-Za-z]:\\[^(]+)\((?P<line>\d+)\) : error C2440: '(?P<ctx>[^']+)' : "
                 r"cannot convert from '(?P<from>[^']+)' to '(?P<to>[^']+)'")

fixes = {}
for raw in sys.stdin:
    m = ERR.match(raw)
    if m and m.group("from") in ("void *", "const void *", "const char *", "VIDEO_FRAME_HANDLE"):
        fixes.setdefault(m.group("file"), {})[int(m.group("line"))] = (m.group("ctx"), m.group("to"))

for path, lines in fixes.items():
    with open(path, encoding="utf-8", newline="") as f:
        text = f.read().split("\n")
    for ln, (ctx, to) in sorted(lines.items()):
        src = text[ln - 1]
        # For "T *x = expr;" use T as written, so "struct foo *" keeps its
        # "struct" (the compiler's C++ spelling drops it, which C rejects).
        decl = re.match(r"\s*((?:const\s+)?(?:struct\s+)?\w+\s*\*+)\s*\w+\s*=", src)
        cast = f"({decl.group(1).strip()})" if ctx == "initializing" and decl else f"({to})"
        if cast in src:
            continue
        if ctx == "return":
            new = re.sub(r"\breturn\s+", "return " + cast, src, count=1)
        else:
            # first '=' that isn't part of ==, <=, >=, !=
            new = re.sub(r"(?<![=<>!])=(?!=)\s*", "= " + cast, src, count=1)
        if new == src:
            print(f"UNHANDLED {path}:{ln}: {src.strip()}")
        else:
            text[ln - 1] = new
            print(f"cast {path}:{ln}: {new.strip()}")
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write("\n".join(text))
