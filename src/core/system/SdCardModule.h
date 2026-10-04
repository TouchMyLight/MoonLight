#pragma once

#include "core/module/MoonModule.h"

#include <cstddef>
#include <cstdint>

namespace mm {

/// Browse and manage a board's microSD card: a folder tree with create, delete and edit.
///
/// The SD counterpart to FileManagerModule, which does the same job for the internal flash.
/// A separate module and a separate panel rather than a second root on FileManagerModule's own tree.
/// The two mounts are independent filesystems reached through independent HTTP routes (`/api/sddir` + `/api/sdfile` here, `/api/dir` + `/api/file` there).
/// Nothing about the already-shipped flash File Manager needed to grow a "which filesystem" parameter as a result.
/// @card SdCardModule.png
///
/// @moreinfo
///
/// ## Catalog-driven, not boot-wired
///
/// FileManagerModule is created unconditionally in main.cpp — every board has internal flash. Most boards have no SD slot, so this one is added only to a board's catalog entry (the Waveshare ESP32-P4-ETH's, in mooninstaller/deviceModels.json), the same way AudioService is.
///
/// ## The mount is attempted once, in setup()
///
/// `platform::hasSdCard` gates whether a mount is attempted at all.
/// It is false on every build without CONFIG_MM_P4_SD (every board but the P4-ETH, and every desktop build).
/// The status then reads "no SD hardware on this build" rather than a misleading mount failure.
/// Where the hardware exists, `platform::sdMount()` runs once in setup() (it powers GPIO45 first).
/// A missing or unreadable card shows as its own status rather than retried every tick — reinsert and reboot, the same contract FilesystemModule's LittleFS mount has.
///
/// Prior art: FileManagerModule, down to its control shape.
class SdCardModule : public MoonModule {
public:
    /// A Services child, same as AudioService: the catalog entry attaches it under "Services", which validates a child's role against what it accepts.
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Service; }
    /// Declare the view toggle and the usage gauges.
    void defineControls() override;
    /// Mount the card (if this board has one) and force the hidden toggle off.
    void setup() override;
    /// Refresh the usage gauge, which walks the card on a slow cadence.
    void tick1s() MM_NONBLOCKING override;

private:
    bool showHidden_ = false;         ///< reveal dot-prefixed entries
    bool mounted_ = false;            ///< true once platform::sdMount() has succeeded
    uint32_t usedBytes_ = 0;          ///< bytes used, refreshed on the tick
    uint32_t totalBytes_ = 0;         ///< the card's capacity, read once on mount
    uint8_t  secondsSinceScan_ = 0;   ///< paces the usage scan
};

} // namespace mm
