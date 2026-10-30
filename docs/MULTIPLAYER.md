# Multiplayer: DS-to-DS local wireless

The original's LAN mode uses a TCP connection (port 47474) carrying newline-delimited JSON messages, and UDP
broadcast (port 47475) so joiners can find hosts. A DS has no IP network without a router, so the port uses the
DS's own **local wireless multiplayer** (the mode DS games use for "DS Wireless Play"): one console is the host
(it acts as the access point), the other joins it directly. No router, no internet, no Wi-Fi settings.

Implementation: `source/netplay.c` (transport), `source/scn_lan.c` (lobby screens and the start message), the
battle hook in `core/battle.c` (`send`) and `net_battle_tick`.

## Discovery and joining

* **Host** (`LAN PLAY › HOST A GAME` after picking rules): `Wifi_MultiplayerHostMode`, a beacon with the game id
  `0x0054544B` ('TTK') and the host's profile name. Channel 1, 6 or 11 (◄ ► on the host screen; 6 by default).
* **Guest** (`LAN PLAY › JOIN A GAME`): scans all channels, lists beacons with the Terminal Tanks game id
  (name, channel, players 1/2, open/full), and joins the chosen one.
* Handshake (same as the original): guest → `HELLO {protocol, version, loadout}`. The host refuses with
  `REFUSE "VERSION MISMATCH: HOST …"` if the protocol or version string differs, otherwise answers
  `WELCOME "<rules summary>"`, which the guest shows. The host presses A to start. One guest per game; new joins
  are refused once a guest is in.

## Transport: a reliable ordered byte stream

dswifi's multiplayer mode gives the host a **CMD** frame it sends to clients and each client a **REPLY** frame
back, once per exchange. Both are unreliable. On top of them `netplay.c` builds a tiny go-back-N stream:

```
packet (100 bytes) = magic 0x5454 | proto | n | off (u32) | ack (u32) | data[88]
```

* Every frame (30 Hz) each side sends a packet carrying the oldest unacknowledged bytes of its outgoing stream
  (`off` = stream offset of `data[0]`, `n` bytes) and `ack` = how many bytes of the peer's stream it has received
  in order. Lost or duplicated packets are harmless: data is resent until acknowledged, and only bytes at the
  next expected offset are delivered.
* The stream carries messages framed as `[type u8][len u16][payload]`, max 400 bytes; the receiver keeps an
  inbox with the original's semantics (`peek` the oldest of a set of types, `take`, `drop`).
* Bandwidth: 88 bytes × 30 Hz ≈ 2.6 KB/s each way, far more than the game needs (an aim update is 20 bytes).
* Link loss: no packet for 6 s, or the host losing its associated client, marks the link dead; the screens show
  "CONNECTION LOST" / "OPPONENT LEFT" and return to the menu, like the original's `NetLink` closing.

## Messages (the original's set)

| Type | Payload | Use |
|---|---|---|
| HELLO / WELCOME / REFUSE | version, guest loadout / rules text / reason | lobby handshake |
| START | mode, seed, wind, the two players (tank, colours, ammo, HP, names, and any custom-tank records) or co-op mission id | host starts the match; both build the same session from it |
| RPS / RPSFIRST | hand / who shoots first | rock-paper-scissors across the link |
| AIM | tank, angle, power (f64) | live aim echo, at most every 0.08 s while aiming (as the original) |
| FIRE / SKIP | tank, angle, power | a shot / a skipped turn (SPECIAL charge) |
| MAP | map index | "pick next map" rule: the round loser's pick |
| READY | – | both players confirmed the round result |
| QUIT | – | the other player left |

Custom workshop tanks travel inside START as complete design records, so the guest can draw a tank it does not
own, as the original sends the design JSON.

## Lockstep battles

Both consoles run the full simulation from the same seed. Only inputs cross the link: the local player's aim
changes, fire and skip. The remote tank's shot is applied when its FIRE arrives (stale AIMs from an earlier turn
are discarded), then both consoles resolve the identical flight, damage and terrain change. The host also runs
the AI for co-op AI tanks; the guest treats them as remote and follows the host's FIRE messages.

Determinism holds because both consoles run the same code on the same CPU with the same math library (the
simulation never reads the clock for decisions; frame `dt` only paces animations).

## Tested

`tests/harness/run_lan_versus.py` runs two emulated consoles linked by melonDS's local-multiplayer emulation:
discovery, join, handshake, RPS, then 30 turn cycles (~30 shots over 2 rounds, including a round end and the next
round on a new map). Both consoles log every phase change with a hash of terrain + HP; the test requires the two
logs to be identical and every sent shot to arrive intact. **Result: PASS.**

Not tested: real hardware radio conditions, disconnects mid-match, co-op missions over LAN (implemented: the host
sends `START` with a mission id and both run `session_init_mission`).
