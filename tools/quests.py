"""Quest manager state through the bridge.
usage: python -I quests.py <profile> [active|completed|complete|stop <quest> | succeed <quest> <offset> | obj <quest>]
succeed = what the co-op peer will do: write 1 to the quest script's own success byte (script object + offset; it
differs per script, 0x5c for Q_GuildTrainingWoodsMelee), so the script runs its own ending: end screen, rewards and the
story state it feeds to other scripts. Completing from outside (complete, then stop) skipped that state and broke the
guild training. obj dumps the script object's first 0x100 bytes.
CQuestManager at [0x3630360] (preferred base): +0x34/+0x38 vector of CCharString, +0x44 std::list of active
quest entries (entry +0x34 = name), +0x6c/+0x70 vector of CCharString. A CCharString is a pointer to
{?, char *text, int length}.
"""
import struct
import sys

sys.path.insert(0, 'coop-anything/tools/bridge')
from bridge_client import Bridge, RpcError, endpoint_from_file

QUEST_MANAGER = 0x3630360
GSI = 0x35bbc34
# CGameScriptInterface slots 0x4ac, 0x4b4, 0x4bc, 0x49c; each takes a CCharString * first
IS_ACTIVE, IS_COMPLETED, SET_COMPLETED, STOP = 0x1c91e50, 0x1c91e70, 0x1c91e90, 0x1c91e10


def rd(g, addr, size):
    return bytes.fromhex(g.call('mem.read', {'addr': hex(addr), 'size': size}))


def u32(g, addr):
    return struct.unpack('<I', rd(g, addr, 4))[0]


def text(g, cstr_obj):
    """CCharString value (the pointer it holds) -> str."""
    if not cstr_obj:
        return ''
    p, n = u32(g, cstr_obj + 4), u32(g, cstr_obj + 8)  # read only the length: a fixed size can cross into a free page
    return rd(g, p, min(n, 256)).decode('latin1') if p and n else ''


def strings(g, begin_addr):
    b, e = u32(g, begin_addr), u32(g, begin_addr + 4)
    return [text(g, u32(g, a)) for a in range(b, e, 4)][:300]


def active(g, qm):
    head = u32(g, qm + 0x44)
    node, out = u32(g, head), []
    while node != head and len(out) < 300:
        entry = u32(g, node + 8)
        out.append((hex(entry), text(g, u32(g, entry + 0x34)), rd(g, entry + 0x28, 2).hex()))
        node = u32(g, node)
    return out


def cstring(g, s):
    """New CCharString holding s (scratch memory, never freed) -> its address."""
    m = int(g.call('mem.alloc', {'size': 0x100}), 16)
    g.call('mem.write', {'addr': hex(m + 0x10), 'hex': s.encode().hex() + '00'})
    g.call('fable.call', {'fn': hex(0x16b7800), 'self': hex(m), 'args': [hex(m + 0x10), '0xffffffff']})
    return m


def gsi_call(g, base, fn, *args):
    gsi = u32(g, base + GSI - 0x400000)
    return int(g.call('fable.call', {'fn': hex(fn), 'self': hex(gsi), 'args': [hex(a) for a in args]}), 16)


def script_object(g, qm, quest):
    """Running entry +8 of the named quest script, or exit."""
    for e, name, flags in active(g, qm):
        if name == quest:
            return u32(g, int(e, 16) + 8)
    sys.exit('%s is not running' % quest)


def main(profile, cmd=None, quest=None, screen='1'):
    ep = endpoint_from_file('testinstall/Fable Anniversary/CoopProfiles/%s/bridge-endpoint.json' % profile)
    with Bridge.connect(ep, wait_s=10) as g:
        base = int(g.call('fable.base')['exe_base'], 16)
        qm = u32(g, base + QUEST_MANAGER - 0x400000)
        if cmd in ('succeed', 'obj'):
            obj = script_object(g, qm, quest)
            if cmd == 'obj':
                b = rd(g, obj, 0x100)
                for i in range(0, 0x100, 16):
                    print('%02x' % i, b[i:i + 16].hex(' '))
                return
            off = int(screen, 0)
            g.call('mem.write', {'addr': hex(obj + off), 'hex': '01'})
            print('succeed', quest, 'object', hex(obj), '+%#x = 1' % off)
            return
        if cmd:
            fn = {'active': IS_ACTIVE, 'completed': IS_COMPLETED, 'complete': SET_COMPLETED, 'stop': STOP}[cmd]
            args = (int(screen), 0, 0) if cmd == 'complete' else ()
            print(cmd, quest, gsi_call(g, base, fn, cstring(g, quest), *args) & 0xff)
            return
        print('quest manager', hex(qm))
        for e, name, flags in active(g, qm):
            print('active', e, flags, name)
        print('+0x34', strings(g, qm + 0x34))
        print('+0x6c', strings(g, qm + 0x6c))


if __name__ == '__main__':
    main(*sys.argv[1:])
