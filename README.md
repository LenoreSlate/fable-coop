# fable-coop

Online co-op prototype for **Fable Anniversary** (Steam, AppID 288470, exe 1.0.8767.0, SHA-256
`2a95eea3c2cce9b47ca0f454a605b6952216f5d25158efd12ba48b70130989f2`). Not playable together yet.

Built with [coop-anything](https://github.com/cranebeaker/coop-anything); the write-up, every address and the
test log are in [games/fable-anniversary](https://github.com/cranebeaker/coop-anything/tree/main/games/fable-anniversary).

## What works (two copies on one PC)

- Each copy shows the other player's hero as a puppet in the same region: position, facing, animations.
- The guest follows the host's time of day.
- Enemies placed in the world (not yet those spawned by generators): health and death are shared both ways, and each
  enemy is run by the game of the player who came within 30 m of it first while the other game mirrors its position
  and animations. Tested with one player near an enemy (a stone troll wakes for the guest with the host far away);
  with both players near the same enemy it still fights only its owner's hero.
- Several copies run side by side, keep running in the background, and save into their own profile folder
  (never the player's real save or Steam Cloud).

Not shared yet: quests, items, money. Transport: local UDP, or the Steam relay through the game's own
`steam_api.dll` (both players need to own the game; checked so far only by a one-account loopback test).

## Play with a friend (Steam)

Both players need Fable Anniversary on Steam. Each runs `FableCoop.bat` (one file, built by `kit/make_kit.py`;
the mod DLL and its SHA-256 are inside) and picks:

1. **Install**: finds the game through Steam's library folders (or `--game-dir`), refuses a game build other than
   the one tested, backs up any `dinput8.dll` already there, writes `dinput8.dll` and `steam_appid.txt`.
2. **Play**: shows your SteamID64 (send it to your friend), asks for theirs and who hosts, copies your saves into
   `<game dir>\CoopProfiles\coop` the first time (the originals are never touched), and starts the game.
3. **Uninstall**: removes what install created and puts the old `dinput8.dll` back. Co-op saves stay.

Started from Steam as usual, the game is unmodded: the DLL does nothing unless the kit (or a test) set
`FABLE_COOP_PROFILE`. Windows may show "Windows protected your PC" for a downloaded `.bat`: More info > Run anyway.
Command line: `FableCoop.bat install | play host|guest <SteamID64> | uninstall [--game-dir <dir>] [--yes]`.
Tests: `powershell -File kit\test_kit.ps1 <fixture>` (fake Steam folder; see the script header).

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
| `FABLE_COOP_PEER` | `127.0.0.1:27961` | The other copy over UDP (needs `FABLE_COOP_LISTEN`), or `steam:<SteamID64>` for the other player over Steam (`steam:me`: loopback self-test) |
| `FABLE_COOP_HOST` | `1` | Set on one copy only: its time of day wins, and it keeps an enemy both claim at once |

## Tools

`tools/` holds the Python scripts used to map the game through the bridge (`python -I <script>`; usage in
each file's docstring): `rtti.py` (MSVC RTTI walker), `walk.py`, `diff.py`, `regionsnap.py`, `q.py`,
`puppet.py`.

## License

MIT, see [LICENSE](LICENSE). No game files are included.
