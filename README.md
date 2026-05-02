# MoonlightVibe

**MoonlightVibe** is the companion Moonlight client for [MultiSeat](https://github.com/vibesoftwarecoder/MultiSeat) — a system that runs multiple simultaneous game streaming sessions on one Windows host.

It is a fork of [moonlight-stream/moonlight-qt](https://github.com/moonlight-stream/moonlight-qt) with two additions: MultiSeat seat auto-discovery and microphone passthrough support.

---

## What's different from upstream Moonlight

| Feature | MoonlightVibe | Upstream Moonlight |
|---|---|---|
| MultiSeat auto-discovery | ✅ Seats appear automatically in the computer list | ❌ Manual host entry required |
| Mic passthrough | ✅ Client mic streamed to host via ApolloVibe | ❌ Not available |
| Branding | MoonlightVibe | Moonlight |

Everything else — hardware video decoding, H.264/HEVC/AV1, HDR, surround sound, gamepad support, touch input — is inherited from upstream Moonlight.

---

## Download

Get the latest release from the [Releases page](https://github.com/vibesoftwarecoder/MoonlightVibe/releases/latest).

Extract the zip and run `MoonlightVibe.exe` — no installer required, fully portable.

---

## MultiSeat seat auto-discovery

When MultiSeat is running on the local machine, MoonlightVibe automatically discovers all active seats and lists each one as a separate server in the computer list. No manual host entry or port configuration needed — seats appear within ~15 seconds of becoming ready.

This works over the LAN too: if you point MoonlightVibe at the MultiSeat host IP, it will discover and list all seats on that machine.

---

## Microphone passthrough

MoonlightVibe streams your microphone to the host and plays it back through `Speakers (Steam Streaming Microphone)`. Games and apps on the host should select `Microphone (Steam Streaming Microphone)` as their input device.

Requires [ApolloVibe](https://github.com/vibesoftwarecoder/Apollo) on the host side with `stream_mic = enabled` in the seat config. MultiSeat enables this automatically.

---

## Requirements

- Windows 10/11 x64
- [MultiSeat](https://github.com/vibesoftwarecoder/MultiSeat) + [ApolloVibe](https://github.com/vibesoftwarecoder/Apollo) on the host for full feature support
- Standard Moonlight-compatible host (e.g. upstream Sunshine) works for basic streaming without mic or auto-discovery

---

## Usage with standard Moonlight hosts

MoonlightVibe is fully compatible with any Moonlight-compatible streaming host (Sunshine, upstream Apollo, NVIDIA GameStream). Auto-discovery only activates when a MultiSeat service is detected. Mic passthrough requires ApolloVibe with `stream_mic = enabled`.

---

## Building

MoonlightVibe uses the same build system as upstream moonlight-qt — Qt 6.7+ and Visual Studio 2022 on Windows.

See the upstream [build instructions](https://github.com/moonlight-stream/moonlight-qt#building) for the full dependency list.

---

## License

MoonlightVibe is licensed under the same terms as Moonlight — [GPL-3.0](LICENSE).

Mic passthrough patches by [logabell](https://github.com/logabell/moonlight-qt-mic).
