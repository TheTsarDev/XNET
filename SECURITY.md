# Security Policy

**Current version: 0.4.1 beta**

XNET is an end-to-end encrypted text, voice, file, and video communication
system for the original Xbox. Because it handles private communication, I take
its security seriously — and I also want to be honest about exactly what it
does and does not protect. This document describes XNET's security model, its
known limitations, and how to report a vulnerability.

> 📐 **For the complete technical design** — key derivation, packet framing,
> the encrypt-then-MAC construction, per-feature data paths, and the relay
> protocol — see [**Security Architecture**](SECURITY_ARCHITECTURE.md). This
> policy is the summary and the reporting process; the architecture document is
> the full reference.

XNET is hobbyist software built for 24-year-old retail hardware and the Xbox
preservation community. It has not been independently audited. Read the
**Threat Model and Limitations** section before relying on it for anything
sensitive.

---

## Supported Versions

Security fixes are applied to the most recent release only. XNET is
pre-1.0 software under active development.

| Version       | Supported          |
| ------------- | ------------------ |
| 0.4.1 beta    | ✅                |
| < 0.4.0       | ❌                |

Always run the latest build. Older builds will not receive fixes. **0.4.0
changed the wire format** (authenticated encryption — see below) and is **not
compatible with 0.3.x**; every console and relay in a session must run 0.4.0 or
later.

---

## Security Model

### What XNET protects

XNET is designed so that **only the people in a room can read what is sent in
that room** — the relay operator cannot.

- **End-to-end encryption.** All payloads — text, voice, file transfers, and
  video — are encrypted on the sending console and decrypted only on the
  receiving consoles. Encryption uses **AES-128 in CBC mode** (via the
  public-domain tiny-AES-c implementation), with a fresh 16-byte
  initialization vector generated per message and prepended to the ciphertext.

- **Authenticated encryption (encrypt-then-MAC).** As of 0.4.0, every packet
  carries an **HMAC-SHA-256 tag** computed over `(IV || ciphertext)` using a MAC
  subkey derived from the session key with domain separation. The receiver
  verifies the tag in constant time **before decrypting** and silently drops any
  packet that fails. This detects tampering and forgery, and prevents an
  attacker from feeding modified ciphertext into the decryptor. (A bad packet is
  treated exactly like a lost one — the session continues.)

- **Local key derivation.** The session key is derived locally on each console
  as `SHA-256(room token)` truncated to 128 bits. **The key is never
  transmitted** — not to the relay, not to peers. Two consoles that type the
  same room token independently arrive at the same key. The token is the shared
  secret.

- **Zero-knowledge relay.** The relay is a blind forwarder. It moves encrypted
  packets between the consoles in a room and nothing else. It cannot decrypt
  traffic because it never has the key. Rooms exist only in memory, are created
  on demand, and are destroyed when they go idle or empty. The relay keeps **no
  message store, no accounts, no friend lists, and no persistent content
  logs.**

- **No peer IP exposure.** Consoles connect to the relay, not directly to each
  other, so peers in a room do not learn one another's IP addresses.

### What the relay can and cannot see

| The relay can see                          | The relay cannot see                  |
| ------------------------------------------ | ------------------------------------- |
| Client IP addresses (it terminates the TCP connection) | Message, voice, file, or video content |
| Which clients share a room token's hash    | The room token itself                 |
| Packet sizes and timing                    | Decryption keys                       |
| When rooms are created and destroyed       | Anything after a room is destroyed    |

---

## Threat Model and Limitations

XNET aims to keep casual conversation private from the relay operator and from
passive network observers. It is **not** designed to withstand a determined or
well-resourced attacker. Please understand the following before trusting it
with anything that matters.

- **The room token is the entire secret.** Anyone who knows a room's token can
  join it, decrypt everything, send messages, and impersonate participants.
  There is no per-user identity or authentication. **Choose long, unpredictable
  tokens and share them over a channel you already trust.** Short or guessable
  tokens produce weak keys.

- **Authentication proves room membership, not individual identity.** The MAC
  (see above) proves a packet was produced by someone holding the room key and
  has not been altered. It does **not** distinguish between members of the same
  room — everyone in a room shares one key, so any member can send as the room.
  There is no per-user identity.

- **No replay protection at the transport layer.** The MAC stops modified or
  forged packets, but a valid packet captured off the wire could be re-injected
  and would pass verification. XNET does not currently reject replayed packets.

- **No forward secrecy.** The key is static for a given token. If a token is
  later disclosed, any traffic that was captured while that token was in use
  can be decrypted retroactively.

- **Initialization vectors are unique but not cryptographically random.** IVs
  are produced by a fast PRNG seeded from the Xbox performance counter. They
  differ per message but are not from a cryptographically secure source.

- **Metadata is not hidden.** The relay operator and anyone watching the network
  can observe IP addresses, connection times, packet sizes, and traffic
  patterns, even though they cannot read content.

- **Local logging.** Each console writes a local log to
  `E:\Dashboard\system\xnet.log` (boot and event lines by default; a full
  per-frame trace if **Debug Logging** is enabled in Settings). This log lives
  in plaintext on the console's own disk. It does not contain message content,
  but it does contain connection metadata. It is truncated on every boot.

- **Implementation maturity.** XNET is beta homebrew. tiny-AES-c is a compact,
  portable implementation and is not hardened against timing or other
  side-channel attacks. On a single-user console these classes of attack are
  largely impractical, but they are not mitigated.

- **Not for high-risk use.** XNET should not be relied upon to protect
  communication from law enforcement, nation-state adversaries, or anyone whose
  safety depends on the secrecy of the contents. Use purpose-built, audited
  tools for those situations.

---

## Reporting a Vulnerability

If you believe you have found a security vulnerability in XNET — in the Xbox
client, the relay, the cryptography, or the protocol — please report it
privately. **Do not open a public GitHub issue for security problems**, as that
discloses the issue before a fix is available.

Preferred reporting channels:

1. **GitHub private vulnerability reporting** — on the repository, go to the
   **Security** tab and choose **Report a vulnerability**. This opens a private
   advisory visible only to the maintainers.
2. **Discord** Find me in the OGXBL, XboxDev, XBOX-SCENE

Please include as much of the following as you can:

- A description of the vulnerability and the component affected (client, relay,
  crypto, protocol).
- Steps to reproduce, or a proof of concept.
- The XNET version and console/relay environment.
- The impact you believe it has, and any suggested remediation.

### What to expect

- **Acknowledgment** of your report within a reasonable time (this is a
  volunteer project, so please allow several days).
- An assessment of the report and, if valid, a plan and timeline for a fix.
- Coordinated disclosure: we ask that you give us a reasonable window to
  release a fix before any public disclosure. We will credit you in the release
  notes and advisory unless you prefer to remain anonymous.

---

## Scope

**In scope**

- The XNET Xbox client (encryption, key handling, networking, input handling,
  parsing of received packets).
- The XNET relay server (packet handling, room lifecycle, resource handling).
- The wire protocol and cryptographic design.

**Out of scope**

- Vulnerabilities in the underlying original Xbox hardware, its kernel, the
  NXDK toolchain, or third-party homebrew/dashboards.
- Attacks that require physical access to a participant's console.
- Social engineering, or compromise that depends on the user sharing the room
  token with an untrusted party.
- Denial of service against a self-hosted relay from a network position that
  could disrupt any TCP service (e.g. raw bandwidth flooding).

---

## Recommendations for Relay Operators

If you host an XNET relay:

- Run it as an unprivileged user and keep the host and Node.js runtime patched.
- The relay is a blind forwarder and stores no content, but it does terminate
  client TCP connections and therefore sees client IP addresses. Be transparent
  with your users about that.
- Consider standard transport-level protections (firewalling, rate limiting) in
  front of the relay to reduce abuse.
- Do not add content logging. The zero-knowledge property is a feature; keep it.

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
