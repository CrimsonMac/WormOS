#!/usr/bin/env python3
"""Apply recorded owners and modes to a copy of the root filesystem.

Usage: apply-ownership.py LIST DIR

LIST holds NUL-separated records "UID GID MODE TYPE PATH" (find -printf
'%U %G %m %y %P\\0', run under fakeroot). Runs as root. Modes are set after
owners, since changing the owner clears set-ID bits.
"""

import os
import sys


def main():
    listfile, top = sys.argv[1], sys.argv[2]
    with open(listfile, "rb") as f:
        records = [r for r in f.read().split(b"\0") if r]
    recorded = set()
    for rec in records:
        uid, gid, mode, kind, path = rec.split(b" ", 4)
        recorded.add(path)
        full = os.path.join(top.encode(), path) if path else top.encode()
        os.lchown(full, int(uid), int(gid))
        if kind != b"l":
            os.chmod(full, int(mode, 8))

    # Anything not in the list would keep the build user as its owner.
    for dirpath, dirnames, filenames in os.walk(top.encode()):
        for name in dirnames + filenames:
            rel = os.path.relpath(os.path.join(dirpath, name), top.encode())
            if rel not in recorded:
                sys.exit(f"no recorded owner for {rel.decode(errors='replace')}")

if __name__ == "__main__":
    main()
