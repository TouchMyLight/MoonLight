/// @defgroup sd_card_impl SD Card implementation
/// The device's microSD browser, whose surface is the UI tree panel rather than a control list.
/// Public surface and class layout live in SdCardModule.h.
/// @{
#include "core/system/SdCardModule.h"

#include "platform/platform.h"       // sd* primitives + hasSdCard

namespace mm {

void SdCardModule::defineControls() {
    // Same shape as FileManagerModule: `show hidden` is the one real control, the tree panel (app.js renderSdCardManager) does the rest over /api/sddir.
    controls_.addControl("show hidden", showHidden_);
    controls_.setHidden(controls_.count() - 1, true);
    // Usage gauge, bound only once mounted (an absent/unreadable card has nothing to show).
    if (mounted_ && totalBytes_ > 0) {
        controls_.addProgress("card", usedBytes_, totalBytes_);
        controls_.setHidden(controls_.count() - 1, true);
    }
    MoonModule::defineControls();
}

void SdCardModule::setup() {
    MoonModule::setup();
    showHidden_ = false;   // a transient view preference, forced off on every boot, same as the flash panel

    if constexpr (!platform::hasSdCard) {
        setStatus("no SD hardware on this build", Severity::Status);
        return;
    }
    mounted_ = platform::sdMount();
    if (!mounted_) {
        setStatus("mount failed — check the card is inserted and formatted FAT32", Severity::Error);
        return;
    }
    totalBytes_ = static_cast<uint32_t>(platform::sdTotal());
    usedBytes_ = static_cast<uint32_t>(platform::sdUsed());
    clearStatus();
    // Scheduler::setup() runs defineControls() for every module before any module's setup(), so the usage gauge's "mounted_ && totalBytes_ > 0" gate above evaluated false on that first pass (the mount is what just changed it). One rebuild picks it up — the same pattern FilesystemModule::applyNode uses after a value-dependent schema change.
    rebuildControls();
}

void SdCardModule::tick1s() MM_NONBLOCKING {
    // Once a minute, not once a second, the same reasoning as FileManagerModule's own scan: a block-walking usage scan is not a per-tick cost.
    if (!mounted_) return;
    if (++secondsSinceScan_ < 60) return;
    secondsSinceScan_ = 0;
    usedBytes_ = static_cast<uint32_t>(platform::sdUsed());
}

// mkdir/delete are HTTP endpoints (POST/DELETE /api/sddir?path=) in HttpServerModule, mirroring the flash File Manager's /api/dir — this module holds no op state and writes nothing persisted.

} // namespace mm

/// @}
