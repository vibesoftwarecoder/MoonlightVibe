# MoonlightVibe

**MoonlightVibe** is a Moonlight game streaming client. It is the companion client for [MultiSeat](https://github.com/vibesoftwarecoder/MultiSeat), which runs several streaming sessions on one Windows host, and it also works with any Moonlight-compatible host.

It is a fork of [moonlight-stream/moonlight-qt](https://github.com/moonlight-stream/moonlight-qt). This fork adds seat auto-discovery, microphone passthrough, USB forwarding and a second window. All four need [ApolloVibe](https://github.com/vibesoftwarecoder/ApolloVibe) on the host, except seat auto-discovery, which needs MultiSeat.

---

## Where to report a problem

**Anything about the client goes here**, including problems inherited from upstream Moonlight.
MoonlightVibe is what you installed, so it is ours to answer for.

| what is wrong | where it goes |
|---|---|
| The client: streaming, decoding, seat discovery, gamepad, microphone, USB forwarding, the second window | **here** |
| Picture, encoder or host audio | [ApolloVibe](https://github.com/vibesoftwarecoder/ApolloVibe/issues) |
| A seat will not provision | [MultiSeat](https://github.com/vibesoftwarecoder/MultiSeat/issues) |

Please do not take MoonlightVibe problems to the moonlight-stream Discord or issue tracker. They do
not maintain this fork, and cannot help with it.

## What's different from upstream Moonlight

| Feature | MoonlightVibe | Upstream Moonlight |
|---|---|---|
| Seat auto-discovery | ✅ MultiSeat seats appear in the computer list by themselves | ❌ Add each host by hand |
| Microphone passthrough | ✅ Your microphone is streamed to the host | ❌ Not available |
| Second window | ✅ A second picture from the same session, in a window of its own: one host window, or a whole display | ❌ Not available |
| USB forwarding | ✅ Share a USB device with the host (Windows, experimental, off by default) | ❌ Not available |
| Branding | MoonlightVibe | Moonlight |

Everything else is inherited from upstream Moonlight: hardware video decoding, H.264, HEVC and AV1, HDR, surround sound, gamepads and touch input.

---

## Download

Get the latest release from the [Releases page](https://github.com/vibesoftwarecoder/MoonlightVibe/releases/latest).

Extract the zip and run `MoonlightVibe.exe`. There is no installer, and it is fully portable.

---

## Second window

A session can show a second picture of the host in a second client window. It is the same connection and the same pairing, and nothing extra to run on the host. The second picture is one of two things:

- **A window on the host.** You name the window by its title, for example a browser or a chat. Only that window is shown, and it follows the window if it moves.
- **A whole display.** The host makes an extra virtual monitor. Drag windows onto it in the main window and they show up in the second one, so it works like a real second screen.

**To use it:** open **Settings → Second Window**, turn on **Show a second window**, and choose what to show. Then start a stream as usual. The options are:

- **What to show:** a window, or a whole display. For a window, type its title. A title that matches exactly is used first, then one that contains your text.
- **Resolution and frame rate** of the second picture: 720p to 4K, at 30 or 60 frames per second. A bigger picture is sharper when the window is large, and uses more bandwidth. It can't add detail the host window doesn't have.
- **Show it on this display:** which of your screens the second window opens on. Automatic picks a screen the main window is not on.
- **Open the second window in full screen:** on by default. Press **F11** in the second window to switch between full screen and a window.

Closing the second window stops only the second picture. The main window keeps streaming.

**Command line.** The same options work from the command line:

```
MoonlightVibe.exe stream <host> <app> --second-window "Window title"
MoonlightVibe.exe stream <host> <app> --second-window-source display
```

Also: `--second-window-resolution WxH`, `--second-window-fps N`, `--second-window-display N`, `--second-window-fullscreen` or `--no-second-window-fullscreen`, and `--window-display N` for the main window.

**What it needs:**

- An [ApolloVibe](https://github.com/vibesoftwarecoder/ApolloVibe) host with `second_video_stream = enabled`. A host without it ignores the request, and you get one window.
- **A whole display needs the console host**, the main desktop of the machine. A MultiSeat seat can only show a window.

**Known limits.** The second window follows the mouse by absolute position only, with no relative mouse mode inside it. If the host window is not 16:9, the picture has black bars.

---

## MultiSeat seat auto-discovery

When MultiSeat is running on the local machine, MoonlightVibe finds all active seats and lists each one as its own server in the computer list. You don't enter a host or a port. Seats appear within about 15 seconds of becoming ready.

This works over the LAN too. Point MoonlightVibe at the MultiSeat host's address, and it lists all the seats on that machine.

---

## Microphone passthrough

MoonlightVibe streams your microphone to the host, which plays it into `Speakers (Steam Streaming Microphone)`. Apps and games on the host should pick `Microphone (Steam Streaming Microphone)` as their input.

It needs [ApolloVibe](https://github.com/vibesoftwarecoder/ApolloVibe) on the host with `stream_mic = enabled`. MultiSeat turns that on for you.

---

## USB forwarding (experimental)

Windows only, and off by default. Open **Settings → USB forwarding** to share a USB device from your PC so it appears on the host you are streaming to. You can share or stop sharing a device, choose to forward it automatically when streaming, and press **Ctrl+Alt+Shift+U** to pause or resume. With the setting off, nothing is created and a stream behaves exactly as before.

It needs `usbipd-win` on your PC, and an ApolloVibe host with USB forwarding turned on, the `usbip-win2` driver, and the USB permission granted to your client. Sharing a device asks for administrator approval. Try a USB stick or a spare gamepad first, not a keyboard or mouse.

It builds on the work of qiin2333 and AlkaidLab. See the [6.3.11 release notes](docs/release-notes/6.3.11.md) for details.

---

## Sharing diagnostic logs

On Windows and macOS, open **Settings → Advanced Settings** and turn on **Enable detailed logging** before reproducing a problem. Then select **Open Log Folder** and attach the newest `Moonlight-*.log` file to your issue. Warning and error messages are logged even when detailed logging is off. The setting stays on until you turn it off.

Please read a log before sharing it. It may include your device or host names, network addresses, and details of your streaming session. Session encryption keys are removed automatically.

---

## Code signing policy

Free code signing provided by [SignPath.io](https://about.signpath.io), certificate by
[SignPath Foundation](https://signpath.org).

- **What is signed:** the two files built from this repository, `MoonlightVibe.exe` and
  `AntiHooking.dll`, in the Windows release. They show "SignPath Foundation" as the publisher.
- **What is not:** third-party libraries bundled from other open-source projects, such as FFmpeg,
  SDL and OpenSSL, are included as those projects or their packagers provide them. Qt's libraries
  carry The Qt Company's own signature.
- **How:** every Windows release is built from this repository by GitHub Actions on GitHub-hosted
  runners, and each signing request is approved by hand before it is signed.

Team roles:

- Committers and reviewers: [vibesoftwarecoder](https://github.com/vibesoftwarecoder)
- Approvers: [vibesoftwarecoder](https://github.com/vibesoftwarecoder)

Privacy: see [PRIVACY.md](PRIVACY.md). MoonlightVibe sends no data to us. It does contact some
servers run by others, such as GitHub for its update check, and that page lists each one.

---

## Requirements

- Windows 10 or 11, x64.
- For the extra features, [ApolloVibe](https://github.com/vibesoftwarecoder/ApolloVibe) on the host. Seat auto-discovery also needs [MultiSeat](https://github.com/vibesoftwarecoder/MultiSeat).
- A standard Moonlight-compatible host, such as upstream Sunshine or Apollo, works for ordinary streaming. It does not offer the microphone, the second window or USB forwarding.

## Using standard Moonlight hosts

MoonlightVibe is fully compatible with Sunshine, upstream Apollo and NVIDIA GameStream. Seat auto-discovery only turns on when a MultiSeat service is found. The microphone, the second window and USB forwarding each need an ApolloVibe host with that feature enabled, and are ignored by other hosts.

---

## Building

MoonlightVibe builds the same way as upstream moonlight-qt: Qt 6.7 or newer and Visual Studio 2022 on Windows.

See the upstream [build instructions](https://github.com/moonlight-stream/moonlight-qt#building) for the full dependency list.

---

## License

MoonlightVibe is licensed under the same terms as Moonlight — [GPL-3.0](LICENSE).

Microphone passthrough patches by [logabell](https://github.com/logabell/moonlight-qt-mic).
