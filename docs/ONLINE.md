# SpaghettiKart Online (unofficial fork)

> Fan project. **Not affiliated with or endorsed by Harbour Masters or Nintendo.** It is a fork of
> [HarbourMasters/SpaghettiKart](https://github.com/HarbourMasters/SpaghettiKart) that adds online play.
> It contains no Nintendo assets: you need your own Mario Kart 64 US ROM (.z64), exactly like upstream.
> Beta software, expect bugs.
>
> *En español:* fork no oficial de SpaghettiKart con juego en línea (hasta 16 jugadores por sala). Beta.

## What it adds

- Online rooms with a short code, a public room list, up to **16 human players** per room.
- Lobby with per-player ping (green / yellow / red), "unstable connection" warning, optional max-ping kick.
- **Chaos mode** (timed items for everybody instead of item boxes), optional bots, and a **lite mode** for weak PCs.
- UI in English and Spanish (CVar `gOnline.Lang`, defaults to the Windows UI language).
- A small relay server (`server/relay_server.py`) so nobody has to open ports.

In the game: on the GAME SELECT screen press **Z** to open the ONLINE menu.

## How it works: deterministic lockstep

Every PC simulates the entire race. Only controller inputs travel over the network, never game state.
That keeps bandwidth small (roughly 4 KB/s up and 20 KB/s down per player in a full 16-player room) but requires the simulation to be bit-for-bit identical on every machine.

### Relay as input aggregator

`server/relay_server.py` is a plain Python `asyncio` UDP server. Besides pairing host and clients through a room code,
during a race it acts as an **input aggregator**:

1. Each client sends its pad state for the upcoming ticks (current tick plus the last few, so a lost packet is not fatal).
2. The relay assembles one frame per tick containing the pads of all humans and broadcasts it. Clients only advance a
   tick when they hold the frame for it.
3. Each player gets an **input delay** (2 to 16 ticks at 30 Hz) derived from its measured RTT and jitter. The relay raises
   it when a player starts arriving late and lowers it again when the connection recovers.
4. If a player's input is still missing at the deadline, the relay **fills it with that player's last pad state**, so one
   slow connection does not freeze the other 15 players.
5. A player silent for 3 s is dropped from the race (the others keep going).

The relay never runs game physics. It cannot cheat for you and does not know what a kart is.

### What had to change to make the game deterministic

- Separate RNG streams: one for the simulation, one for rendering/cosmetics.
- The whole circuit is simulated regardless of the camera (`online_no_cull()`), so culling cannot alter gameplay.
- FMA is disabled (`_set_FMA3_enable(0)` in `Game.cpp`) and the logic tick count is fixed.
- Online races ignore local CVars that would change the simulation (`online_cvar_int` returns defaults).
- 16 karts (`NUM_PLAYERS = 16`), and item distribution, HUD, rankings and results reworked for 16 players.
- A per-second state hash (karts + RNG) is logged by every client, which is how desyncs were found and fixed.

## Building

Same as upstream, see [BUILDING.md](BUILDING.md). The online code lives mostly in:

| Path | What |
|---|---|
| `src/port/net/Net.cpp`, `Net.h` | UDP client, lobby protocol (version 6), lockstep loop, ping/quality, i18n helper `Net::TR` |
| `src/port/net/NetBridge.*` | glue between the netcode and the game |
| `src/online_race.c`, `online_race.h` | race setup, Chaos item roll, bots on/off, 16-kart views |
| `src/online_menu.c` | in-game ONLINE menu and lobby |
| `src/port/ui/OnlineWindow.cpp`, `OnlineOverlay.cpp`, `OnlineMenuBridge.cpp` | ImGui window and in-race overlay |
| `server/relay_server.py` | the relay / aggregator |
| `tools/online/` | packaging script and asset helpers |

## Running your own relay

```
python3 server/relay_server.py --port 7778 --verbose
```

Open that UDP port in the server firewall. In the game, set the relay host and port in the online menu
(or `gOnline.Relay` / `gOnline.RelayPort` in `spaghettify.cfg.json`). There is no authentication and no encryption:
use it for casual play, not for anything sensitive.

The relay also has test-only flags to emulate bad networks (`--sim-owd`, `--sim-jitter`, `--sim-loss`, `--sim-extra`,
`--sim-degrade`, `--agg-mode`); run it with `--help` for the list.

## Environment variables

| Variable | Effect |
|---|---|
| `SK_ONLINE_BOTS=0` | race without CPU bots (only the humans) |
| `SK_NET_LOGFILE=<file>` | also write the netcode log to a file |
| `SK_CONSOLE=1` | show the console window (hidden by default on Windows) |
| `SK_RELAY_DEBUG=1` | (relay) extra timing diagnostics |
| `SK_ONLINE_TEST_*` | automation hooks used for headless testing (auto-drive, quick race chains, fake peers) |

## Known limits

- Everyone must run the same build (same protocol version).
- Weak PCs or Wi-Fi with spikes show up as late inputs for that player. The lobby ping column and the in-race warning help find them.
- The host leaving the room is not handled gracefully yet.
- Larger scale (many simultaneous 16-player rooms on one small server) is untested.

## Credits

Base game: [SpaghettiKart](https://github.com/HarbourMasters/SpaghettiKart) by Harbour Masters and contributors.
Engine and tools: libultraship and Torch (MIT). Mario Kart is a trademark of Nintendo.
