# Flurry Streaming Protocol v1

> **Status: draft — pending ground-up redesign.** This version is a strawman;
> the protocol will likely be rewritten once the 3DS-side capture/encode loop
> has been profiled (transport choice, raw formats, frame skip are all open).
> Do not treat the wire format below as frozen.

Source of truth for the wire protocol between the Flurry 3DS system module
(`soos/`) and PC clients ([flurry-client](https://github.com/JohnRamberger/flurry-client)).
Both sides implement exactly what is written here. Changes to this file and to
either implementation must land together.

This is a clean-slate protocol. It is **not** compatible with the legacy
HzMod/ChirunoMod protocol used by Chokistream and HorizonScreen; the legacy
protocol is documented in [Appendix A](#appendix-a-legacy-hzmod-protocol)
for reference.

## 1. Overview

- The 3DS captures its screens, encodes each capture as JPEG (or optionally
  TGA), and streams the images to a PC over WiFi.
- Transport: a single bidirectional **TCP** connection.
- The **3DS is the server**, listening on port **6464**. The PC is the client.
- One client at a time (listen backlog 1). A new connection replaces a dead one.
- All multi-byte integers are **little-endian**. There is no alignment padding
  beyond what is written explicitly.

## 2. Message framing

Every message in either direction is:

| Offset | Size | Field    | Notes                              |
|--------|------|----------|------------------------------------|
| 0      | 1    | `type`   | Message type (see catalog)         |
| 1      | 3    | reserved | Sender writes 0, receiver ignores  |
| 4      | 4    | `length` | u32 LE, payload bytes that follow  |
| 8      | n    | payload  | `length` bytes                     |

Type space:

- `0x01–0x7F` — client → server (PC → 3DS)
- `0x80–0xFE` — server → client (3DS → PC)
- `0xFF` — `ERROR`, either direction

### Extensibility rules

- **Unknown message types are ignored** (skipped using `length`), except
  during the handshake (§3).
- **Payloads may grow**: a receiver must accept a payload longer than the
  fields it knows and ignore the trailing bytes. Senders must never shrink a
  payload below the size defined here for the protocol version in use.
- Incrementing behavior that would break an existing implementation requires a
  new `proto_version` in `HELLO`.

## 3. Connection lifecycle

```
PC                                3DS (listening on :6464)
|  ---------- TCP connect ---------->  |
|  <---------- HELLO ----------------  |   immediately on accept
|  ----------- CONFIG -------------->  |   optional, any number
|  ----------- START --------------->  |
|  <--- FRAME, FRAME, STATS, ... ----  |   until STOP or disconnect
|  ----------- CONFIG -------------->  |   allowed mid-stream, applies live
|  ----------- STOP ---------------->  |   back to configured-idle state
|  ----------- DISCONNECT ---------->  |   3DS closes, returns to listen
```

- On accept, the 3DS immediately sends `HELLO`. The client must validate the
  magic and `proto_version` before sending anything; on mismatch it just
  closes the socket.
- `CONFIG` is idempotent full state (not a delta). The 3DS applies it whenever
  received, including mid-stream.
- If the TCP connection drops, the 3DS stops streaming and returns to listen.

## 4. Message catalog

### 4.1 `HELLO` — `0x80` (3DS → PC)

Sent once, immediately after accept.

| Offset | Size | Field          | Notes                                   |
|--------|------|----------------|-----------------------------------------|
| 0      | 4    | `magic`        | ASCII `"FLRY"` (`46 4C 52 59`)          |
| 4      | 2    | `proto_version`| u16, this document = `1`                |
| 6      | 1    | `device`       | 0 = New 3DS, 1 = Old 3DS                |
| 7      | 1    | reserved       |                                          |

`device` tells the client what to expect: Old 3DS streams frames in chunks
(§5.2) and does not support interlacing.

### 4.2 `CONFIG` — `0x02` (PC → 3DS)

Full desired state. Fields outside their valid range are clamped by the 3DS.

| Offset | Size | Field       | Notes                                       |
|--------|------|-------------|----------------------------------------------|
| 0      | 1    | `quality`   | JPEG quality 1–100 (default 70)              |
| 1      | 1    | `screens`   | 0 = top, 1 = bottom, 2 = both (default 0)    |
| 2      | 1    | `format`    | 0 = JPEG, 1 = TGA (default 0)                |
| 3      | 1    | `interlace` | 0 = off, 1 = on (default 0; forced off on Old 3DS and on 24-bit source formats) |

### 4.3 `START` — `0x03` (PC → 3DS)

No payload. Begin streaming `FRAME` messages with the current config.

### 4.4 `STOP` — `0x04` (PC → 3DS)

No payload. Stop streaming; connection and config stay intact.

### 4.5 `DISCONNECT` — `0x05` (PC → 3DS)

No payload. The 3DS stops streaming, closes the socket, and returns to listen.

### 4.6 `DEBUG` — `0x7F` (PC → 3DS)

Payload: UTF-8 text. Logged on the 3DS in debug builds, otherwise ignored.

### 4.7 `FRAME` — `0x81` (3DS → PC)

One encoded image: a full frame, one chunk of a frame (Old 3DS), or one field
of an interlaced frame.

| Offset | Size | Field         | Notes                                        |
|--------|------|---------------|-----------------------------------------------|
| 0      | 1    | `screen`      | 0 = top (400×240), 1 = bottom (320×240)       |
| 1      | 1    | `format`      | 0 = JPEG, 1 = TGA (RLE)                       |
| 2      | 1    | `pixfmt`      | GSP source format, informative only (§5.4)    |
| 3      | 1    | `interlace`   | 0 = progressive, 1 = field A, 2 = field B (§5.3) |
| 4      | 1    | `chunk_index` | 0-based, `< chunk_count`                      |
| 5      | 1    | `chunk_count` | 1 = whole frame in one message (§5.2)         |
| 6      | 2    | `frame_id`    | u16, wraps; all chunks of one capture share it |
| 8      | n    | image data    | standard JPEG or TGA stream                   |

Image dimensions come from the JPEG/TGA headers, not the protocol. Decoded
images are in framebuffer orientation and must be rotated by the client (§5.1).

`frame_id` increments by 1 (mod 65536) per capture, independently per screen.
A client reassembling chunks discards an incomplete frame when a new
`frame_id` for that screen arrives.

### 4.8 `STATS` — `0x82` (3DS → PC)

Payload: UTF-8 text, `key=value` pairs separated by newlines (frame times,
encode times, etc.). Informative; emitted by verbose/debug builds. Clients
may display or ignore it.

### 4.9 `ERROR` — `0xFF` (either direction)

| Offset | Size | Field     | Notes                        |
|--------|------|-----------|-------------------------------|
| 0      | 1    | `code`    | 0 = unspecified               |
| 1      | n    | `message` | UTF-8 text                    |

The sender may close the connection after sending `ERROR`.

## 5. Image geometry

### 5.1 Orientation

3DS framebuffers are stored rotated 90° relative to the physical screen: one
framebuffer row is 240 pixels long and corresponds to one **column** of the
screen, starting at the bottom. Rows advance left to right across the screen.

Encoded images therefore have width 240 (or 120 when interlaced) and height
equal to the number of screen columns covered (400/320 full, or the chunk's
share). **To display, rotate the decoded image 90° counter-clockwise**:
image pixel (row `r`, col `c`) maps to screen pixel `x = r`, `y = 239 − c`
(y = 0 at the top of the screen; for interlaced fields see §5.3).

### 5.2 Chunking (Old 3DS)

Old 3DS lacks the memory and CPU to encode a full frame at once, so it splits
each capture into `chunk_count` = 8 vertical strips of the screen (contiguous
framebuffer slices): 50 screen-columns per chunk on top, 40 on bottom.
`chunk_index` runs left to right across the screen. New 3DS sends
`chunk_count` = 1.

Clients must handle any `chunk_count` ≥ 1 and reassemble by pasting chunk `i`
at screen-column offset `i × (screen_width / chunk_count)`.

### 5.3 Interlacing (New 3DS only)

When enabled, each capture carries every other pixel of each framebuffer row
(i.e. every other **screen row**), halving the image width to 120:

- `interlace = 1` (field A): framebuffer-row pixels 0, 2, 4, … 238
- `interlace = 2` (field B): framebuffer-row pixels 1, 3, 5, … 239

Fields alternate A/B on successive frames. Image pixel (row `r`, col `c`) of
field A maps to screen `y = 239 − 2c`; field B to `y = 239 − (2c + 1)`
(`x = r` as usual). Clients weave the two most recent fields, or
line-double a single field.

Interlacing is never combined with chunking.

### 5.4 `pixfmt` values (informative)

The GSP source framebuffer format the frame was captured from. Image payloads
are always self-describing RGB; this field only explains upstream quality
(e.g. a 16-bit source can't carry more than 5–6 bits per channel).

| Value | Format | Source bpp |
|-------|--------|------------|
| 0 | RGBA8  | 32 |
| 1 | RGB8   | 24 |
| 2 | RGB565 | 16 |
| 3 | RGB5A1 | 16 |
| 4 | RGBA4  | 16 |

## 6. Notes for implementers

- **3DS send buffer**: 48 KB on Old 3DS, 448 KB on New 3DS. A `FRAME` payload
  never exceeds the buffer minus the 8-byte header.
- **Client read loop**: read exactly 8 bytes, then exactly `length` bytes.
  `length` over the negotiated buffer size (448 KB) indicates a desynced or
  hostile stream — close the connection.
- **JPEG**: baseline, 4:2:0 chroma subsampling, encoded with libjpeg-turbo
  fast-DCT. Any compliant decoder works.
- **TGA**: RLE-compressed, BGR order (standard TGA).

---

## Appendix A: legacy HzMod protocol

Non-normative. What ChirunoMod/HzMod (and thus Chokistream/HorizonScreen)
speak, as implemented by the pre-rewrite `soos/main.cpp`. Kept for reference
during migration.

Same transport: TCP, 3DS server on 6464. Header (8 bytes):
`[type:u8][subtype:u8][subtypeB:u8][unused:u8][size:u32 LE]`, then payload.

**PC → 3DS**

| Type | Subtype | Meaning |
|------|---------|---------|
| 0x02 | —    | Init (start streaming) |
| 0x03 | —    | Disconnect |
| 0x04 | 0x01 | Set JPEG quality (u8 1–100) |
| 0x04 | 0x02 | Set CPU cap (u8; dummied out) |
| 0x04 | 0x03 | Set screen (u8: 1 top, 2 bottom, 3 both) |
| 0x04 | 0x04 | Set format (u8: 0 JPEG, 1 TGA) |
| 0x04 | 0x05 | Set interlace (u8 bool) |
| 0xFF | —    | Debug text |

**3DS → PC**

| Type | Subtype | Meaning |
|------|---------|---------|
| 0x01 | flags   | Image frame (see below) |
| 0xFF | 0x03    | Perf stats text |
| 0xFF | 0x00    | Error text |

Image frame `subtype` bit-packing: bits 0–2 = pixfmt (same table as §5.4),
bit 3 = TGA (0 = JPEG), bit 4 = screen (0 top, 1 bottom), bit 5 = interlaced,
bit 6 = interlace row phase. On Old 3DS, `subtypeB` = `0b00001000 + chunk_index`
(chunks 0–7); frames are split into 8 strips with a 5 ms sleep per chunk, and
interlacing is disabled. Defaults: quality 70, top screen, JPEG.

### A.1 Flurry legacy extensions (normative, interim)

Extensions the Flurry sysmodule and client add on top of the legacy protocol
until the v2 rewrite. Both directions stay compatible with pre-extension
peers: unknown setting subtypes are ignored by old sysmodules, and old
clients ignore the announce packet.

**Announce** — sent once by extended sysmodules immediately after the client
connects: type `0xFF`, subtype `0x04`, payload
`[announce_rev: u8][features: u8]`. Feature bits: bit 0 = strip skip,
bit 1 = fps cap, bit 2 = Old-3DS interlace. A client that has not received
an announce within ~1 s must assume a pre-extension sysmodule and only use
subtypes 0x01–0x05.

**New settings** (type `0x04`, u8 payload each):

| Subtype | Setting | Payload |
|---------|---------|---------|
| 0x06 | Strip skip | bool — don't send strips whose content (crc32) is unchanged |
| 0x07 | Refresh interval | force-send every N frames per strip, 0 = never |
| 0x08 | FPS cap | target fps, 0 = uncapped |

Strip skip requires the client to keep a persistent per-screen buffer (a
skipped strip simply stays stale until it changes or the refresh interval
forces it). The refresh interval bounds how long a lost/corrupt strip can
stay wrong.

## Appendix B: v2 transport direction (design notes, non-normative)

Decided direction for the protocol rewrite, pending one hardware
measurement:

- **Hybrid transport**: TCP :6464 for control (HELLO, config, stats,
  receiver reports) + **UDP for frames**. Video is perishable; TCP
  head-of-line blocking turns WiFi loss into freezes. Strips are already
  datagram-sized (~1–3 KB JPEG ≤ 2× 1400-byte datagrams); a lost datagram
  = one stale strip, healed by the refresh interval against the client's
  persistent screen buffer.
- **Datagram header**: `[frame_id][screen][strip][frag/frag_count]` so the
  client can paste strips in any order.
- **Client sends the first datagram** (hole punch) so 3DS→client traffic
  traverses the client firewall as established.
- **Backpressure replacement**: without TCP, the 3DS is blind to loss.
  Client sends periodic receiver reports (received/lost counts) over the
  TCP channel; the sysmodule's adaptive rate control uses loss% + encode
  time to auto-tune quality/interlace toward a user-set target
  (quality-priority ↔ fps-priority).
- **Gate**: socket ops are IPC calls into the `nwm` sysmodule; one 12 KB
  TCP send ≈ 1 IPC vs ~9 UDP sendto ≈ 9 IPCs. Before committing, benchmark
  on hardware: blast N MB via TCP vs 1400-byte UDP datagrams, compare CPU
  time per MB. If UDP overhead is unacceptable, fall back to TCP with
  strip skip + small send buffer (bounds staleness), which keeps most of
  the win.
