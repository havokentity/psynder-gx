// SPDX-License-Identifier: MIT OR Apache-2.0
// Psynder-GX — .psydem demo reader impl. Lane 18 (net).
//
// Reads a file produced by DemoWriter (see DemoWriter.cpp + DemoFormat.h).
//
// Open flow:
//   1. fopen(path, "rb"); check.
//   2. fseek to EOF - sizeof(DemoTocFooter); read footer; validate magic.
//   3. Read TOC array at footer.toc_offset.
//   4. fseek to 0; read DemoFileHeader; validate magic + version.
//   5. Read roster.
//   6. Seek to header.first_record_offset.
//
// advance_tick gathers all records whose `rhdr.tick == current_tick_ + 1`
// (or whatever the next encountered tick is) and applies them. Frames are
// reconstructed via XOR-delta against the running baseline buffer; the
// baseline is overwritten on every keyframe encountered.

#include "DemoReader.h"

#include "SnapshotDelta.h"

#include <cstdio>
#include <cstring>
#include <utility>

namespace psynder::net {

namespace {

PSY_FORCEINLINE std::FILE* as_file(void* p) noexcept {
    return static_cast<std::FILE*>(p);
}

}  // namespace

DemoReader::~DemoReader() noexcept {
    if (open_) close();
}

DemoReader::DemoReader(DemoReader&& o) noexcept
    : file_(o.file_)
    , open_(o.open_)
    , current_tick_(o.current_tick_)
    , header_(o.header_)
    , roster_(std::move(o.roster_))
    , toc_(std::move(o.toc_))
    , tick_cfg_(o.tick_cfg_)
    , baseline_(std::move(o.baseline_))
    , baseline_tick_(o.baseline_tick_)
    , records_end_offset_(o.records_end_offset_)
{
    o.file_         = nullptr;
    o.open_         = false;
    o.current_tick_ = 0;
    o.records_end_offset_ = 0;
}

DemoReader& DemoReader::operator=(DemoReader&& o) noexcept {
    if (this != &o) {
        if (open_) close();
        file_         = o.file_;
        open_         = o.open_;
        current_tick_ = o.current_tick_;
        header_       = o.header_;
        roster_       = std::move(o.roster_);
        toc_          = std::move(o.toc_);
        tick_cfg_     = o.tick_cfg_;
        baseline_     = std::move(o.baseline_);
        baseline_tick_ = o.baseline_tick_;
        records_end_offset_ = o.records_end_offset_;
        o.file_         = nullptr;
        o.open_         = false;
        o.current_tick_ = 0;
        o.records_end_offset_ = 0;
    }
    return *this;
}

DemoReaderResult DemoReader::open(const char* path) noexcept {
    if (!path) return DemoReaderResult::IoError;
    if (open_) close();
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return DemoReaderResult::IoError;

    // Footer.
    if (std::fseek(f, -static_cast<long>(sizeof(DemoTocFooter)), SEEK_END) != 0) {
        std::fclose(f);
        return DemoReaderResult::CorruptFile;
    }
    DemoTocFooter footer{};
    if (std::fread(&footer, sizeof(footer), 1, f) != 1) {
        std::fclose(f);
        return DemoReaderResult::CorruptFile;
    }
    if (footer.footer_magic != kDemoMagic) {
        std::fclose(f);
        return DemoReaderResult::CorruptFile;
    }

    // TOC.
    toc_.clear();
    if (footer.toc_count > 0) {
        toc_.resize(footer.toc_count);
        if (std::fseek(f, static_cast<long>(footer.toc_offset), SEEK_SET) != 0
            || std::fread(toc_.data(), sizeof(DemoTocEntry), footer.toc_count, f)
               != footer.toc_count) {
            std::fclose(f);
            return DemoReaderResult::CorruptFile;
        }
    }

    // Header.
    if (std::fseek(f, 0, SEEK_SET) != 0) {
        std::fclose(f);
        return DemoReaderResult::CorruptFile;
    }
    if (std::fread(&header_, sizeof(header_), 1, f) != 1) {
        std::fclose(f);
        return DemoReaderResult::CorruptFile;
    }
    if (header_.magic != kDemoMagic) {
        std::fclose(f);
        return DemoReaderResult::CorruptFile;
    }
    if (header_.demo_version != kDemoVersion) {
        std::fclose(f);
        return DemoReaderResult::VersionMismatch;
    }

    // Roster.
    roster_.clear();
    if (header_.player_count > 0) {
        roster_.resize(header_.player_count);
        if (std::fread(roster_.data(), sizeof(DemoRosterEntry),
                       header_.player_count, f) != header_.player_count) {
            std::fclose(f);
            return DemoReaderResult::CorruptFile;
        }
    }

    // Seek to first record.
    if (std::fseek(f, static_cast<long>(header_.first_record_offset), SEEK_SET) != 0) {
        std::fclose(f);
        return DemoReaderResult::CorruptFile;
    }

    // Pick tick config from header.
    tick_cfg_ = (header_.tick_hz == 128)
                ? tick_config_128()
                : tick_config_64();

    file_         = f;
    open_         = true;
    current_tick_ = (header_.start_tick > 0) ? header_.start_tick - 1 : 0;
    baseline_.clear();
    baseline_tick_ = 0;
    records_end_offset_ = footer.toc_offset;
    return DemoReaderResult::Ok;
}

DemoReaderResult DemoReader::advance_tick(DemoTickData& out) noexcept {
    if (!open_ || !file_) return DemoReaderResult::IoError;
    std::FILE* f = as_file(file_);

    out.tick           = 0;
    out.has_frame      = false;
    out.is_keyframe    = false;
    out.snapshot_bytes.clear();
    out.inputs.clear();

    u32 active_tick = 0;
    bool saw_record = false;

    while (true) {
        // TOC region begins at records_end_offset_. Stop before entering it.
        const long pos = std::ftell(f);
        if (records_end_offset_ != 0
            && pos >= 0
            && static_cast<u64>(pos) >= records_end_offset_) {
            if (saw_record) {
                current_tick_ = active_tick;
                out.tick      = active_tick;
                return DemoReaderResult::Ok;
            }
            return DemoReaderResult::EndOfFile;
        }

        DemoRecordHeader rhdr{};
        const usize got = std::fread(&rhdr, sizeof(rhdr), 1, f);
        if (got != 1) {
            // EOF reached without the records_end_offset_ guard (e.g. the
            // demo was truncated). Fall back to returning what we have.
            if (saw_record) {
                current_tick_ = active_tick;
                out.tick      = active_tick;
                return DemoReaderResult::Ok;
            }
            return DemoReaderResult::EndOfFile;
        }
        // Validate record type — if we've drifted into a corrupt region
        // bail.
        if (rhdr.type != kRecordTypeFrame
            && rhdr.type != kRecordTypeInput
            && rhdr.type != kRecordTypeEvent) {
            std::fseek(f, pos, SEEK_SET);
            if (saw_record) {
                current_tick_ = active_tick;
                out.tick      = active_tick;
                return DemoReaderResult::Ok;
            }
            return DemoReaderResult::EndOfFile;
        }

        if (!saw_record) {
            active_tick = rhdr.tick;
        } else if (rhdr.tick != active_tick) {
            // We just stepped into the NEXT tick. Rewind to its header so
            // the following advance_tick() picks it up.
            std::fseek(f, pos, SEEK_SET);
            current_tick_ = active_tick;
            out.tick      = active_tick;
            return DemoReaderResult::Ok;
        }
        saw_record = true;

        if (rhdr.type == kRecordTypeFrame) {
            DemoFrameRecordPreamble pre{};
            if (std::fread(&pre, sizeof(pre), 1, f) != 1) return DemoReaderResult::CorruptFile;
            if (rhdr.payload_len < sizeof(pre)) return DemoReaderResult::CorruptFile;
            const usize payload_bytes =
                static_cast<usize>(rhdr.payload_len) - sizeof(pre);
            std::vector<u8> delta(payload_bytes);
            if (payload_bytes > 0
                && std::fread(delta.data(), 1, payload_bytes, f) != payload_bytes) {
                return DemoReaderResult::CorruptFile;
            }
            if (pre.baseline_tick == rhdr.tick) {
                // Keyframe: payload is the raw snapshot.
                baseline_       = std::move(delta);
                baseline_tick_  = rhdr.tick;
                out.snapshot_bytes = baseline_;
                out.is_keyframe = true;
            } else {
                if (baseline_.empty()) return DemoReaderResult::CorruptFile;
                if (!xor_delta_decode(std::span<const u8>(baseline_.data(), baseline_.size()),
                                      std::span<const u8>(delta.data(), delta.size()),
                                      out.snapshot_bytes)) {
                    return DemoReaderResult::CorruptFile;
                }
            }
            out.has_frame = true;
        } else if (rhdr.type == kRecordTypeInput) {
            DemoInputPreamble pre{};
            if (std::fread(&pre, sizeof(pre), 1, f) != 1) return DemoReaderResult::CorruptFile;
            const usize entries = pre.input_count;
            out.inputs.resize(entries);
            if (entries > 0
                && std::fread(out.inputs.data(), sizeof(PlayerInputEntry), entries, f) != entries) {
                return DemoReaderResult::CorruptFile;
            }
        } else {
            // Skip unknown / reserved.
            std::fseek(f, rhdr.payload_len, SEEK_CUR);
        }
    }
}

DemoReaderResult DemoReader::seek_to_tick(u32 tick) noexcept {
    if (!open_ || !file_) return DemoReaderResult::IoError;
    if (toc_.empty()) return DemoReaderResult::CorruptFile;
    if (tick < header_.start_tick || tick > header_.end_tick) {
        return DemoReaderResult::CorruptFile;
    }
    // Binary-search for the largest TOC entry with tick <= requested.
    u32 lo = 0, hi = static_cast<u32>(toc_.size()) - 1u;
    u32 best = 0;
    bool found = false;
    while (lo <= hi) {
        const u32 mid = (lo + hi) / 2u;
        if (toc_[mid].tick <= tick) {
            best  = mid;
            found = true;
            if (mid == toc_.size() - 1u) break;
            lo = mid + 1;
        } else {
            if (mid == 0) break;
            hi = mid - 1;
        }
    }
    if (!found) return DemoReaderResult::CorruptFile;

    if (std::fseek(as_file(file_),
                   static_cast<long>(toc_[best].file_offset),
                   SEEK_SET) != 0) {
        return DemoReaderResult::IoError;
    }
    // The TOC always points at a keyframe — reset the baseline so the
    // next read reconstructs cleanly.
    baseline_.clear();
    baseline_tick_ = 0;
    current_tick_  = (toc_[best].tick > 0) ? toc_[best].tick - 1u : 0u;
    return DemoReaderResult::Ok;
}

void DemoReader::close() noexcept {
    if (file_) std::fclose(as_file(file_));
    file_         = nullptr;
    open_         = false;
    current_tick_ = 0;
    roster_.clear();
    toc_.clear();
    baseline_.clear();
    baseline_tick_ = 0;
}

}  // namespace psynder::net
