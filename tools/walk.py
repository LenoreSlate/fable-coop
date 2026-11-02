"""Snapshot an object and every object it points to, naming each by RTTI.

usage: python -I walk.py <profile> <hex addr> <out.json>
"""
import json
import struct
import sys

sys.path.insert(0, 'coop-anything/tools/bridge')
sys.path.insert(0, 'fable-coop/tools')
from bridge_client import Bridge, RpcError, endpoint_from_file
from rtti import PE

EXE = 'testinstall/Fable Anniversary/Binaries/Win32/Fable Anniversary.exe'
pe = PE(EXE)


def class_name(vft_runtime, exe_base):
    """RTTI name of the class whose vftable is at this runtime address, or None."""
    va = vft_runtime - exe_base + pe.base
    try:
        col = pe.u32(va - 4)
        name_off = pe.off(pe.u32(col + 12) + 8)
        return pe.d[name_off:pe.d.index(b'\0', name_off)].decode()[4:-2]
    except (TypeError, struct.error, ValueError):
        return None


def main(profile, root, out):
    ep = endpoint_from_file('testinstall/Fable Anniversary/CoopProfiles/%s/bridge-endpoint.json' % profile)
    with Bridge.connect(ep, wait_s=10) as g:
        exe_base = int(g.call('fable.base')['exe_base'], 16)
        image_end = exe_base + pe.sec('.text')[1] + 0x3000000

        def read(addr, size):
            try:
                return bytes.fromhex(g.call('mem.read', {'addr': hex(addr), 'size': size}))
            except RpcError:
                return None

        snap = {'root': hex(root), 'objects': {}}
        top = read(root, 2048)
        snap['objects'][hex(root)] = {'from': None, 'class': class_name(struct.unpack_from('<I', top)[0], exe_base), 'hex': top.hex()}
        for off in range(0, len(top), 4):
            p = struct.unpack_from('<I', top, off)[0]
            if p < 0x10000 or exe_base <= p < image_end or hex(p) in snap['objects']:
                continue
            body = read(p, 512)
            if body is None:
                continue
            snap['objects'][hex(p)] = {'from': '+0x%03x' % off, 'class': class_name(struct.unpack_from('<I', body)[0], exe_base), 'hex': body.hex()}
        json.dump(snap, open(out, 'w'))
        for a, o in snap['objects'].items():
            print(o['from'], a, o['class'])


if __name__ == '__main__':
    main(sys.argv[1], int(sys.argv[2], 16), sys.argv[3])
