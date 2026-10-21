#!/usr/bin/env python3
"""Copy the data of a sparse file into another file at an offset.

Usage: sparse-copy.py SRC DST OFFSET

Only the parts of SRC that hold data are read and written (found with
SEEK_DATA and SEEK_HOLE); holes stay holes in DST, which is not truncated.
"""

import os
import sys


def main():
    src, dst, offset = sys.argv[1], sys.argv[2], int(sys.argv[3])
    fin = os.open(src, os.O_RDONLY)
    fout = os.open(dst, os.O_WRONLY)
    size = os.fstat(fin).st_size
    pos = 0
    while pos < size:
        try:
            start = os.lseek(fin, pos, os.SEEK_DATA)
        except OSError:
            break                       # only a hole is left
        end = os.lseek(fin, start, os.SEEK_HOLE)
        while start < end:
            chunk = os.pread(fin, min(end - start, 8 << 20), start)
            os.pwrite(fout, chunk, offset + start)
            start += len(chunk)
        pos = end
    os.close(fin)
    os.close(fout)


if __name__ == "__main__":
    main()
