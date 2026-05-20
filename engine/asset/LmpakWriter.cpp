// SPDX-License-Identifier: MIT
// Psynder — .lmpak writer support: thin zstd compression helper used by
// tools/lm_pak (lane 24) and in-lane unit tests. The struct layout and
// hashing logic stay in the header so the cooker can use them too, but
// the actual zstd call needs the optional dep so it lives here.

#include "LmpakWriter.h"

#if __has_include(<zstd.h>)
#   include <zstd.h>
#   define PSYNDER_ASSET_HAS_ZSTD 1
#else
#   define PSYNDER_ASSET_HAS_ZSTD 0
#endif

namespace psynder::asset::lmpak {

bool zstd_available() noexcept {
    return PSYNDER_ASSET_HAS_ZSTD != 0;
}

bool zstd_compress(const u8* src, usize src_len, int level, std::vector<u8>& out) {
#if PSYNDER_ASSET_HAS_ZSTD
    usize bound = ZSTD_compressBound(src_len);
    out.assign(bound, 0);
    usize n = ZSTD_compress(out.data(), bound, src, src_len, level);
    if (ZSTD_isError(n)) {
        out.clear();
        return false;
    }
    out.resize(n);
    return true;
#else
    (void)src;
    (void)src_len;
    (void)level;
    out.clear();
    return false;
#endif
}

bool zstd_decompress(const u8* src, usize src_len, u8* dst, usize dst_capacity) noexcept {
#if PSYNDER_ASSET_HAS_ZSTD
    usize n = ZSTD_decompress(dst, dst_capacity, src, src_len);
    return !ZSTD_isError(n) && n == dst_capacity;
#else
    (void)src;
    (void)src_len;
    (void)dst;
    (void)dst_capacity;
    return false;
#endif
}

u64 zstd_frame_content_size(const u8* src, usize src_len) noexcept {
#if PSYNDER_ASSET_HAS_ZSTD
    unsigned long long n = ZSTD_getFrameContentSize(src, src_len);
    if (n == ZSTD_CONTENTSIZE_UNKNOWN || n == ZSTD_CONTENTSIZE_ERROR) return ~u64{0};
    return static_cast<u64>(n);
#else
    (void)src;
    (void)src_len;
    return ~u64{0};
#endif
}

}  // namespace psynder::asset::lmpak
