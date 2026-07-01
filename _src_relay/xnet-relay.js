/**
 * XNET Relay Server
 * Dead Orbit Studios / Tsardev
 *
 * Zero-persistence encrypted relay for Original Xbox text + voice chat.
 * No disk writes. No logs. No message storage. Rooms exist in RAM only.
 * Server is blind — all payloads are AES ciphertext derived from the room token.
 *
 * Protocol: binary TCP framing
 *   [1 byte]  packet type
 *   [1 byte]  slot ID (0-3)
 *   [2 bytes] payload length (uint16 big-endian)
 *   [N bytes] payload (ciphertext blob)
 */

'use strict';

const net = require('net');
const crypto = require('crypto');

// ─── CONFIG ────────────────────────────────────────────────────────────────────
const PORT            = process.env.XNET_PORT  || 7777;
const MAX_SLOTS       = 4;
const TOKEN_LENGTH    = 8;
const TOKEN_CHARS     = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'; // no O/0/1/I ambiguity
const TOKEN_TTL_MS    = 5  * 60 * 1000;  // 5 min to join before token expires
const IDLE_TTL_MS     = 10 * 60 * 1000;  // 10 min idle auto-destroy
const PING_INTERVAL   = 30 * 1000;       // 30s keepalive
const MAX_PAYLOAD     = 8192;            // max frame payload bytes (safety cap)

// ─── PACKET TYPES ──────────────────────────────────────────────────────────────
const PKT = {
  CREATE:      0x01,
  TOKEN:       0x02,
  JOIN:        0x03,
  JOINED:      0x04,
  PEER_JOINED: 0x05,
  TEXT:        0x06,
  TEXT_RELAY:  0x07,
  VOICE:       0x08,
  VOICE_RELAY: 0x09,
  PING:        0x0A,
  PONG:        0x0B,
  PEER_LEFT:   0x0C,
  END:         0x0D,
  KICK:        0x0E,
  ERROR:       0x0F,
  // ── Secure Transfer (blind-relayed like TEXT/VOICE) ──
  FILE_META:        0x10,
  FILE_META_RELAY:  0x11,
  FILE_DATA:        0x12,
  FILE_DATA_RELAY:  0x13,
  FILE_PROG:        0x14,
  FILE_PROG_RELAY:  0x15,
  FILE_DONE:        0x16,
  FILE_DONE_RELAY:  0x17,
  // ── Secure Video (one encrypted JPEG frame per packet, blind-relayed) ──
  VIDEO:            0x18,
  VIDEO_RELAY:      0x19,
};

// ─── ROOM STORE (RAM ONLY) ─────────────────────────────────────────────────────
// rooms = Map<token, Room>
// Room = { token, slots: [Socket|null x4], createdAt, lastActive, joinTimer, idleTimer }
const rooms = new Map();

// ─── TOKEN GENERATION ──────────────────────────────────────────────────────────
function generateToken() {
  let token;
  do {
    token = Array.from(
      crypto.randomBytes(TOKEN_LENGTH),
      b => TOKEN_CHARS[b % TOKEN_CHARS.length]
    ).join('');
  } while (rooms.has(token));
  return token;
}

// ─── PACKET BUILDER ────────────────────────────────────────────────────────────
function buildPacket(type, slot, payload) {
  const data   = payload ? Buffer.from(payload) : Buffer.alloc(0);
  const header = Buffer.alloc(4);
  header[0] = type;
  header[1] = slot & 0xFF;
  header.writeUInt16BE(data.length, 2);
  return Buffer.concat([header, data]);
}

// ─── SAFE SOCKET WRITE ─────────────────────────────────────────────────────────
function safeSend(socket, type, slot, payload) {
  if (!socket || socket.destroyed) return;
  try {
    socket.write(buildPacket(type, slot, payload));
  } catch (_) { /* socket gone, ignore */ }
}

// ─── BROADCAST TO ROOM (excluding sender slot) ────────────────────────────────
function broadcast(room, type, senderSlot, payload) {
  for (let i = 0; i < MAX_SLOTS; i++) {
    if (i === senderSlot || !room.slots[i]) continue;
    safeSend(room.slots[i], type, senderSlot, payload);
  }
}

// ─── ROOM CLEANUP ──────────────────────────────────────────────────────────────
function destroyRoom(token, reason) {
  const room = rooms.get(token);
  if (!room) return;

  clearTimeout(room.joinTimer);
  clearTimeout(room.idleTimer);

  // kick all connected slots
  const kickPayload = Buffer.from(reason || 'room ended');
  for (let i = 0; i < MAX_SLOTS; i++) {
    if (!room.slots[i]) continue;
    safeSend(room.slots[i], PKT.KICK, i, kickPayload);
    try { room.slots[i].destroy(); } catch (_) {}
  }

  rooms.delete(token);
}

// ─── RESET IDLE TIMER ──────────────────────────────────────────────────────────
function resetIdle(room) {
  room.lastActive = Date.now();
  clearTimeout(room.idleTimer);
  room.idleTimer = setTimeout(() => {
    destroyRoom(room.token, 'idle timeout');
  }, IDLE_TTL_MS);
}

// Same effect but throttled to once / 30s — for high-rate streams (voice, video)
// that ARE activity but shouldn't churn a timer on every packet. Without this a
// pure video/voice call with no text traffic hits IDLE_TTL and gets destroyed.
function touchIdle(room) {
  const now = Date.now();
  if (now - (room.lastIdleTouch || 0) < 30000) return;
  room.lastIdleTouch = now;
  resetIdle(room);
}

// ─── PEER COUNT ────────────────────────────────────────────────────────────────
function peerCount(room) {
  return room.slots.filter(Boolean).length;
}

// ─── HANDLE CLIENT DISCONNECT ─────────────────────────────────────────────────
function handleDisconnect(room, slotIndex) {
  if (!room.slots[slotIndex]) return;
  room.slots[slotIndex] = null;

  const slotBuf = Buffer.alloc(1);
  slotBuf[0] = slotIndex;
  broadcast(room, PKT.PEER_LEFT, slotIndex, slotBuf);

  // if host (slot 0) leaves, destroy room
  if (slotIndex === 0) {
    destroyRoom(room.token, 'host disconnected');
    return;
  }

  // if everyone is gone, destroy
  if (peerCount(room) === 0) {
    destroyRoom(room.token, 'empty');
  } else {
    resetIdle(room);
  }
}

// ─── PACKET PARSER — streaming buffer reassembly ───────────────────────────────
function makeParser(onPacket) {
  let buf = Buffer.alloc(0);

  return function onData(chunk) {
    buf = Buffer.concat([buf, chunk]);

    while (buf.length >= 4) {
      const payloadLen = buf.readUInt16BE(2);

      // safety: drop oversized frames
      if (payloadLen > MAX_PAYLOAD) {
        buf = Buffer.alloc(0);
        return;
      }

      if (buf.length < 4 + payloadLen) break; // wait for more data

      const type    = buf[0];
      const slot    = buf[1];
      const payload = buf.slice(4, 4 + payloadLen);
      buf           = buf.slice(4 + payloadLen);

      onPacket(type, slot, payload);
    }
  };
}

// ─── CONNECTION HANDLER ────────────────────────────────────────────────────────
function handleConnection(socket) {
  socket.setNoDelay(true);
  socket.setTimeout(PING_INTERVAL * 3); // 3 missed pings = dead

  let assignedRoom  = null;
  let assignedSlot  = -1;
  let pingTimer     = null;

  function cleanup() {
    clearInterval(pingTimer);
    if (assignedRoom && assignedSlot >= 0) {
      handleDisconnect(assignedRoom, assignedSlot);
      assignedRoom = null;
      assignedSlot = -1;
    }
  }

  socket.on('error',   cleanup);
  socket.on('close',   cleanup);
  socket.on('timeout', () => { socket.destroy(); });

  // keepalive ping
  pingTimer = setInterval(() => {
    safeSend(socket, PKT.PING, 0, null);
  }, PING_INTERVAL);

  // ── packet dispatch ──
  const parser = makeParser((type, _slot, payload) => {
    switch (type) {

      // ── CREATE: host requests a new room ──
      case PKT.CREATE: {
        if (assignedRoom) return; // already in a room

        const token = generateToken();
        const room  = {
          token,
          sessionId:  crypto.randomBytes(16), // per-room nonce -> client replay session_id
          slots:      new Array(MAX_SLOTS).fill(null),
          createdAt:  Date.now(),
          lastActive: Date.now(),
          joinTimer:  null,
          idleTimer:  null,
        };

        room.slots[0] = socket;
        rooms.set(token, room);

        assignedRoom = room;
        assignedSlot = 0;

        // send token + session nonce back to host (client appends nonce to its
        // replay state; nonce is non-secret, binds frames to this room only)
        safeSend(socket, PKT.TOKEN, 0,
                 Buffer.concat([Buffer.from(token, 'ascii'), room.sessionId]));

        // expire room if no one joins within TTL
        room.joinTimer = setTimeout(() => {
          if (peerCount(room) < 2) {
            destroyRoom(token, 'token expired');
          }
        }, TOKEN_TTL_MS);

        resetIdle(room);
        break;
      }

      // ── JOIN: peer submits token ──
      case PKT.JOIN: {
        if (assignedRoom) return; // already in a room

        const token = payload.toString('ascii').trim().toUpperCase();
        const room  = rooms.get(token);

        if (!room) {
          safeSend(socket, PKT.ERROR, 0, Buffer.from('invalid token'));
          return;
        }

        // find open slot
        const slot = room.slots.indexOf(null);
        if (slot === -1) {
          safeSend(socket, PKT.ERROR, 0, Buffer.from('room full'));
          return;
        }

        room.slots[slot] = socket;
        assignedRoom = room;
        assignedSlot = slot;

        // tell joiner their slot (+ this room's session nonce)
        const joinedBuf = Buffer.alloc(1);
        joinedBuf[0] = slot;
        safeSend(socket, PKT.JOINED, slot,
                 Buffer.concat([joinedBuf, room.sessionId]));

        // notify existing peers about the new joiner
        const peerBuf = Buffer.alloc(1);
        peerBuf[0] = slot;
        broadcast(room, PKT.PEER_JOINED, slot, peerBuf);

        // …and tell the new joiner about everyone ALREADY in the room, or its
        // grid never registers them and their tiles stay stuck on "connecting"
        // even though their video/voice is arriving.
        for (let i = 0; i < MAX_SLOTS; i++) {
          if (i !== slot && room.slots[i]) {
            const existBuf = Buffer.alloc(1);
            existBuf[0] = i;
            safeSend(socket, PKT.PEER_JOINED, i, existBuf);
          }
        }

        resetIdle(room);
        break;
      }

      // ── TEXT: encrypted text frame — blind relay ──
      case PKT.TEXT: {
        if (!assignedRoom || assignedSlot < 0) return;
        broadcast(assignedRoom, PKT.TEXT_RELAY, assignedSlot, payload);
        resetIdle(assignedRoom);
        break;
      }

      // ── VOICE: encrypted audio frame — blind relay ──
      case PKT.VOICE: {
        if (!assignedRoom || assignedSlot < 0) return;
        broadcast(assignedRoom, PKT.VOICE_RELAY, assignedSlot, payload);
        touchIdle(assignedRoom);   /* voice is activity (throttled) */
        break;
      }

      // ── VIDEO: encrypted JPEG frame — blind relay ──
      case PKT.VIDEO: {
        if (!assignedRoom || assignedSlot < 0) return;
        broadcast(assignedRoom, PKT.VIDEO_RELAY, assignedSlot, payload);
        touchIdle(assignedRoom);   /* video is activity (throttled) */
        break;
      }

      // ── SECURE TRANSFER: encrypted file frames — blind relay ──
      case PKT.FILE_META: {
        if (!assignedRoom || assignedSlot < 0) return;
        broadcast(assignedRoom, PKT.FILE_META_RELAY, assignedSlot, payload);
        resetIdle(assignedRoom);
        break;
      }

      case PKT.FILE_DATA: {
        if (!assignedRoom || assignedSlot < 0) return;
        broadcast(assignedRoom, PKT.FILE_DATA_RELAY, assignedSlot, payload);
        resetIdle(assignedRoom);
        break;
      }

      case PKT.FILE_PROG: {
        if (!assignedRoom || assignedSlot < 0) return;
        broadcast(assignedRoom, PKT.FILE_PROG_RELAY, assignedSlot, payload);
        resetIdle(assignedRoom);
        break;
      }

      case PKT.FILE_DONE: {
        if (!assignedRoom || assignedSlot < 0) return;
        broadcast(assignedRoom, PKT.FILE_DONE_RELAY, assignedSlot, payload);
        resetIdle(assignedRoom);
        break;
      }

      // ── PING: respond with PONG ──
      case PKT.PING: {
        safeSend(socket, PKT.PONG, assignedSlot < 0 ? 0 : assignedSlot, null);
        break;
      }

      // ── PONG: keepalive ack, nothing to do ──
      case PKT.PONG: {
        break;
      }

      // ── END: host destroys room ──
      case PKT.END: {
        if (!assignedRoom || assignedSlot !== 0) return; // host only
        destroyRoom(assignedRoom.token, 'host ended session');
        assignedRoom = null;
        assignedSlot = -1;
        break;
      }

      default:
        break; // unknown packet type — silently drop
    }
  });

  socket.on('data', parser);
}

// ─── SERVER ────────────────────────────────────────────────────────────────────
const server = net.createServer(handleConnection);

// suppress all server-level logging — relay is silent
server.on('error', () => {});

server.listen(PORT, '0.0.0.0', () => {
  // one startup line only — no runtime logging after this
  process.stdout.write(`XNET relay listening on :${PORT} (secure-transfer: 0x10-0x17, secure-video: 0x18-0x19)\n`);
  // redirect stdout/stderr to /dev/null after boot if running in prod
  // (handled by systemd unit — see xnet.service)
});

// ─── GRACEFUL SHUTDOWN ─────────────────────────────────────────────────────────
function shutdown() {
  // destroy all rooms cleanly
  for (const token of rooms.keys()) {
    destroyRoom(token, 'server shutdown');
  }
  server.close(() => process.exit(0));
}

process.on('SIGINT',  shutdown);
process.on('SIGTERM', shutdown);

// ─── UNHANDLED REJECTION SINK — no crash logging ──────────────────────────────
process.on('uncaughtException',  () => {});
process.on('unhandledRejection', () => {});
