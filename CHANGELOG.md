# Changelog

All notable changes to XNET are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/), and the project uses informal
versioning during early development.

---

## [0.4.1] - 2026-06-20 - Voice Decoupling

### Changed
- Voice servicing is no longer tied to the render and video decode cadence. A
  new `pump_voice()` helper services the audio paths independently while video
  decoding continues.
- Audio is now serviced multiple times per frame: before the main loop switch,
  immediately after rendering and presentation, and between each per-peer video
  decode. This keeps long decode bursts from starving audio.

### Fixed
- Reduced voice packet drops during video calls. Heavy decode and presentation
  previously created service gaps longer than the 20 ms voice frame interval,
  which caused drops, bursty audio, and degraded voice under load.
- In multi-participant sessions, large decode bursts from three or four peers no
  longer monopolize the frame loop; speaker buffers and mic transmission stay
  active throughout the decode cycle.

### Notes
- The single-threaded cooperative design is preserved: no new threads, locks,
  synchronization primitives, or interrupt-context callbacks.
- `stash_frame()` remains main-loop only and video callbacks run only after a
  frame completes, so no IRQ re-entrancy is introduced.
- `pump_voice()` does minimal work and returns immediately when no audio is
  pending. No measurable CPU overhead was added.

---

## [0.4.0] - 2026-06-20 - Authenticated Encryption

### Security
- XNET now uses authenticated encryption with an Encrypt-then-MAC construction.
  AES-128-CBC continues to provide confidentiality; HMAC-SHA-256 is computed
  over `(IV || ciphertext)`.
- MAC verification runs before decryption. Invalid packets are discarded without
  affecting session stability.
- Constant-time comparison prevents timing leaks, and MAC keys are derived
  independently from encryption keys using domain separation.
- Voice, video, file metadata, and file chunks are all authenticated. Corrupted
  or modified packets are silently discarded.

### Changed
- Frame buffers were resized to accommodate the 32-byte MAC. Maximum
  authenticated frame size remains within relay limits.
- Security documentation updated to describe authenticated encryption,
  verify-before-decrypt behavior, constant-time comparison, and domain-separated
  MAC keys.

### Compatibility
- This is a breaking wire-format change. 0.3.x clients are incompatible with
  0.4.0, and older packets are safely rejected by MAC verification. All consoles
  in a room must run 0.4.0 or later.
- The relay required no changes. It remains a blind forwarder with no
  server-side cryptography.

### Notes
- Not yet addressed and still on the roadmap: replay protection, forward
  secrecy, per-user cryptographic identity, and cryptographically secure IV
  generation.

---

## [0.3.6] - 2026-06-20 - Microphone Gain

### Added
- Software microphone gain stage applied before ADPCM encoding. A new MIC GAIN
  setting sits in Settings below MIC SENSITIVITY, adjustable from 25% to 200%
  (default 100%), saved per console in `xnet.cfg` via `mic_gain=`.
- Microphone peak monitoring and live clip detection on the level meters. The
  meter turns red and shows "CLIP! mic too hot - lower gain" when input
  approaches full scale, on both the monitor and transmit paths.

### Changed
- Hot microphones (for example a Logitech Pro X on a Hawk Communicator adapter)
  can now be attenuated before ADPCM encoding, reducing encoder distortion. Each
  console keeps its own gain, so mixed headset types can share a room.

### Notes
- Clip detection helps distinguish encoder saturation from upstream ADC
  clipping: if clipping shows, lower MIC GAIN until levels return to normal. If
  it persists at low gain, the distortion is in the headset hardware.

---

## [0.3.5] - 2026-06-19 - Runtime Debug Logging

### Added
- DEBUG LOGGING toggle in Settings. Debug logging is now runtime-configurable
  rather than compile-time, persisted via `debug_log=0/1` in `xnet.cfg`, and
  survives reboots. Release builds ship with it off.
- Adaptive heartbeat cadence: every 60 seconds with debug off, every 5 seconds
  with debug on, updating immediately when the toggle changes. Loop counters keep
  incrementing regardless of interval.

### Changed
- Renamed `tsar.log` to `xnet.log`.
- High-frequency diagnostics (camera ISO stats, video transmit/receive logging,
  voice transmit/receive/drop counters) are now gated behind DEBUG LOGGING. This
  lets testers capture full traces without a special build.

### Notes
- Always-logged events regardless of debug mode: boot and initialization, device
  and camera bring-up, relay connect/disconnect, configuration loads and saves,
  camera reset and recovery, errors, and heartbeat memory monitoring.

---

## [0.3.4] - 2026-06-19 - Stability and Logging Pass

### Changed
- Gated high-frequency frame-level logging behind `XNET_VERBOSE_LOG`, off by
  default. Heartbeat logging is retained for memory and stability monitoring.

### Fixed
- Verified extended sessions: 20+ minute video and voice calls across multiple
  consoles with no freezes or crashes. Settings, About, Camera Test, and File
  Transfer were exercised during testing.
- Confirmed bidirectional voice during video sessions across multiple consoles.

### Notes
- Memory was investigated and found bounded: usage scales with session activity
  and then plateaus, with no continuous runaway leak. Application-level checks
  confirmed camera ISO buffers are allocated once and reused, receive buffers are
  static, packet queues are fully drained, and connection code frees address
  information.
- Voice remains stable, with some jitter-buffer drops during bursty arrival and
  higher drop rates on host systems under load. Further buffer tuning may improve
  smoothness.
- Testers: MORPH10U5, Geni, TealC.

---

## [0.3.3] - 2026-06-19 - Settings and About Screens

### Added
- Dedicated Settings screen: Relay IP (keyboard pre-filled, small font for long
  addresses), Relay Port (numeric, validated 1 to 65535), Mic Sensitivity
  (adjusted live with the D-pad), Camera Test (the former debug-camera screen,
  returns to Settings on B), and Back.
- Scrollable About screen showing features, privacy-by-design information,
  architecture overview, credits, and project philosophy, with accent-colored
  headings and D-pad scrolling.

### Changed
- Reordered the main menu: CREATE ROOM, JOIN ROOM, VIDEO CHAT, FILE TRANSFER,
  SETTINGS, ABOUT, QUIT. Renamed Secure Video to Video Chat and Secure Transfer
  to File Transfer. Moved Debug Camera into Settings as Camera Test.
- Renamed Mic Threshold to Mic Sensitivity and replaced raw threshold values
  with named modes (OPEN MIC, HIGH, MEDIUM, LOW, VERY LOW). Added a live
  microphone meter with a gate marker that turns green when voice crosses the
  threshold.
- Relay IP, relay port, and mic threshold now persist to `D:\xnet.cfg`, saved on
  confirm and when leaving Settings.

### Fixed
- Main menu relay display now refreshes immediately after editing, via a shared
  `refresh_relay_info()` helper.
- Relay IP and port editing reuse the keyboard with explicit edit-target
  tracking, preventing stale keyboard state from affecting room joins, video
  joins, or file-transfer receive.

---

## [0.3.2] - 2026-06-18 - Video Session Fixes

### Fixed
- Restored two-way audio in video sessions. `xnet_audio_tick()` previously only
  transmitted while in `SCREEN_CHAT`; `SCREEN_VIDEO` now transmits and shows talk
  activity, with `g_talking` updating correctly.
- Fixed active video calls timing out after roughly 10 to 14 minutes. A
  throttled `touchIdle()` now refreshes the relay idle timer during voice and
  video traffic (at most once every 30 seconds), so rooms with no text traffic
  are not destroyed.
- Corrected EyeToy color reproduction with a configurable `XNET_CHROMA_SWAP` in
  picojpeg that handles Cb/Cr inversion across all scan-type paths. Added JPEG
  format diagnostics. The framebuffer path was confirmed good; the issue was in
  the decode stage.
- Fixed the EyeToy staying active when quitting to the dashboard.
  `xnet_camera_shutdown()` now runs before `XReboot()`, turning off the LED and
  holding the FIFO.

### Notes
- Memory confirmed stable: heartbeat logs show about 59.6 MB free throughout
  extended sessions, with only normal allocator noise.
- A 14-minute session completed with zero freezes; the earlier video rate caps
  continue to prevent the sudden system wedge.

---

## [0.3.1] - 2026-06-18 - Runtime Visibility and Load Management

### Added
- Live RAM meter in the header bar on all screens, including video, showing used
  and total memory in real time.

### Changed
- Leaving a video session now properly idles the EyeToy: LED off, OV519 FIFO held
  in reset, streaming stopped. Re-entering a session releases reset, re-enables
  the LED, and restores streaming quickly.
- Reduced video transmit to roughly 10 FPS and decode cadence from 15 FPS to
  10 FPS, lowering sustained CPU load on both sides.

### Fixed
- The camera no longer remains active after leaving a video session, removing
  unnecessary USB traffic.
- A sender can no longer overwhelm a slower receiver by flooding it with frames.

### Notes
- Long sessions showed asymmetric decode counts between peers (for example ~511
  local vs ~721 remote), indicating the receiver does more work and is more
  likely to freeze first. The receive side is the most expensive part of the
  pipeline (decode, decryption, networking, rendering). 10 FPS was chosen because
  webcam-sized tiles look nearly identical to 15 FPS at much lower load.
- A pipeline bounds audit found no obvious buffer overruns. Heartbeat
  diagnostics remain on; if freezes persist despite reduced load, the next
  suspect is an OHCI/USB wedge from three concurrent isochronous streams (camera,
  Communicator mic, Communicator speaker).

---

## [0.3.0] - 2026-06-18 - 64 MB Memory Optimization

The video subsystem was optimized for stock 64 MB consoles. Large static
allocations were consuming low memory below the framebuffer, leaving too little
headroom for networking, audio, and USB, which caused crashes at video startup
on 64 MB hardware.

### Changed
- Reduced decoded video storage from 320x240 to 160x120 ARGB per slot. Full
  320x240 JPEG decode is preserved; frames are subsampled 2:1 before storage
  with no noticeable impact at tile size.
- Resized camera frame buffers to match actual OV519 JPEG output rather than raw
  YUV worst-case assumptions.
- Reduced active isochronous transfer descriptors from four to three to ease
  pressure on the USB DMA pool.

### Memory Savings

| Component                      | Before |    After |
| ------------------------------ | -----: | -------: |
| Per-slot decoded pixel buffers | 1.2 MB |   300 KB |
| Camera frame buffers           | 474 KB |    96 KB |
| Total static reduction         |        | ~1.25 MB |

### Notes
- The framebuffer sits at the top of detected RAM, so all other allocations live
  below it. On 64 MB systems the oversized buffers starved lwIP packet buffers,
  USB descriptors, audio DMA, and the runtime heap. 128 MB systems tolerated the
  waste; 64 MB systems crashed at video startup. This is the first reliable 64 MB
  video build and a major stabilization milestone.

---

## [0.2.9.1] - 2026-06-18 - Receive Pipeline Decoupling

### Fixed
- Removed inline JPEG decode from the packet receive path. Incoming video packets
  now store only the latest JPEG per slot; older frames are discarded in favor of
  the newest. Decode moved into the render loop and throttled to about 15 fps.

### Notes
- Transport, encryption, relay forwarding, and delivery were all correct. The
  bottleneck was receive-side starvation from decoding each JPEG inline, which
  left tiles stuck on CONNECTING and froze video despite frames arriving. Treating
  incoming frames as disposable keeps the display on the freshest image. Receive,
  decode, and render are now fully decoupled, completing the video feature stack.

---

## [0.2.9.0] - 2026-06-18 - Peer Synchronization

### Fixed
- Room roster synchronization. New participants now receive `PKT_PEER_JOINED` for
  every existing occupant on join, fixing one-directional sessions where frames
  arrived and decoded but the tile never activated because `peers_online[]` was
  never populated.

### Notes
- The issue was entirely relay-side; the client already handled
  `PKT_PEER_JOINED` correctly. Join order no longer matters, and video activates
  in both directions over the same blind relay path used by text, voice, and file
  transfer. (This also supersedes the earlier 0.2.8.5 roster fix.)

---

## [0.2.8.8] - 2026-06-18 - Video Session Initialization

### Fixed
- Video sessions now call `xnet_camera_init()` unconditionally on entry.
  Previously, camera streaming from boot caused initialization to be skipped,
  leaving frame assembly disabled. `g_want_frames` is now always enabled before
  frame consumption begins.

### Notes
- Camera hardware and isochronous transfers were fine; packet data was discarded
  before assembly, producing `bytes=0`, no SOF/EOF markers, no self-preview, and
  sessions stuck on CONNECTING. DEBUG CAMERA masked the bug because it always
  initialized the camera. Video now follows the same path. Existing re-kick and
  self-healing safeguards remain.

---

## [0.2.8.4] - 2026-06-18 - Live Video Stabilization

The video pipeline reached its first truly usable state, with continuous
real-time video on original Xbox hardware using a modified PS2 EyeToy.

### Added
- Relaxed frame publication to match Darkone83's proven approach: publish-all
  with lightweight size validation, double-buffering, and an expanded UTR queue.

### Fixed
- Removed over-aggressive JPEG end-marker validation that rejected nearly all
  valid OV519 frames. Restored smooth updates and correct SOF/EOF counters, and
  eliminated the slideshow behavior caused by dropping good frames.

### Notes
- Typical frame size ~2.4 to 3.8 KB baseline JPEG, about 12 FPS at 300 to
  400 kbps per participant. Test video came from a modified silver PS2 EyeToy.
  Built on OV519 research by Darkone83, Ryzee119, xbox7887, Harcroft, Libby, Luke
  Usher, and Evan Blax.

---

## [0.2.8.3] - 2026-06-18 - Stability Pass

### Added
- Double-buffered frame pipeline, isochronous ring expanded from 2 to 4 UTRs,
  reduced self-preview decode cadence, and frame sanity checks before decode.

### Fixed
- Eliminated frame tearing from IRQ and decoder contention, and corruption from
  buffers overwritten mid-decode. Improved tolerance to delayed transfer
  resubmission.

### Notes
- Local preview became much more stable. Remaining corruption appeared to come
  from transport overhead rather than decode, and long runs still showed
  occasional freezes.

---

## [0.2.8.2] - 2026-06-17 - Camera Diagnostics

### Added
- DEBUG CAMERA menu option with live counters (ISO IRQs, bytes, SOF, EOF,
  completed frames, camera state), a local preview independent of relay traffic,
  and readable status messages (NO PIXELS; DATA, NO FRAME MARKERS; LIVE).

### Fixed
- Removed network variables from camera testing and improved startup logging.

### Notes
- Confirmed transport and relay were not responsible for video failures, and
  allowed rapid hardware iteration without a second console.

---

## [0.2.8.1] - 2026-06-17 - EyeToy Bring-Up

First successful hardware integration of the PS2 EyeToy and Xbox Live Video
Camera. The generic camera HAL was replaced with a real OV519 backend built on
Darkone83's research and libusbohci isochronous transfers.

### Added
- OV519 camera backend (`xnet_camera_nxdk.c`) implementing the `xnet_camera.h`
  interface, with support for Sony EyeToy cameras (silver and black) and the
  Japan-only Xbox Live Video Camera.
- Automatic VID/PID detection and endpoint selection, OV7620 and 76xx sensor
  init paths, isochronous streaming over libusbohci, and JPEG frame assembly
  using OV519 SOF (`FF FF FF 50`) and EOF (`FF FF FF 51`) markers.

### Fixed
- Corrected sensor initialization ordering to match Darkone83's documented
  sequence, resolving all-zero isochronous streams caused by incorrect setup.

### Notes
- First live camera feed on retail hardware. Artifacts and tearing still present;
  the network path was untouched.

---

## [0.2.8] - 2026-06-17 - Secure Video

Encrypted camera chat: a 2x2 tiled video session over the same relay path as
text, voice, and file transfer. Built on Darkone83's OV519 research, with the
capture driver behind a clean interface so the EyeToy bring-up can be dropped in
without touching the rest of XNET.

### Added
- Secure Video on the main menu with START/JOIN, the same token/room model as
  chat (up to 4 participants).
- Video transport with two packet types (`PKT_VIDEO 0x18`, `PKT_VIDEO_RELAY
  0x19`), blind-relayed like voice. The OV519's on-chip JPEG engine yields ~2.4
  to 3.8 KB frames, so one frame fits in a single AES packet with no chunking.
  About 12 fps, 300 to 400 kbps per sender.
- Decode and display (`xnet_video.c`): picojpeg decodes each peer's frame into a
  per-slot ARGB buffer, blitted into a 2x2 grid with per-slot borders and labels.
- Camera HAL (`xnet_camera.h`): a four-call capture contract
  (`init`/`streaming`/`get_frame`/`shutdown`) isolating XNET from the USB driver.
- Stub camera backend (`xnet_camera_stub.c`) emitting a 320x240 test pattern so
  the full encrypt, relay, decode, and tile pipeline can be proven before the
  real driver exists.
- Vendored picojpeg (public domain) for software JPEG decode. Relay advertises
  `secure-video: 0x18-0x19` in its startup line.

### Notes
- This was the stub-camera build; real EyeToy capture was the remaining piece.
  Builds entirely under NXDK, with Darkone83's RXDK project as driver reference
  only.

---

## [0.2.7] - 2026-06-16 - Browser and Input Polish

### Added
- X is now a quick backspace on both on-screen keyboards, in addition to the
  on-screen `<` key.

### Changed
- File browser entry cap raised from 256 to 1024; large directories were silently
  truncated past the 256th entry.
- Moved the dirs-first sort scratch buffer off the stack into static memory
  (~74 KB at the new cap), which is what makes the larger cap safe.

### Fixed
- Missing files in directories, caused by the old cap. Added per-directory
  enumeration logging to tell a too-small cap apart from the NXDK
  `FindFirstFile` shim quitting early.

---

## [0.2.6] - 2026-06-15 - First-Hardware Fixes

### Fixed
- Room token no longer lingers in the chat input; it is cleared on both join
  transitions.
- Spurious boot network error resolved. On slow DHCP, `nxNetInit` could return
  before the lease landed and fire a hard error. Boot is now non-fatal: slow or
  failed DHCP logs a line and proceeds to the menu, while a real outage still
  surfaces at connect time as CANNOT CONNECT TO RELAY.
- Transfers that reached the relay but timed out at the receiver (E06). If every
  chunk was acked via `PROG` but the final `DONE` raced or dropped, the transfer
  now counts as success. Added per-event logging on both ends, and the relay
  startup line advertises `secure-transfer: enabled`.
- Build error from `XFILES_VISIBLE_ROWS` being referenced in `xnet_ui.c` but
  defined in `xnet_files.c`; moved it into `xnet_files.h`.

---

## [0.2.5] - 2026-06-15 - Secure Transfer

Encrypted one-to-one file transfer, console to console, over the same relay path
as text and voice.

### Added
- Secure Transfer on the main menu with a SEND/RECEIVE chooser. First entry
  creates `E:\XNET FILES`, where received files land.
- On-console file browser (`xnet_files.c`/`.h`): drive-letter roots with
  on-demand partition mounting, directory walk via the Win32 `FindFirstFile`
  family, directories sorted first. Right Trigger opens the browser.
- Chunked transfer protocol: eight packet types (`0x10` to `0x17`), 4 KB chunks,
  each carrying its index and AES-128-CBC encrypted with a fresh IV,
  blind-relayed like text and voice.
- Transfer UI with dual progress bars, a receiver wait screen, and a SUCCESS or
  FAILED result screen with error codes E01 to E09.
- Bulk crypto helpers (`xnet_crypto_encrypt_block`/`_decrypt_block`):
  IV-prefixed, PKCS7-padded, without the 128-byte cap of the message path.
- Relay blind-forward cases for the file packet types, with idle timer reset on
  transfer traffic.

### Fixed
- Hardened `xnet_net_send_pkt`. A non-blocking `EWOULDBLOCK` mid-frame previously
  returned -1 and corrupted the stream; it now `select()`s for writability and
  retries with a bounded stall cap.

### Notes
- Extension-agnostic, one file per transfer. Maximum size ~4 GB, bounded by the
  32-bit size field and FATX; the practical ceiling is the receiver's free space
  on E:. Per-chunk activity resets the timeout, so a large transfer fails only on
  an actual stall.

---

## [0.2.4] - 2026-06-13 - Voice on Hardware

Encrypted voice chat working console to console over the internet, verified on
real hardware across the country.

### Added
- Xbox Live Communicator voice support. Custom USB host driver (`xnet_xblc.c`)
  for the vendor-class `0x78` Communicator on NXDK's libusbohci stack, running
  both isochronous streams at the native 16 kHz.
- Voice engine (`xnet_audio.c`): mic capture, 16 to 8 kHz decimation, RMS noise
  gate with hang time, IMA ADPCM 4:1, AES, then network, and the reverse for
  playback with per-speaker jitter queues and a saturating mixer for up to 4
  participants.
- IMA ADPCM codec (`xnet_voice.c`): 8 kHz, 20 ms self-contained 86-byte frames,
  verified bit-exact across C and Python.
- Headset status indicators on the main menu and chat screen, and the `mic_gate`
  config key (0 = always-on open mic).

### Fixed
- Black screen on USB init. `usbh_core_init()` memsets the driver table, so
  registering the XBLC driver before SDL's core-init wiped it and the
  Communicator was never probed. Fixed by calling `usbh_core_init()` first, then
  registering; SDL's later call becomes a no-op.
- Driver now reads endpoint addresses from interface descriptors instead of
  hardcoding them. Real hardware presented `0x04`/`0x85` rather than the
  `0x02`/`0x81` in reference firmware (appears controller-slot dependent), and
  the descriptor-driven approach adapts automatically.

### Notes
- Voice runs at 8 kHz on the wire (~46 kbps per active speaker), WAN-friendly.
  (Version 0.2.3 was the immediate pre-verification cut of this same work.)

---

## [0.2.0] - 2026-06-12 - Voice Pipeline (Pre-Hardware)

### Added
- Voice codec and framing, unit-tested on host (round-trip SNR, packet-loss
  independence, malformed-frame rejection), with a Python mirror proven bit-exact
  against the C version.
- Voice packets (`PKT_VOICE`/`PKT_VOICE_RELAY`) flowing end to end through the
  live relay, which blind-forwards them with no server changes.

---

## [0.1.5] - 2026-06-12 - LAN and Self-Hosting

### Added
- Runtime relay config: relay host and port read from `D:\xnet.cfg` at boot
  instead of compiled in. The main menu shows the configured target.
- A PC test client for text testing without a second console.

### Changed
- Relay packaged as a hardened systemd service (zero-log, restart-on-fail,
  survives reboots).

---

## [0.1.0] - 2026-06-11 - Encrypted Text Chat

First working release: encrypted text chat between two consoles over the
internet.

### Added
- AES-128-CBC encrypted text chat, up to 4 participants per room.
- Token-based rooms (key derived from `SHA-256(token)`; the relay never holds the
  key).
- Zero-knowledge Node.js relay (`xnet-relay.js`): create/join, blind forward,
  ping/pong keepalive, room teardown.
- Framebuffer UI (main menu, create/join, chat, error screen), networking over
  lwIP, and controller input via SDL.
- On-disk logging flushed per line so it survives a hang or crash, covering boot
  progress and USB enumeration.

### Fixed
- Black screen at boot, from three stacked issues: pbkit never presented
  CPU-drawn frames (switched to the `XVideoGetFB()` software framebuffer path),
  the first frame dereferenced a NULL framebuffer pointer (replaced with a static
  back buffer), and DHCP blocked before anything drew while a network failure
  rebooted the console (now draws a splash first and shows an error screen instead
  of rebooting).

---

## Roadmap

- Replay protection, forward secrecy, ephemeral session keys, per-user identity,
  and cryptographically secure IV generation.
- Map Communicator USB endpoint numbering across both controller slots.
- Add multiple custom drivers for USB 1.1 webcams.

---

*Developed by Tsardev / Dead Orbit Studios.*
