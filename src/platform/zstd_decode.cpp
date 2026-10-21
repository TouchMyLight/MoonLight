/// `platform::zstdInflateAll`/`zstdWorkspaceBytes`, decoding one complete zstd frame through the vendored decompressor.
///
/// @technical
///
/// `ZSTD_initStaticDCtx` places the context into the caller's own workspace, sized once per user like `zlibInflateAll`'s own. Nothing is allocated here: a module that frees its workspace holds none of this.
///
/// `zstdWorkspaceBytes()` measured 119,808 bytes (117KB) on an esp32p4rev1-eth build, far above `zlibWorkspaceBytes()`'s ~2.5KB: zstd's entropy tables cost real memory zlib's ROM state does not.
///
/// This is a generic zstd primitive, not FSEQ's: the caller hands over one complete, standard zstd frame (magic, header, blocks).
///
/// FSEQ's own compression blocks are headerless (FPP/xLights' writer omits the frame header). `FseqPlayerEffect` synthesizes one (`core/util/ZstdFrameHeader.h`) before calling here, so that quirk never reaches this file.
///
/// Shared between ESP32 and Teensy: the vendored decoder (`src/platform/vendor/zstddeclib.c`) is portable C with no chip dependency, unlike zlib's ROM-resident decoder, which is ESP32-only for real.
///
/// `CONFIG_MM_ZSTD` is off by default on both. ESP32's own Kconfig option sets it in the generated `sdkconfig.h`; Teensy has no Kconfig, so `teensy/CMakeLists.txt`'s own `MM_ZSTD` option defines the same macro directly.

#ifdef ESP_PLATFORM
#include "sdkconfig.h"   // CONFIG_MM_ZSTD, ESP32's own opt-in; Teensy defines it directly in teensy/CMakeLists.txt
#endif

#if defined(CONFIG_MM_ZSTD)

#include "platform/platform.h"

#include <cstring>

// zstddeclib.c (its own C translation unit, per each platform's own CMakeLists.txt) declares no header of its own; these are its only callees, given C linkage and a hand-written declaration instead of pulling in the real zstd.h.
extern "C" {
typedef struct ZSTD_DCtx_s ZSTD_DCtx;
size_t ZSTD_estimateDCtxSize(void);
ZSTD_DCtx* ZSTD_initStaticDCtx(void* workspace, size_t workspaceSize);
size_t ZSTD_decompressDCtx(ZSTD_DCtx* dctx, void* dst, size_t dstCapacity, const void* src, size_t srcSize);
unsigned ZSTD_isError(size_t code);
}

namespace mm::platform {

size_t zstdWorkspaceBytes() { return ZSTD_estimateDCtxSize(); }

bool zstdInflateAll(void* workspace, const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstCap, size_t& outLen) {
    if (!workspace) return false;
    const size_t size = ZSTD_estimateDCtxSize();
    // ZSTD_initStaticDCtx only sets the fields a fresh decode needs; a reused workspace's leftover bytes from the
    // PREVIOUS decode (window/prefix tracking the static API does not re-zero) corrupted a second call in practice.
    // Re-zeroing first restores the same "freshly allocated" precondition every time, matching ScratchBuffer's own
    // one-time zero-on-first-allocation that masked this the first call always sees.
    std::memset(workspace, 0, size);
    ZSTD_DCtx* dctx = ZSTD_initStaticDCtx(workspace, size);
    if (!dctx) return false;
    const size_t n = ZSTD_decompressDCtx(dctx, dst, dstCap, src, srcLen);
    if (ZSTD_isError(n) || n != dstCap) return false;
    outLen = n;
    return true;
}

} // namespace mm::platform

#else // !CONFIG_MM_ZSTD

#include "platform/platform.h"

namespace mm::platform {
bool zstdInflateAll(void*, const uint8_t*, size_t, uint8_t*, size_t, size_t&) { return false; }
size_t zstdWorkspaceBytes() { return 0; }
} // namespace mm::platform

#endif // CONFIG_MM_ZSTD
