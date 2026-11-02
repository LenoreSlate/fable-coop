# fable-coop

Online co-op prototype for **Fable Anniversary** (Steam, AppID 288470, exe 1.0.8767.0, SHA-256
`2a95eea3c2cce9b47ca0f454a605b6952216f5d25158efd12ba48b70130989f2`). Not playable together yet.

Built with [coop-anything](https://github.com/cranebeaker/coop-anything); the write-up, every address and the
test log are in [games/fable-anniversary](https://github.com/cranebeaker/coop-anything/tree/main/games/fable-anniversary).

## What works (two copies on one PC)

- Each copy shows the other player's hero as a puppet in the same region: position, facing, animations.
- The guest follows the host's time of day.
- Several copies run side by side, keep running in the background, and save into their own profile folder
  (never the player's real save or Steam Cloud).

Not shared yet: enemies, quests, items, money. Transport is plain UDP; Steam relay is next.

## Build

32-bit MSVC (Build Tools 2022). The coopbridge headers come from a coop-anything checkout next to this repo
(`..\coop-anything`).

```
proxy\build.bat
```

## Try it

Use a **copy** of the game folder, never your Steam install. Put `proxy\build\dinput8.dll` in
`<game dir>\Binaries\Win32`, then start each copy with:

| Variable | Example | Meaning |
|---|---|---|
| `FABLE_COOP_PROFILE` | `p1` | Save folder: `<game dir>\CoopProfiles\<profile>` (log: `fable_coop.log` there) |
| `FABLE_COOP_LISTEN` | `27960` | Local UDP port |
| `FABLE_COOP_PEER` | `127.0.0.1:27961` | The other copy |
| `FABLE_COOP_HOST` | `1` | Set on one copy only: its time of day wins |

## Tools

`tools/` holds the Python scripts used to map the game through the bridge (`python -I <script>`; usage in
each file's docstring): `rtti.py` (MSVC RTTI walker), `walk.py`, `diff.py`, `regionsnap.py`, `q.py`,
`puppet.py`.

## License

MIT, see [LICENSE](LICENSE). No game files are included.
