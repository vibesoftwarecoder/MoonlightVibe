# Privacy

MoonlightVibe does not collect, store or send personal data to us. We run no servers, and the app
has no analytics, telemetry or crash reporting.

It does make network connections. Some of them go to servers run by other people. This page lists
every one, so you know what leaves your device and why.

## Connections to your own hosts

- **Streaming.** MoonlightVibe connects to the hosts you add, or that it finds on your local network,
  to pair, list apps and stream. Your input, and your microphone if you turn on microphone
  passthrough, go to that host only.
- **Host discovery.** It looks for streaming hosts on your local network with mDNS.
- **MultiSeat seat discovery.** Every 15 seconds it checks up to four ports on each host you have
  already added, to find MultiSeat seats. It checks no other addresses.
- **Wake-on-LAN.** When you wake a host, it sends Wake-on-LAN packets to every address it knows for
  that host, including its internet address if it has one, and to broadcast addresses on your local
  network.

## Connections to other servers

Most of these are inherited from upstream Moonlight. Each one reveals your IP address to the server it
contacts, as any internet request does. None of them sends your hosts, your settings or anything
you stream.

| what | when | server | can you turn it off? |
|---|---|---|---|
| Update check | each time the app starts, on Windows and macOS | `api.github.com` (GitHub), to read MoonlightVibe's latest release | no |
| Game controller mappings | when controller support starts, downloaded only when a newer file exists | `moonlight-stream.org` | no |
| Network connection test | when adding a host fails, or when a stream fails to start or drops, to tell you whether your network blocks streaming | `qt.conntest.moonlight-stream.org` | only the test after a failed "add host", with the setting "Automatically detect blocked connections" |
| Discord Rich Presence | while you stream, only if the Discord app is installed | the Discord app on your device, which shares your activity with Discord | yes, in Settings. It is on by default |

## Questions

Open an issue at <https://github.com/vibesoftwarecoder/MoonlightVibe/issues>.
