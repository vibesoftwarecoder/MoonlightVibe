# MoonlightVibe

MoonlightVibe is the companion streaming client for [MultiSeat](https://github.com/vibesoftwarecoder/MultiSeat) — a system that lets multiple people play games simultaneously on a single Windows PC, each on their own screen.

It is a fork of [moonlight-qt](https://github.com/moonlight-stream/moonlight-qt) with MultiSeat-specific additions. It works with [ApolloVibe](https://github.com/vibesoftwarecoder/Apollo), the companion streaming server.

[![Build](https://img.shields.io/github/actions/workflow/status/vibesoftwarecoder/MoonlightVibe/build.yml?branch=moonlightvibe-dev)](https://github.com/vibesoftwarecoder/MoonlightVibe/actions)
[![Latest Release](https://img.shields.io/github/v/release/vibesoftwarecoder/MoonlightVibe)](https://github.com/vibesoftwarecoder/MoonlightVibe/releases/latest)

---

## What's different from upstream Moonlight

| Feature | MoonlightVibe | Upstream Moonlight |
|---|---|---|
| MultiSeat seat auto-discovery | Seats appear automatically within ~15 seconds | N/A |
| Mic passthrough to host | Yes (via ApolloVibe) | No |
| Per-seat stream settings | Bitrate + codec override per host | Global only |
| Configurable gamepad quit combo | 7 combos + disable | Hardcoded |

### Seat auto-discovery
When MultiSeat is running, each seat registers itself as a separate host. Open MoonlightVibe on any seat and your seat's ApolloVibe instance will appear automatically — no manual IP entry needed.

### Per-seat stream settings
Right-click any host → **Stream Settings** to override the bitrate and video codec for that specific seat. Leave fields blank to inherit your global settings.

### Configurable gamepad quit combo
Go to **Settings → Gamepad** to choose which button combination quits the stream. Options:

| Setting | Buttons |
|---|---|
| Default | Start + Select + L1 + R1 |
| Select + L1 + R1 + X | |
| Select + L1 + R1 + Y | |
| Start + L1 + R1 + A | |
| Start + L1 + R1 + B | |
| L1 + R1 + X + Y | |
| L1 + R1 + A + B | |
| Disabled | — |

### Mic passthrough
ApolloVibe routes microphone audio from the client back to the seat's user account on the host, so voice chat works in games without any extra setup.

---

## Download

**[Latest release → Windows x64](https://github.com/vibesoftwarecoder/MoonlightVibe/releases/latest)**

Extract the zip and run `MoonlightVibe.exe` — no installer needed.

---

## Setup

1. Install and configure [MultiSeat](https://github.com/vibesoftwarecoder/MultiSeat) on the host PC.
2. Download MoonlightVibe on the client machine (or on the host for local seats).
3. Launch MoonlightVibe — your seat's host will appear automatically within ~15 seconds.
4. Click the host to pair, then select a game.

---

## Related projects

| Project | Role |
|---|---|
| [MultiSeat](https://github.com/vibesoftwarecoder/MultiSeat) | Host service — manages seats, displays, audio, sessions |
| [ApolloVibe](https://github.com/vibesoftwarecoder/Apollo) | Streaming server (Apollo fork) running on each seat |
| MoonlightVibe | This repo — streaming client |

---

## Building

### Requirements
- Qt 6.7 SDK or later
- Visual Studio 2022 (MSVC — MinGW not supported)
- MSYS2 UCRT64 (for the CMake-based submodule dependencies)

### Steps
```bash
git clone https://github.com/vibesoftwarecoder/MoonlightVibe.git
cd MoonlightVibe
git submodule update --init --recursive
# Open moonlight-qt.pro in Qt Creator, or build from command line:
qmake moonlight-qt.pro
make release
```

The CI workflow (`.github/workflows/build.yml`) shows the exact build steps used for releases.

---

## Credits

MoonlightVibe is built on top of [moonlight-qt](https://github.com/moonlight-stream/moonlight-qt) by the Moonlight team. All upstream features are preserved.
