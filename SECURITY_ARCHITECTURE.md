# Security Architecture

**XNET**

This document describes the complete security architecture of XNET; from key derivation and packet framing to authenticated encryption, relay design, and known limitations.

XNET is an end-to-end encrypted communication platform for the original Xbox supporting text, voice, file transfer, and video communication. XNET is designed around a zero-knowledge relay architecture in which only room participants possess the encryption key.

> ⚠️ **Beta Software Notice**
>
> XNET is software built for 24-year-old retail hardware. It has not undergone independent security review or formal audit. Read the **Threat Model and Limitations** section before relying on XNET for anything sensitive.

---

# Table of Contents

1. [Threat Model](#threat-model)
2. [Protocol Overview](#protocol-overview)
3. [Packet Framing](#packet-framing)
4. [Session Key Derivation](#session-key-derivation)
5. [AES Encryption](#aes-encryption)
6. [Initialization Vectors](#initialization-vectors)
7. [Authenticated Encryption](#authenticated-encryption)
8. [Packet Types and Flow](#packet-types-and-flow)
9. [Voice Architecture](#voice-architecture)
10. [Video Architecture](#video-architecture)
11. [File Transfer Architecture](#file-transfer-architecture)
12. [Relay Architecture](#relay-architecture)
13. [Metadata Exposure](#metadata-exposure)
14. [Local Logging](#local-logging)
15. [Known Limitations](#known-limitations)
16. [Anti-Replay Architecture](#anti-replay-v045)
17. [Recommendations for Relay Operators](#recommendations-for-relay-operators)
18. [Future Improvements](#future-improvements)

---

# Threat Model

XNET is designed to provide private communication between trusted participants sharing a room token.

## Security Goals

- Confidentiality
- Integrity
- Tamper detection
- Zero-knowledge relay operation
- No peer IP exposure

## Out of Scope

XNET is not intended to defend against:

- Nation-state adversaries
- Physical access to consoles
- Users sharing room tokens with untrusted parties
- Traffic analysis
- Compromised endpoints

---

# Protocol Overview

All communication occurs through a relay server.

The relay forwards packets but never possesses session keys.

```text
Xbox A                        Relay                         Xbox B
  |                             |                             |
  |---- Create Room ----------->|                             |
  |<--- Token ------------------|                             |
  |                             |<---- Join Room ------------ |
  |                             |                             |
  |==== Session key derived locally from room token ==========|
  |                             |                             |
  |--- Encrypt + HMAC --------->|--- Blind Forward ---------> |
  |                             |                             |
  |<---------------- Encrypted Packets -----------------------|
```

Only participants possessing the room token can decrypt traffic.

---

# Packet Framing

Every packet begins with a 4-byte frame envelope:

```text
[1 byte]   packet type
[1 byte]   slot
[2 bytes]  payload length
```

Encrypted payload packets (`TEXT`, `VOICE`, `VIDEO`, `FILE_META`, `FILE_DATA`,
and their `_RELAY` echoes) then carry:

```text
[8 bytes]  sequence number (big-endian)
[16 bytes] IV
[N bytes]  AES-CBC ciphertext
[32 bytes] HMAC-SHA-256 tag
```

The authentication tag covers the associated data followed by the IV and
ciphertext:

```text
AAD = session_id(16) || stream(1) || sender_slot(1) || seq(8)
tag = HMAC-SHA256(mac_key, AAD || IV || ciphertext)
```

The `seq` prefix is transmitted; `session_id` is authenticated but **not**
transmitted in the frame — the receiver supplies it locally. Together they
underpin replay protection; see [Anti-Replay](#anti-replay-architecture) for the
full construction.

Invalid packets are silently discarded.

---

# Session Key Derivation

Session keys are derived locally:

```text
AES key = SHA256(room_token)[0:16]
```

The room token itself is never transmitted.

All consoles independently derive identical keys.

The relay never receives:

- Room token
- Session key
- Plaintext content

The room token acts as the shared secret.

---

# AES Encryption

XNET uses:

- AES-128
- CBC mode
- PKCS#7 padding
- tiny-AES-c

Every packet carries a 16-byte initialization vector, derived synthetically per
message (see [Initialization Vectors](#initialization-vectors)) rather than drawn
from a random pool.

Encryption protects:

- Text
- Voice
- Video
- File metadata
- File transfer chunks

---

# Initialization Vectors

CBC requires a fresh, unpredictable IV per message. Earlier builds filled the IV
from a small `KeQueryPerformanceCounter()`-seeded xorshift. That output is
**predictable** — an observer who can correlate frame timing can narrow the
counter — and a predictable IV under CBC is a chosen-plaintext (BEAST-style)
hazard. It also offered no guarantee against reuse.

XNET now derives the IV **synthetically**, as a keyed function of the message's
associated data:

```text
K_iv = SHA256(aes_key || "XNET-IV1")          (IV subkey, domain-separated)
AAD  = session_id(16) || stream(1) || sender_slot(1) || seq(8)   (26 bytes)
IV   = HMAC-SHA256(K_iv, AAD)[0:16]
```

`AAD` is the same associated data the anti-replay layer already builds and
authenticates, so the IV reuses it at no extra wire cost. `K_iv` is
domain-separated from the AES key by the `"XNET-IV1"` label, exactly as the MAC
key is separated by `"XNET-MAC1"`; the three keys are independent.

This yields two properties:

- **Unpredictable.** Computing the IV requires `K_iv`, which is derived from the
  secret session key. An attacker without the room token cannot derive `K_iv` and
  cannot predict the IV, regardless of timing.
- **Non-repeating.** `AAD` is unique per message (`seq` is a 64-bit monotonic send
  counter), so the HMAC input never repeats under one `K_iv`, so the IV never
  repeats.

The IV is still transmitted in the clear and is still covered by the packet MAC;
the receiver does **not** recompute it. The change is contained entirely to the
crypto layer — no wire-format, receiver, or relay change.

> **Load-bearing detail.** Because the session key is the static `SHA256(token)`,
> `K_iv` is the same every time a given token is used. IV uniqueness therefore
> rests on the `AAD`, and specifically on `session_id`: the relay mints 16 random
> bytes per room (see [Anti-Replay](#anti-replay-architecture)), so two rooms
> opened with the same token produce different IVs for the same stream/slot/seq.
> Keep `session_id` in the `AAD` — it is what keeps the synthetic IV safe under a
> static, token-derived key, not merely a replay defense.

---

# Authenticated Encryption

Beginning with v0.4.0, XNET uses an Encrypt-then-MAC construction; since v0.4.5
the tag also binds the replay-protection associated data.

```text
AAD = session_id(16) || stream(1) || sender_slot(1) || seq(8)
tag = HMAC-SHA256(mac_key, AAD || IV || ciphertext)

encrypted payload =
    seq
    IV
    ciphertext
    tag
```

MAC keys are domain-separated from encryption keys.

Receivers perform:

1. HMAC verification.
2. Constant-time comparison.
3. Verify-before-decrypt.
4. Packet discard on failure.

This protects against:

- Ciphertext modification
- Packet forgery
- Active tampering

Corrupted packets behave identically to packet loss and do not destabilize sessions.

---

# Packet Types and Flow

The following packet classes are protected:

```text
TEXT
VOICE
VIDEO
FILE_META
FILE_CHUNK
PING
PONG
JOIN
LEAVE
END
```

All payload packets are encrypted and authenticated.

The relay performs no cryptographic operations.

---

# Voice Architecture

Voice path:

```text
Microphone
    ↓
Mic Gain Stage
    ↓
Noise Gate
    ↓
IMA ADPCM Encoder
    ↓
AES-CBC
    ↓
HMAC-SHA256
    ↓
Relay
    ↓
Verify
    ↓
Decrypt
    ↓
Jitter Buffer
    ↓
Mixer
    ↓
Speaker
```

Features:

- Open microphone
- Configurable gate
- Per-console gain control
- Packet jitter buffering
- Multi-speaker mixing

Audio mixing occurs entirely on clients.

The relay performs no audio processing.

> Note: input gain is applied before the noise gate, so the gate decision is
> made on the gained signal. This is why raising the gain can also push a quiet
> source past the gate.

---

# Video Architecture

```text
PS2 EyeToy / Xbox Camera (Xbox ビデオチャット キット)
     ↓
OV519 Driver
     ↓
ISO Buffers
     ↓
JPEG Frame
     ↓
AES-CBC
     ↓
HMAC-SHA256
     ↓
Relay
     ↓
Verify
     ↓
Decrypt
     ↓
JPEG Decode (picojpeg)
     ↓
Framebuffer Blit (software)
```

XNET renders video entirely in software: decoded frames are scaled and blitted
into a backbuffer, which is copied to the console framebuffer on vblank. There
is no Direct3D in the pipeline.

Inbound frames are decoded with a **bounded** macroblock loop: the decoder will
not iterate past the frame's declared MCU count, so a malformed, truncated, or
hostile JPEG from a peer is dropped (logged as a decode overrun) instead of
stalling the decoder. This closes an availability gap in which a single bad frame
could hang the console. The frame is authenticated before it ever reaches the
decoder, so only a peer that already holds the room key can submit one at all.

The relay never processes or decodes video.

---

# File Transfer Architecture

```text
File
 ↓
Chunking
 ↓
AES-CBC
 ↓
HMAC-SHA256
 ↓
Relay
 ↓
Verify
 ↓
Decrypt
 ↓
Reassembly
```

Both metadata and file chunks are authenticated.

Tampered chunks are discarded.

---

# Relay Architecture

The relay is intentionally zero-knowledge.

## Relay Can See

- Client IP addresses
- Packet timing
- Packet sizes
- Room hashes
- Connection counts

## Relay Cannot See

- Text messages
- Voice data
- Video frames
- Files
- Session keys
- Room tokens

Rooms exist entirely in memory.

No message persistence exists.

---

# Metadata Exposure

Encryption does not conceal:

- Packet sizes
- Timing patterns
- Client IP addresses
- Room activity

Traffic analysis remains possible.

---

# Local Logging

Consoles maintain:

```text
E:\Dashboard\system\xnet.log
```

Default logging contains:

- Boot events
- Connection events

Debug mode enables additional diagnostics.

Message contents are never logged.

Logs are truncated on every boot.

---

# Known Limitations

| Limitation | Impact |
|------------|--------|
| Shared room key | No individual identity |
| No forward secrecy | Captured traffic decryptable if token compromised |
| Metadata visibility | Traffic analysis possible |
| No independent audit | Implementation risk |
| Endpoint compromise | Defeats encryption |
| Token compromise | Full room compromise |

---

# Anti-Replay

## Threat

XNET frames are authenticated (encrypt-then-MAC), so an attacker cannot forge or
tamper with a frame without the room key. Authentication alone does **not** stop
**replay**: a captured, still-valid frame that is re-injected verifies fine.

Two replay vectors matter for XNET specifically:

1. **In-session replay.** The relay terminates the TCP connection, TCP's own
   sequence numbers protect each hop, not end-to-end. A malicious or compromised
   relay can re-broadcast a member's frame (or reorder/drop frames) within a
   live room.

2. **Cross-session replay.** Room keys are derived as `SHA256(token)` and are
   therefore **static and reused** every time a room is created with the same
   token. A frame captured from an earlier room can be replayed into a later
   room that happens to share the token, because the key — and thus the MAC —
   is identical across both.

## Mechanisms

Both mechanisms are bound into the existing HMAC, so neither can be stripped or
rewritten without invalidating the tag.

### 1. Sequence counter + sliding receive window (in-session)

* Each sender maintains a per-stream monotonic 64-bit counter. The value is
  written big-endian as an 8-byte prefix ahead of the IV on every encrypted
  frame. Counters start at 1; 0 is reserved.
* Each receiver maintains an anti-replay **sliding window** per
  `(sender slot, stream class)`. A frame is accepted iff its sequence number is
  newer than the window's high-water mark, or falls inside the window and has
  not been seen. This is the IPsec/DTLS pattern: it accepts in-order traffic on
  the fast path, tolerates the reordering and loss a hostile relay might
  introduce, and rejects any duplicate or too-old sequence.
* Window width is `XNET_REPLAY_WINDOW_WORDS × 64` (default 128 packets). XNET
  rides a single in-order TCP stream, so the monotonic fast path is the norm;
  the window only does work if the relay reorders.

### 2. Per-room session nonce (cross-session)

* The relay generates 16 random bytes (`crypto.randomBytes(16)`) when a room is
  created and stores them as `room.sessionId`.
* The nonce is delivered to the host appended to the `TOKEN` packet and to each
  joiner appended to the `JOINED` packet. All room members thus share the same
  value.
* The nonce is **mixed into the MAC but never transmitted in data frames.** The
  receiver reconstructs it locally. A frame from a different room carries a tag
  computed over a different nonce, so it fails verification — even though the key
  is identical. This is what defeats cross-session replay despite static,
  token-derived keys.
* The nonce is **not secret** — its only job is uniqueness per room. Leaking it
  reveals nothing.

## Wire format

The encrypted payload of every encrypted packet type (`TEXT`, `VOICE`, `VIDEO`,
`FILE_META`, `FILE_DATA`, and their `_RELAY` echoes) changes from:

```
[IV 16][AES-CBC ciphertext][HMAC 32]
```

to:

```
[seq 8 BE][IV 16][AES-CBC ciphertext][HMAC 32]
```

The HMAC-SHA-256 tag is computed over the associated data followed by the
existing input:

```
AAD = session_id(16) || stream(1) || sender_slot(1) || seq(8)        (26 bytes)
tag = HMAC-SHA256( mac_key, AAD || IV || ciphertext )
```

`mac_key` is unchanged: `SHA256(aes_key || "XNET-MAC1")`. Binding `stream` and
`sender_slot` into the AAD prevents a frame from being relabelled across stream
classes or spoofed as another slot to slip past a different window. `session_id`
and the AAD bytes are authenticated but **not** stored in the frame; the receiver
supplies the same values to verify.

The relay is unaffected by the `seq` prefix — it forwards encrypted payloads
verbatim and remains fully blind. Its only change is minting and distributing the
session nonce.

## Receive order of operations (load-bearing)

On every encrypted frame the receiver does, strictly in this order:

1. Reconstruct the AAD using the **local** `session_id`, `stream`, claimed
   `sender_slot`, and the `seq` read from the frame.
2. Recompute the HMAC and compare in constant time. On mismatch → **drop**
   (forged, tampered, or wrong-room).
3. Only after authenticity is proven, run the replay-window check for
   `(sender_slot, stream)`. On replay/too-old → **drop**.
4. Decrypt.

Verifying the MAC *before* touching the window is mandatory: if an unverified
sequence number could mark the bitmap, an attacker could punch holes in it and
deny service to legitimate frames.

## Properties and non-goals

* **Adds:** detection and rejection of replayed and (relay-induced) reordered
  frames, in-session and across sessions that share a token.
* **Does not add:** confidentiality or integrity — those are provided by the
  existing encrypt-then-MAC and are untouched.
* **Degraded mode:** if a member is connected to a relay that predates v0.4.5
  and never sends a nonce, `session_id` stays all-zero on both ends. Two v0.4.5
  peers still agree on that (zero) value, so in-session replay protection still
  holds; only cross-session protection is lost. (This only arises in a
  mixed-version deployment, which is otherwise unsupported — see below.)
* **Counter exhaustion:** a 64-bit per-stream counter cannot realistically wrap
  (centuries at any real packet rate). If it ever reaches 0, `seal` refuses to
  transmit and the caller must start a new room. Never reuse a counter under the
  same key.

## State cost

Per session: `16` bytes session_id, `MAX_SLOTS × STREAM_COUNT` receive windows
(`4 × 4 = 16` windows × `(8 + WINDOW_WORDS×8)` bytes = ~384 bytes at default
width) and `STREAM_COUNT` send counters (32 bytes). Well under 0.5 KB total — no
heap, no FATX writes in the hot path.

## Compatibility

This is a **wire-format break.** The 8-byte prefix shifts every encrypted frame,
and the MAC input changes, so a pre-v0.4.5 peer and a v0.4.5 peer cannot
interoperate. All clients **and** the relay must be on v0.4.5 together.


# Recommendations for Relay Operators

Run relays:

- Behind firewalls
- As unprivileged users
- With patched operating systems
- With patched Node.js versions

Consider:

- Rate limiting
- Reverse proxies
- DDoS protection

Avoid:

- Packet logging
- Content inspection
- Persistent room storage

The relay's zero-knowledge design is a feature.

---

# Future Improvements

Planned hardening includes:

- Forward secrecy
- Ephemeral session keys
- Per-user identities
- Digital signatures
- Session ratcheting
- Formal protocol review
- Independent audit

---

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

*"Privacy by Design."*