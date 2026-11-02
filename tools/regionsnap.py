"""Snapshot world objects to find where the current region is kept (diff before/after a region change).

usage: python -I regionsnap.py <profile> <out.json>
       python -I regionsnap.py diff <p1_before> <p2_before> <p1_after> <p2_after>
Values that point at a CRegionDef are stored as "R<global def id>", so they compare across processes.
"""
import json
import struct
import sys

sys.path.insert(0, 'coop-anything/tools/bridge')
from bridge_client import Bridge, RpcError, endpoint_from_file

ROOTS = {'CWorld': 0x2f26c10, 'CWorldMap': 0x2f2b48c, 'CEngineWorldMap': 0x2eac410}
REGION_DEF = 0x2edd4bc


def snap(profile, out):
    with Bridge.connect(endpoint_from_file('testinstall/Fable Anniversary/CoopProfiles/%s/bridge-endpoint.json' % profile), wait_s=3) as g:
        def read(a, n):
            try:
                return bytes.fromhex(g.call('mem.read', {'addr': hex(a), 'size': n}))
            except RpcError:
                return None
        regions = {}
        for a in g.call('fable.instances', {'vftable': hex(REGION_DEF), 'limit': 500}):
            d = read(int(a, 16) + 0x20, 4)
            if d:
                regions[int(a, 16)] = struct.unpack('<I', d)[0]
        name = lambda x: 'R%x' % regions[x] if x in regions else x
        vals = {}
        for root, vft in ROOTS.items():
            base = int(g.call('fable.instances', {'vftable': hex(vft), 'limit': 10})[-1], 16)  # first ones are stack leftovers
            top = read(base, 0x800)
            for o in range(0, len(top), 4):
                x = struct.unpack_from('<I', top, o)[0]
                vals['%s+%x' % (root, o)] = name(x)
                sub = read(x, 0x200) if 0x10000 < x < 0x7fff0000 and x not in regions else None
                for o2 in range(0, len(sub or b''), 4):
                    vals['%s+%x+%x' % (root, o, o2)] = name(struct.unpack_from('<I', sub, o2)[0])
        json.dump(vals, open(out, 'w'))
        print(profile, len(vals), 'values,', sum(isinstance(v, str) for v in vals.values()), 'region refs')


def diff(p1b, p2b, p1a, p2a):
    a, b, c, d = (json.load(open(f)) for f in (p1b, p2b, p1a, p2a))
    for k in a:
        if k in b and k in c and k in d and a[k] == b[k] == c[k] and d[k] != b[k]:
            print(k, a[k], '->', d[k])


if __name__ == '__main__':
    diff(*sys.argv[2:]) if sys.argv[1] == 'diff' else snap(*sys.argv[1:])
