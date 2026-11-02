"""Find MSVC x86 RTTI for named classes in a PE: bases, vftables, virtual methods, code writing the vftable.

usage: python -I rtti.py <exe> <out.json> CClass [CClass ...]
"""
import json
import struct
import sys


class PE:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        pe = struct.unpack_from('<I', self.d, 0x3c)[0]
        nsec = struct.unpack_from('<H', self.d, pe + 6)[0]
        opt = pe + 24
        self.base = struct.unpack_from('<I', self.d, opt + 28)[0]
        osz = struct.unpack_from('<H', self.d, pe + 20)[0]
        self.secs = []
        for i in range(nsec):
            name, vsize, va, rsize, raw = struct.unpack_from('<8sIIII', self.d, opt + osz + i * 40)
            self.secs.append((name.rstrip(b'\0').decode(), va, max(vsize, rsize), raw, rsize))

    def sec(self, name):
        return next(s for s in self.secs if s[0] == name)

    def va(self, off):
        for _, va, size, raw, rsize in self.secs:
            if raw <= off < raw + rsize:
                return self.base + va + off - raw

    def off(self, va):
        rva = va - self.base
        for _, sva, size, raw, rsize in self.secs:
            if sva <= rva < sva + rsize:
                return raw + rva - sva

    def u32(self, va):
        return struct.unpack_from('<I', self.d, self.off(va))[0]

    def in_text(self, va):
        _, sva, size, _, _ = self.sec('.text')
        return self.base + sva <= va < self.base + sva + size

    def find_u32(self, value, section, aligned=True):
        _, _, _, raw, rsize = self.sec(section)
        needle = struct.pack('<I', value)
        hits, i = [], self.d.find(needle, raw, raw + rsize)
        while i != -1:
            if not aligned or i % 4 == 0:
                hits.append(self.va(i))
            i = self.d.find(needle, i + 1, raw + rsize)
        return hits

    PROLOGUES = (b'\x55\x8b\xec', b'\x6a\xff\x68', b'\x8b\xff\x55', b'\x83\xec', b'\x81\xec',
                 b'\x56\x8b\xf1', b'\x53\x8b\xd9', b'\x57\x8b\xf9', b'\x51\x56', b'\x56\x57', b'\x53\x56')

    def func_start(self, va):
        # ponytail: nearest prologue right after a ret/int3 before va. Can stop early on a
        # C3/CC byte inside an instruction; check the decompiled output writes the vftable.
        d, o = self.d, self.off(va)
        for p in range(o, max(o - 0x8000, 4), -1):
            after_end = d[p - 1] in (0xC3, 0xCC) or (d[p - 3] == 0xC2 and d[p - 1] == 0)
            if after_end and d[p:p + 3].startswith(self.PROLOGUES) or after_end and d[p:p + 2] in self.PROLOGUES:
                return self.va(p)


def td_name(pe, td):
    o = pe.off(td + 8)
    return pe.d[o:pe.d.index(b'\0', o)].decode()


def describe(pe, cls):
    o = pe.d.find(b'.?AV' + cls.encode() + b'@@\0')
    if o < 0:
        return {'class': cls, 'error': 'no TypeDescriptor'}
    td = pe.va(o) - 8
    out = {'class': cls, 'type_descriptor': hex(td), 'vftables': []}
    for ref in pe.find_u32(td, '.rdata'):
        col = ref - 12
        if pe.u32(col) != 0:
            continue
        try:
            chd = pe.u32(col + 16)
            n, bca = pe.u32(chd + 8), pe.u32(chd + 12)
            bases = [td_name(pe, pe.u32(pe.u32(bca + 4 * i))) for i in range(n)]
        except TypeError:  # the TypeDescriptor reference wasn't a CompleteObjectLocator
            continue
        for slot in pe.find_u32(col, '.rdata'):
            vft = slot + 4
            methods = []
            while pe.in_text(pe.u32(vft + 4 * len(methods))):
                methods.append(hex(pe.u32(vft + 4 * len(methods))))
            writers = sorted({pe.func_start(r) for r in pe.find_u32(vft, '.text', aligned=False)} - {None})
            out['vftables'].append({'vftable': hex(vft), 'offset_in_object': pe.u32(col + 4),
                                    'bases': bases, 'methods': methods,
                                    'vftable_writers': [hex(w) for w in writers]})
    return out


if __name__ == '__main__':
    pe = PE(sys.argv[1])
    res = [describe(pe, c) for c in sys.argv[3:]]
    json.dump(res, open(sys.argv[2], 'w'), indent=1)
    for r in res:
        print(r['class'], r.get('error', ''))
        for v in r.get('vftables', []):
            print('  vftable', v['vftable'], 'off', v['offset_in_object'], 'bases', v['bases'])
            print('    methods', len(v['methods']), 'writers', v['vftable_writers'])


def find_calls(pe, target):
    """VAs of `call rel32` instructions in .text that land on target."""
    _, sva, _, raw, rsize = pe.sec('.text')
    d, hits = pe.d, []
    i = d.find(b'\xe8', raw, raw + rsize)
    while i != -1:
        if pe.base + sva + (i - raw) + 5 + struct.unpack_from('<i', d, i + 1)[0] == target:
            hits.append(pe.va(i))
        i = d.find(b'\xe8', i + 1, raw + rsize)
    return hits


def function_entries(pe, cache=None):
    """Sorted VAs that are surely function starts: targets of call rel32 inside .text."""
    import os
    if cache and os.path.exists(cache):
        return json.load(open(cache))
    _, sva, size, raw, rsize = pe.sec('.text')
    lo, hi = pe.base + sva, pe.base + sva + size
    d, found = pe.d, set()
    i = d.find(b'\xe8', raw, raw + rsize)
    while i != -1:
        t = pe.base + sva + (i - raw) + 5 + struct.unpack_from('<i', d, i + 1)[0]
        if lo <= t < hi and t % 2 == 0 or lo <= t < hi and d[pe.off(t) - 1] in (0xCC, 0xC3, 0x90):
            found.add(t)
        i = d.find(b'\xe8', i + 1, raw + rsize)
    out = sorted(found)
    if cache:
        json.dump(out, open(cache, 'w'))
    return out


def containing_function(entries, va):
    import bisect
    i = bisect.bisect_right(entries, va) - 1
    return entries[i] if i >= 0 else None


def string_refs(pe, text):
    """VAs in .text that use the address of this NUL-terminated ASCII string as an immediate."""
    o = pe.d.find(text.encode() + b'\0')
    while o > 0 and pe.d[o - 1] != 0:  # the match may be the tail of a longer string
        o = pe.d.find(text.encode() + b'\0', o + 1)
    return pe.va(o), pe.find_u32(pe.va(o), '.text', aligned=False)
