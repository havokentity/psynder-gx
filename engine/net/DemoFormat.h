// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — .psydem demo file format definitions. Lane 18 (net).
//
// DESIGN-PSYNDER-GX.md §10.4 (Demo / replay recording):
//
//   ".psydem format. Records all inputs + snapshots. Replays deterministically.
//    Supports fast-forward, rewind, free-camera.
//
//    - Header: server version, map id, player roster, tick rate.
//    - Frame entries: tick number + delta-compressed snapshot.
//    - Input entries: per-tick player inputs for replay reconstruction.
//    - Indexable: seek to any frame in O(log n) via a TOC at the end of the file."
//
// File layout (all values little-endian on wire):
//
//   ┌────────────────────────────────────────────────────────────┐
//   │  DemoFileHeader  (64 bytes, fixed)                         │
//   ├────────────────────────────────────────────────────────────┤
//   │  DemoRecord* …   (variable; records are tagged by type)    │
//   │   - kRecordTypeFrame  → DemoFrameRecord                    │
//   │   - kRecordTypeInput  → DemoInputRecord                    │
//   │   - kRecordTypeEvent  → DemoEventRecord (reserved)         │
//   ├────────────────────────────────────────────────────────────┤
//   │  DemoTocEntry[toc_count]  (8 bytes each)                   │
//   │  DemoTocFooter            (16 bytes, fixed — at file end)  │
//   └────────────────────────────────────────────────────────────┘
//
// TOC strategy: the writer appends records sequentially. When the demo is
// finalised, the writer serialises a TOC array (one entry per recorded tick)
// followed by a fixed footer containing the TOC offset and count. Readers
// parse the footer first, then binary-search the TOC for O(log n) tick seeks.
//
// Delta compression: snapshot frames carry a delta against the last acked
// baseline. The delta encoding is XOR-style at the byte level (DESIGN §10.4:
// "XOR-style delta against the last acked baseline"). A frame with
// `baseline_tick == tick` is a keyframe (no delta; full snapshot data follows).
//
// Sub-tick input (DESIGN §10.4):
//   Per-tick player inputs include a 16-bit sub-tick fraction for each
//   firing event, so the replay can reconstruct the exact aim ray used in
//   lag compensation. See also TickConfig.h (kSubTickFractionBits = 16).
//
// Compile flag: this TU MUST be compiled with
//   -fno-fast-math -ffp-contract=off
// per DESIGN-PSYNDER-GX.md §14.

#pragma once

#include "TickConfig.h"
#include "core/Types.h"

#include <span>

namespace psynder::net {

// ──────────────────────────────────────────────────────────────────────────────
// Magic + version
// ──────────────────────────────────────────────────────────────────────────────
inline constexpr u32 kDemoMagic    = 0x50534444u;  // 'PSDD'
inline constexpr u16 kDemoVersion  = 0x0100u;      // 1.0 — bumped on breaking changes.

// ──────────────────────────────────────────────────────────────────────────────
// File header — 64 bytes, written once at offset 0.
// ──────────────────────────────────────────────────────────────────────────────
struct DemoFileHeader {
    u32  magic           = kDemoMagic;    //  0  4  'PSDD'
    u16  demo_version    = kDemoVersion;  //  4  2  format version
    u8   tick_hz         = 64;            //  6  1  matches TickConfig::tick_hz
    u8   reserved0       = 0;            //  7  1  padding / future flags
    u32  map_id          = 0;            //  8  4  hashed map asset id
    u32  server_build    = 0;            // 12  4  server binary build number
    u64  match_start_ns  = 0;            // 16  8  wall-clock UNIX time (ns) at tick 0
    u32  start_tick      = 0;            // 24  4  first recorded tick number
    u32  end_tick        = 0;            // 28  4  last recorded tick number (0 = unfinished)
    u8   player_count    = 0;            // 32  1  players in roster (max 64)
    u8   reserved1[3]    = {};           // 33  3
    //    player_roster follows immediately: player_count × 16-byte entries.
    //    Each entry: u64 steam_id (or 0 for bots) + u32 entity_id + u32 name_crc32.
    //    Roster is in DemoFileHeader::player_roster[] rather than inline here to
    //    keep the fixed header compact; the full header extent is
    //    sizeof(DemoFileHeader) + player_count * 16.
    u32  roster_offset   = 0;           // 36  4  byte offset of roster block (= 64)
    u32  first_record_offset = 0;       // 40  4  byte offset of first DemoRecord
    u32  reserved2       = 0;           // 44  4
    u64  reserved3       = 0;           // 48  8
    u64  reserved4       = 0;           // 56  8
    //                                  // 64 bytes total
};
static_assert(sizeof(DemoFileHeader) == 64, "DemoFileHeader must be 64 bytes");

// ──────────────────────────────────────────────────────────────────────────────
// Player roster entry — 16 bytes per slot.
// ──────────────────────────────────────────────────────────────────────────────
struct DemoRosterEntry {
    u64 steam_id   = 0;   // 0 for bots / offline players
    u32 entity_id  = 0;   // ECS entity id at match start
    u32 name_crc32 = 0;   // CRC32 of player name string (locale: UTF-8)
};
static_assert(sizeof(DemoRosterEntry) == 16, "DemoRosterEntry must be 16 bytes");

// ──────────────────────────────────────────────────────────────────────────────
// Record tags
// ──────────────────────────────────────────────────────────────────────────────
enum DemoRecordType : u8 {
    kRecordTypeFrame  = 0x01,  // DemoFrameRecord (snapshot delta)
    kRecordTypeInput  = 0x02,  // DemoInputRecord (player inputs for one tick)
    kRecordTypeEvent  = 0x03,  // DemoEventRecord (kills, round changes, etc. — reserved)
};

// Common record header — 8 bytes. Precedes every record.
struct DemoRecordHeader {
    u8   type         = 0;    // DemoRecordType
    u8   reserved0    = 0;
    u16  payload_len  = 0;    // bytes following this header (NOT including header)
    u32  tick         = 0;    // server tick this record belongs to
};
static_assert(sizeof(DemoRecordHeader) == 8, "DemoRecordHeader must be 8 bytes");

// ──────────────────────────────────────────────────────────────────────────────
// Frame record — snapshot delta for one tick.
//
// Payload layout (after DemoRecordHeader):
//   u32  baseline_tick  — tick whose snapshot was used as the XOR base.
//                         Equal to `tick` for a keyframe (no delta, full data).
//   u32  entity_count   — number of entity records that follow.
//   u8   delta_data[]   — XOR-delta payload; length = payload_len - 8.
//
// The receiver XORs delta_data against the baseline snapshot buffer to recover
// the full snapshot for this tick. Keyframes (baseline_tick == tick) carry raw
// (non-XOR'd) snapshot bytes.
// ──────────────────────────────────────────────────────────────────────────────
struct DemoFrameRecordPreamble {
    u32 baseline_tick = 0;
    u32 entity_count  = 0;
    // delta_data follows inline in the payload.
};

// ──────────────────────────────────────────────────────────────────────────────
// Input record — per-tick player inputs for ALL players (relay format).
//
// Payload layout (after DemoRecordHeader):
//   u8   input_count    — number of PlayerInputEntry records that follow.
//   u8   reserved[3]
//   PlayerInputEntry[]  — one per active player this tick.
// ──────────────────────────────────────────────────────────────────────────────
struct DemoInputPreamble {
    u8  input_count  = 0;
    u8  reserved[3]  = {};
};

// Per-player input for one tick.
// Sub-tick aim (DESIGN §10.4 sub-tick):
//   `aim_frac_u16` encodes the client-reported aim fraction within the tick as
//   a 16-bit value. 0x0000 = start of tick; 0xFFFF = just before the next tick
//   boundary. The replay system uses this to reconstruct the exact aim ray for
//   sub-tick-accurate lag compensation validation.
struct PlayerInputEntry {
    u32  entity_id      = 0;   // identifies the player
    u16  aim_frac_u16   = 0;   // sub-tick aim fraction (DESIGN §10.4)
    u16  button_mask    = 0;   // W/A/S/D, jump, crouch, fire, zoom, reload
    i16  pitch_delta    = 0;   // mouse delta Y (scaled; not absolute aim)
    i16  yaw_delta      = 0;   // mouse delta X
    u8   weapon_slot    = 0;   // selected weapon slot
    u8   reserved[3]    = {};
};
static_assert(sizeof(PlayerInputEntry) == 16, "PlayerInputEntry must be 16 bytes");

// ──────────────────────────────────────────────────────────────────────────────
// TOC — written at the end of the file for O(log n) seeks.
// ──────────────────────────────────────────────────────────────────────────────

// One entry per keyframe tick in the demo.
// Consumers binary-search by `tick`, then fseek to `file_offset` and scan
// forward (records are small) to reach any tick within one keyframe interval.
struct DemoTocEntry {
    u32  tick        = 0;   // tick number of this keyframe
    u32  file_offset = 0;   // byte offset of the DemoRecordHeader for this keyframe
};
static_assert(sizeof(DemoTocEntry) == 8, "DemoTocEntry must be 8 bytes");

// Fixed footer — always at file end. Readers parse from EOF - sizeof(DemoTocFooter).
struct DemoTocFooter {
    u64  toc_offset   = 0;   // byte offset of the first DemoTocEntry
    u32  toc_count    = 0;   // number of DemoTocEntry records
    u32  footer_magic = kDemoMagic;  // sanity check ('PSDD')
};
static_assert(sizeof(DemoTocFooter) == 16, "DemoTocFooter must be 16 bytes");

}  // namespace psynder::net
