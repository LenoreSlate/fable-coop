"""Player 2 puppet tests through the bridge.

usage: python -I puppet.py <profile> spawn [CREATURE_DEF] [dx dy]
       python -I puppet.py <profile> follow [CREATURE_DEF] [seconds] [delay]
           spawns a creature under scripted control, then streams the hero's position to it at ~20 Hz
           like network samples; the proxy replays them `delay` seconds late (default 0.5) through the
           game's own move-to orders, so the puppet walks or runs the hero's path with matching legs
"""
import struct
import sys
import time

sys.path.insert(0, 'coop-anything/tools/bridge')
from bridge_client import Bridge, RpcError, endpoint_from_file

HERO_VFTABLE = 0x2f1dbb4       # CThingPlayerCreature
NAVIGATOR_VFTABLE = 0x2f079ac  # CTCPhysicsNavigator, the physics component of NPCs
CONTROLLED_VFTABLE = 0x2f0764c # CTCPhysicsControlled, the physics component of hero creatures
PHYS = 0x6c                    # hero -> CTCPhysicsControlled
POS, FACING = 0x0c, 0x5c       # in the physics component: x,y,z (z up) and facing x,y
DEFAULT_DEF = 'CREATURE_OAKVALE_VILLAGER_MALE_UNEMPLOYED'
SCRIPTED_CLEAR = 0x1d0fee0     # scripted actor slot 0x28: empties the creature's scripted action queue


def u32(g, addr):
    return struct.unpack('<I', bytes.fromhex(g.call('mem.read', {'addr': hex(addr), 'size': 4})))[0]


def hero_physics(g):
    for a in g.call('fable.instances', {'vftable': hex(HERO_VFTABLE), 'limit': 20}):
        try:
            phys = u32(g, int(a, 16) + PHYS)
            pos = g.call('mem.floats', {'addr': hex(phys + POS), 'count': 3})
        except RpcError:
            continue  # stack leftover, not a live hero
        if all(abs(v) < 1e6 for v in pos) and any(pos):
            return phys
    raise SystemExit('hero not found')


def navigators(g):
    """Physics components of every creature (NPC and hero kinds share the CTCPhysicsBase layout)."""
    return {a for v in (NAVIGATOR_VFTABLE, CONTROLLED_VFTABLE)
            for a in g.call('fable.instances', {'vftable': hex(v), 'limit': 1000})}


def spawn(g, define, x, y, z, scripted=False):
    """Returns (physics component, scripted actor or None)."""
    before = navigators(g)
    r = g.call('fable.spawn', {'def': define, 'x': x, 'y': y, 'z': z, 'scripted': int(scripted)})
    print('spawn', r)
    for h in sorted(navigators(g) - before):
        p = g.call('mem.floats', {'addr': hex(int(h, 16) + POS), 'count': 3})
        if abs(p[0] - x) < 3 and abs(p[1] - y) < 3:
            return int(h, 16), int(r['actor'], 16) if scripted else None
    raise SystemExit('spawned creature not found among navigators')


def main(profile, cmd='spawn', define=DEFAULT_DEF, *rest):
    ep = endpoint_from_file('testinstall/Fable Anniversary/CoopProfiles/%s/bridge-endpoint.json' % profile)
    with Bridge.connect(ep, wait_s=10) as g:
        phys = hero_physics(g)
        x, y, z = g.call('mem.floats', {'addr': hex(phys + POS), 'count': 3})
        if cmd == 'spawn':
            dx, dy = (float(v) for v in (rest or ('2', '0')))
            print('puppet physics', hex(spawn(g, define, x + dx, y + dy, z)[0]))
            f0 = g.call('fable.frames')
            time.sleep(3)
            print('frames in the 3 s after spawn:', g.call('fable.frames') - f0)
            return
        seconds = float(rest[0]) if rest else 30
        delay = float(rest[1]) if len(rest) > 1 else 0.5
        puppet, actor = spawn(g, define, x, y + 2, z, scripted=True)
        if not actor:
            raise SystemExit('could not take scripted control of the creature')
        print('puppet physics', hex(puppet), 'actor', hex(actor), 'replaying the hero for', seconds, 's,', delay, 's late')
        end = time.time() + seconds
        frames, frames_at = g.call('fable.frames'), time.time()
        try:
            while time.time() < end:
                f = g.call('fable.frames')
                if f != frames:
                    frames, frames_at = f, time.time()
                elif time.time() - frames_at > 3:
                    raise SystemExit('game frames stuck at %d for 3 s: game thread hung' % f)
                x, y, z = g.call('mem.floats', {'addr': hex(phys + POS), 'count': 3})
                fx, fy = g.call('mem.floats', {'addr': hex(phys + FACING), 'count': 2})
                g.call('fable.puppet', {'phys': hex(puppet), 'actor': hex(actor),
                                        'x': x, 'y': y, 'z': z, 'fx': fx, 'fy': fy, 'delay': delay})
                time.sleep(0.05)  # ~20 Hz, like network samples
        finally:
            g.call('fable.puppet', {'phys': 0})
            g.call('fable.call', {'fn': hex(SCRIPTED_CLEAR), 'self': hex(actor)})
        print('puppet now at', g.call('mem.floats', {'addr': hex(puppet + POS), 'count': 3}))


if __name__ == '__main__':
    main(*sys.argv[1:])
