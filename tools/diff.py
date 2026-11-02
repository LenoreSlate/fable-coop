"""Floats that changed between two mem.read snapshots: python -I diff.py a.json b.json [base]"""
import json
import struct
import sys

a, b = (bytes.fromhex(json.load(open(p))) for p in sys.argv[1:3])
base = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0
for off in range(0, min(len(a), len(b)) - 3, 4):
    fa, fb = struct.unpack_from('<f', a, off)[0], struct.unpack_from('<f', b, off)[0]
    ia, ib = struct.unpack_from('<I', a, off)[0], struct.unpack_from('<I', b, off)[0]
    if ia != ib:
        plausible = all(1e-4 < abs(x) < 1e6 or x == 0 for x in (fa, fb))
        print('+0x%03x  %14.4f -> %14.4f  %s' % (off, fa, fb, '' if plausible else '(int %d -> %d)' % (ia, ib)))
