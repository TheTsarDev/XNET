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
6. [Authenticated Encryption](#authenticated-encryption)
7. [Packet Types and Flow](#packet-types-and-flow)
8. [Voice Architecture](#voice-architecture)
9. [Video Architecture](#video-architecture)
10. [File Transfer Architecture](#file-transfer-architecture)
11. [Relay Architecture](#relay-architecture)
12. [Metadata Exposure](#metadata-exposure)
13. [Local Logging](#local-logging)
14. [Known Limitations](#known-limitations)
15. [Recommendations for Relay Operators](#recommendations-for-relay-operators)
16. [Future Improvements](#future-improvements)

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

All packets use:

```text
[1 byte]   packet type
[1 byte]   slot
[2 bytes]  payload length
[16 bytes] IV
[N bytes]  AES-CBC ciphertext
[32 bytes] HMAC-SHA-256 tag
```

The authentication tag covers:

```text
IV || ciphertext
```

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

Every packet receives a fresh 16-byte initialization vector.

Encryption protects:

- Text
- Voice
- Video
- File metadata
- File transfer chunks

---

# Authenticated Encryption

Beginning with v0.4.0, XNET uses an Encrypt-then-MAC construction.

```text
tag = HMAC-SHA256(mac_key, IV || ciphertext)

packet =
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
| No replay protection | Captured packets may be replayed |
| No forward secrecy | Captured traffic decryptable if token compromised |
| IV generator not cryptographically secure | Lower entropy than modern systems |
| Metadata visibility | Traffic analysis possible |
| No independent audit | Implementation risk |
| Endpoint compromise | Defeats encryption |
| Token compromise | Full room compromise |

---

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

- Replay protection
- Cryptographically secure IV generation
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