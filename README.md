# XNET v0.4.5.5 [PUBLIC RELAY ADDED]

**End-to-end encrypted text, voice, file, and video communication for the original Xbox.**

<p align="center">
  <img src="_img/xnet-boot.png" width="800">
</p>

`v0.4.5.5 public beta` · Built with [NXDK](https://github.com/XboxDev/nxdk) · *Privacy by Design*

XNET turns a retail original Xbox into a secure communicator. Up to four
consoles anywhere in the world can chat, talk, send files, and see each other on
camera — all encrypted end-to-end, routed through a relay that can never read a
single byte of it.

> ✅ **Runs on a stock, unmodified 64 MB retail Xbox.**
> No RAM upgrade, no developer hardware, no special revision required. If your
> console runs homebrew, it runs XNET — camera video included.

---

## What's New in v0.4.5.5

This release hardens the encrypted transport and stabilizes the camera and video
paths.

- **Per-packet initialization vectors.** Every encrypted packet now uses a unique
  IV derived deterministically from its own authenticated header (session,
  stream, sender, and sequence number), so no two packets ever share an IV. This
  removes the IV-reuse weakness present in earlier builds.
- **Replay protection.** Each packet carries a sequence number, and receivers
  keep a sliding window per sender to detect and drop duplicated or replayed
  packets. A captured packet can no longer be re-injected into a live session.
- **Camera hard-lock fix.** Resolved an isochronous-scheduling issue that could
  freeze the console during camera use — reproduced most reliably with a camera
  connected and no Communicator present.
- **Video decoder stability.** The JPEG decoder is now strictly bounded.
  Malformed, truncated, or oversized camera frames are rejected cleanly instead
  of stalling or overrunning the decoder.
- **Public relay, zero configuration.** XNET now ships with a public relay
  pre-set in the bundled `xnet.cfg`, so a fresh install connects out of the box —
  nothing to host, no address to enter.

> 🌐 **Connect out of the box.** The release includes an `xnet.cfg` already
> pointed at a live public relay! flash, launch, and create or join a room. The
> relay is **zero-knowledge**: it blindly forwards encrypted packets and can't
> read anything, so sharing it costs you nothing, and your room token stays the
> only secret. Prefer your own relay? Change it in Settings any time.

> **Compatibility:** v0.4.5+ changed the wire format (sequence numbers + per-packet
> IVs) and does **not** interoperate with v0.4.1. Every console in a
> session must be on **v0.4.5+** ; flash all participants before joining.

---

## Downloads

| File | Download |
|------|----------|
| 💿 **XNET .iso** | [Download ISO v0.4.5.5](_deployments/XNET.iso) |
| 💿 **XNET .xiso** | [Download XISO v0.4.5.5](_deployments/XNET.xiso) |
| 📁 **XNET .zip** | [Download XBE v0.4.5.5](_deployments/XNET.zip) |

### SHA-256 (v0.4.5.5)

| File | SHA-256 |
|------|----------|
| `XNET.iso` | `f2474b7059769d95326f017db30f1c28149aeedcbf8ec025e79e4d278436b9a7` |
| `XNET.xiso` | `d7854b64388a49dfadd14f029fc5067a9377bd973b8a5e4b853a334cf195a7c4` |
| `XNET.zip` | `1044a0cf0e3ca9cc2bbf999ed5cc501e7aa12538894434aea979b9da3e44c3ce` |

## Features

- **Encrypted text chat** — type from an on-screen keyboard.
- **Real-time voice chat** — open-mic voice over an Xbox Communicator or Hawk adapter and headset.
- **Secure video chat** — live camera video from a PS2 EyeToy or Xbox camera, up to four tiles.
- **Encrypted file transfer** — send files console-to-console.
- **Authenticated encryption** — AES-128-CBC with HMAC-SHA-256 (encrypt-then-MAC, verified before decryption).
- **Per-packet IVs + replay protection** — a unique IV on every packet and a sliding-window sequence check that rejects replayed traffic.
- **Zero-knowledge relay** — the server blindly forwards encrypted packets and
  stores nothing.
- **Zero persistence** — rooms live only in memory and vanish when they end. No
  accounts, no friend lists, no cloud, no logs on the relay.
- **Built for the hardware** — fits and runs on a stock **64 MB** console at
  640×480.

---

## Requirements

### Console

- An original Xbox capable of running homebrew (softmod, modchip, or TSOP).
- **64 MB of RAM — i.e. a completely stock console.** A 128 MB debug/dev console
  works too, but is **not** required.
- A network connection (wired Ethernet; DHCP).

### Peripherals (optional, per feature)

| Feature        | Hardware                                                        |
| -------------- | --------------------------------------------------------------- |
| Text + files   | Controller only                                                 |
| Voice          | **Xbox Communicator** headset (045E:0283) — retail or Hawk; works with headsets like the Xbox 360 headset or a Logitech Pro X |
| Video          | **PS2 EyeToy** (silver US, 054C:0155) or the Japanese Xbox Video Camera (045E:028C) |

You can mix and match per console — a console with only a headset can still join
a video room and participate by voice; a console with only a camera will be seen
but silent.

### Relay

XNET routes traffic between consoles through a relay (consoles never connect
directly, so no one learns anyone else's IP). **The release ships with a public
relay already configured in `xnet.cfg`, so you don't need to set anything up to
get started.** You can point at a different relay or host your own any time — see
**Hosting a Relay** below.

---

## Installation

1. Build `default.xbe` (see **Building from Source**) or grab a release build.
2. FTP the XNET folder to your console (for example under
   `E:\Apps\XNET\`).
3. The release includes an `xnet.cfg` next to `default.xbe` (the `D:` launch
   directory at runtime), already pointed at a public relay — keep it as-is to
   connect immediately, or edit it to use your own relay. See **Configuration**.
4. Launch XNET from your dashboard.

On boot, XNET mounts the drives, brings up USB and networking, and drops you at
the main menu. A log is written to `E:\Dashboard\system\xnet.log`.

---

## Configuration

XNET reads `xnet.cfg` from its launch directory (`D:\xnet.cfg`) at boot. **The
release ships with this file already pointed at a public relay**, so it works
untouched — the example below just shows the format if you want to change
anything. All keys are optional (anything omitted falls back to a built-in
default), and everything here can also be changed in-app from the **Settings**
screen, which writes the file back for you.

```ini
relay=your.relay.host.or.ip   # bundled cfg ships with a public relay pre-set
port=7777
mic_gate=50000
mic_gain=100
debug_log=0
```

| Key         | Meaning                                                      |
| ----------- | ----------------------------------------------------------- |
| `relay`     | Relay hostname or IP (bundled cfg ships with a public relay) |
| `port`      | Relay port (default 7777)                                    |
| `mic_gate`  | Voice activation threshold; `0` = open mic (always transmit) |
| `mic_gain`  | Mic input gain in percent (25–200); lower it if a hot mic distorts |
| `debug_log` | `1` enables verbose per-frame logging for bug reports        |

---

## Usage

<p align="center">
  <img src="_img/xnet_mainmenu.png" width="800">
</p>

From the main menu:

- **CREATE ROOM** — generates a room token and waits for others to join.
- **JOIN ROOM** — type a room token to enter an existing room.
- **VIDEO CHAT** — start or join a Secure Video session (camera tiles + voice).
- **FILE TRANSFER** — send or receive a file with another console.
- **SETTINGS** — relay, mic, and diagnostics (below).
- **ABOUT** — version and project info.
- **QUIT** — return to the dashboard.

**The room token is the secret.** Anyone who has it can join the room and
decrypt everything in it, so share tokens only over a channel you already trust,
and prefer long, unpredictable ones.

<p align="center">
  <img src="_img/xnet-token.png" width="800">
</p>

### Voice

Voice is **open-mic** — just talk; there's no push-to-talk. In a video session
your own tile shows a live **MIC ON / TALKING / NO MIC** indicator so you can
confirm you're transmitting.

### Settings

- **RELAY IP / RELAY PORT** — edit the relay you connect to (saved to `xnet.cfg`).
- **MIC SENSITIVITY** — how loud you must be to transmit. A live meter shows your
  level against the gate; speak and watch the bar cross the line.
- **MIC GAIN** — input volume (25–200%). If the meter shows **CLIP** while you
  talk normally, lower the gain. (Great for hot headset mics like a Pro X.)
- **DEBUG LOGGING** — turn on a full diagnostic trace for bug reports, off for
  normal use.

---

## Building from Source

XNET is built with the [NXDK](https://github.com/XboxDev/nxdk) toolchain.

```sh
# install and set up NXDK first (see the NXDK docs), then:
export NXDK_DIR=/path/to/nxdk
cd _src
make
```

The build produces `bin/default.xbe`. The boot `build:` timestamp is regenerated
automatically on every build, so it always reflects the current binary. The
relay is a single Node.js file in `_src_relay/`.

---

## Hosting a Relay

The relay (`xnet-relay.js`) is a small Node.js server. It is a **blind
forwarder**: it moves encrypted packets between the consoles in a room and
stores nothing.

```sh
node xnet-relay.js        # listens on port 7777 by default
```

Run it on any always-on host (a small VPS is plenty), point your consoles'
`relay`/`port` at it, and you're done. See **Recommendations for Relay
Operators** in the security docs for hardening notes. Please don't add content
logging — the zero-knowledge design is the point.

---

## Security

XNET encrypts all text, voice, video, and file content end-to-end with
**AES-128-CBC** and authenticates every packet with **HMAC-SHA-256**
(encrypt-then-MAC, verified before decryption). As of **v0.4.5**, every packet
also carries a **unique per-packet IV** — derived deterministically from its
authenticated header, so IVs are never reused — and a **sequence number** checked
against a per-sender sliding window that rejects duplicated or replayed packets.
Session keys are derived **locally** from the room token and are never
transmitted; the relay never holds a key and cannot read traffic.

It is honest, hobbyist software for 24-year-old hardware — not an audited,
high-threat tool. Please read the full details and limitations:

- [**Security Policy**](SECURITY.md) — supported versions, threat model, and how
  to report a vulnerability.
- [**Security Architecture**](SECURITY_ARCHITECTURE.md) — key derivation, packet
  framing, per-packet IV generation, the encrypt-then-MAC construction, the
  replay window, per-feature data paths, and the relay design.

> **Compatibility note:** v0.4.5 changed the wire format (sequence numbers and
> per-packet IVs) and does **not** interoperate with v0.4.1 or v0.3.x. Every
> console in a session must be on **v0.4.5** — flash all participants.

---

## Troubleshooting

- **A log is always written to** `E:\Dashboard\system\xnet.log` (boot, device,
  and connection events). It truncates on every boot, so it stays small.
- For a detailed report, enable **DEBUG LOGGING** in Settings, reproduce the
  issue, then grab the log over FTP. Verbose logging is memory-intensive on
  console — turn it on only to capture a specific problem, then turn it back off.
- Can't connect on a fresh install? Make sure `xnet.cfg` is present next to
  `default.xbe` — the release ships with one pointed at the public relay. Without
  it, enter a relay in **Settings** before creating or joining a room.
- No video tiles? Confirm the EyeToy enumerates in the log (`camera: streaming
  started`). No audio? Confirm the Communicator enumerates (`xblc: mic/spk
  streaming started`).

---

## Known Bugs

- **xemu / disc builds:** in-app config changes may not persist across launches
  on `.iso` / `.xiso` builds. A fix exists but is not folded into this build yet.
- **Crash on boot:** there is a chance that if you boot XNET with an EyeToy connected
and no HAWK or Xbox Communcator in the controller, a driver compat will cause a hard crash.
---

## Source Availability and Documentation

- XNET is intentionally written with extensive inline documentation and descriptive code structure. Comments, protocol notes, and implementation details are included throughout the source to promote transparency, simplify auditing, and make the project easier for others to understand, maintain, and contribute to. This level of documentation is deliberate and reflects the project's commitment to openness and long-term preservation.

## Acknowledgments

XNET is built on [tiny-AES-c](https://github.com/kokke/tiny-AES-c),
[NXDK](https://github.com/XboxDev/nxdk), camera and USB research from the
original Xbox homebrew community, and the testers and contributors who keep the
original Xbox alive online.

Special thanks to **Team Resurgent** and **Darkone83** for their RXDK [camera research project](https://github.com/Darkone83/Xbox-live-camera-research-project)

Their work researching the original Xbox camera hardware and developing RXDK-based drivers for the OV519 chipset provided invaluable insight during the development of XNET's video subsystem. Their research greatly accelerated hardware bring-up, testing, and validation efforts.

Additional thanks to the contributors of the Xbox EyeToy project and [ConsoleMods Wiki](https://consolemods.org/wiki/Xbox:EyeToy_Mod_Guide)

- Ryzee119 — discovering and testing OV519 hardware registers and device descriptors, and identifying camera device IDs within the original Xbox Video Chat software.
- xbox7887 — research, testing, and documentation imagery.
- Harcroft — research, testing, EEPROM patching, Xbox camera teardown, and guide development.
- Libby — additional patching work.
- Luke Usher — the original idea.
- Evan Blax — English translation patch.

Their collective work preserving and documenting the original Xbox camera ecosystem made modern experimentation and compatibility efforts possible.

XNET would not exist without the collective knowledge shared by the original Xbox community over the past two decades.

---

## ☕ Support Development

If you enjoy this project and would like to help support continued development, consider buying me a coffee.

Your support helps fund:

- New features
- Bug fixes
- Original Xbox hardware for testing
- Server infrastructure
- Documentation and tooling
- Future open-source projects

<p align="center">
  <a href="https://buymeacoffee.com/tsardev">
    <img src="https://img.buymeacoffee.com/button-api/?text=Buy+Me+a+Coffee&emoji=☕&slug=tsardev&button_colour=40DCA5&font_colour=000000&font_family=Arial&outline_colour=000000&coffee_colour=ffffff" alt="Buy Me A Coffee">
  </a>
</p>

Thank you for supporting independent development!

*"Privacy by Design."*
