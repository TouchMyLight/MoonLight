/// @defgroup platform_esp32_sdmmc The microSD mount and its file seam
/// A second, independent mount (FAT over 4-bit SDMMC) alongside the internal LittleFS one, for boards that carry a card slot.
/// SdCardModule and its HTTP routes read like FileManagerModule's, just against sdXxx instead of fsXxx — the same shape as the LittleFS seam in platform_esp32_fs.cpp.
///
/// @moreinfo
///
/// ## Not SDMMC_SLOT_CONFIG_DEFAULT()
///
/// The IDF's own P4 default slot config fills `d4` with GPIO45 (an 8-bit-mode data line the driver ignores in 4-bit mode, per its own doc comment) — but on the Waveshare ESP32-P4-ETH, GPIO45 is wired to a SI2301CDS P-channel load switch gating the slot's VDD rail, not a data line.
/// The struct is built here field-by-field (clk/cmd/d0-d3/width only) rather than from that macro, so nothing here implies a data role for a pin this board uses for power.
///
/// ## The card's power switch is driven before the mount, not left to the slot config
///
/// GPIO45 is a plain GPIO output, set before `esp_vfs_fat_sdmmc_mount` and left alone after: the card needs power before it will answer CMD0, and the mount call has no slot for "also flip a GPIO first".
/// The enable polarity here (LOW) is the P-channel-high-side-switch-convention guess, not bench-confirmed on its own — see @xref{internal-pullups-on-cmdd0-d3|the pull-up flag below}, which was the actual fix for the mount timeout this polarity guess was first blamed for.
///
/// ## Internal pullups on CMD/D0-D3
///
/// `SDMMC_SLOT_FLAG_INTERNAL_PULLUP` — set here, matching Waveshare's own `09_sdmmc` reference example for this platform.
/// The board's own CMD/D0-D3 pull-ups are confirmed present (see the next section), so this flag is redundant in practice; left set anyway since it's harmless and matches the vendor reference this file otherwise follows.
///
/// ## Two separate power rails, both required
///
/// The timeout this file spent longest on (`sdmmc_init_ocr: send_op_cond`, ESP_ERR_TIMEOUT) turned out to have nothing to do with GPIO45 or the pull-up flag above — six bench variants across both made no difference.
/// The actual fix, found by building and flashing Waveshare's own `09_sdmmc` example standalone: the board's CMD/D0-D3 pull-ups are fed from `ESP_LDO_VO4`, the SoC's own on-chip LDO channel 4.
/// That LDO feeds the `VDDPST_5` IO power domain those pins live in, which is off by default and has nothing to do with the card's own power.
/// `sd_pwr_ctrl_new_on_chip_ldo()` turns it on, matching `SOC_SDMMC_IO_POWER_EXTERNAL`'s own doc comment.
/// GPIO45/the SI2301CDS switch is the card's VDD rail, a separate concern — both rails are driven here, since nothing bench-tested yet shows GPIO45 is dead weight, only that it was never the cause of this particular timeout.
///
/// ## cd/wp: explicit "none", not zero-initialized
///
/// Once mounted, every write failed identically (`ESP_ERR_INVALID_STATE`, mount/read/list all fine) — not a DMA or silicon issue, despite how that error reads.
/// `sd_host_sdmmc.c`'s `sd_host_slot_start_command()` returns exactly that code for any write whose card-detect read says "write protected", and this struct's zero-initialized `.cd`/`.wp` leave both at GPIO0 rather than `SDMMC_SLOT_NO_CD`/`SDMMC_SLOT_NO_WP` (`GPIO_NUM_NC`) — this slot has neither signal wired.
/// `SDMMC_SLOT_CONFIG_DEFAULT()` sets both to "none" for exactly this reason; building the struct field-by-field means setting them explicitly instead.

#include "platform/platform.h"

#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if SOC_SDMMC_HOST_SUPPORTED && defined(CONFIG_MM_P4_SD)
#define MM_HAS_SDMMC 1
#endif

#if MM_HAS_SDMMC

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#if SOC_SDMMC_IO_POWER_EXTERNAL
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mm::platform {

namespace {

const char* SD_TAG = "mm_sdmmc";
constexpr const char* SD_MOUNT_POINT = "/sdcard";
constexpr gpio_num_t SD_PWR_GPIO = GPIO_NUM_45;

sdmmc_card_t* g_card = nullptr;
bool g_mounted = false;
#if SOC_SDMMC_IO_POWER_EXTERNAL
sd_pwr_ctrl_handle_t g_pwrCtrl = nullptr;
#endif

/// Map an API path onto the mount point; false on truncation, leaving nothing partial to consume.
bool sdTranslate(const char* apiPath, char* out, size_t outLen) {
    if (outLen == 0) return false;
    if (!apiPath) { out[0] = 0; return false; }
    const char* sep = (apiPath[0] == '/') ? "" : "/";
    int n = std::snprintf(out, outLen, "%s%s%s", SD_MOUNT_POINT, sep, apiPath);
    if (n < 0 || static_cast<size_t>(n) >= outLen) { out[0] = 0; return false; }
    return true;
}

}  // namespace

bool sdMount() {
    if (g_mounted) return true;

    // Power the slot before anything talks to it: see @xref{the-cards-power-switch-is-driven-before-the-mount-not-left-to-the-slot-config|the polarity caveat}.
    gpio_config_t pwrCfg = {};
    pwrCfg.pin_bit_mask = 1ULL << SD_PWR_GPIO;
    pwrCfg.mode = GPIO_MODE_OUTPUT;
    gpio_config(&pwrCfg);
    gpio_set_level(SD_PWR_GPIO, 0);   // LOW = enabled: the working guess, see the file doc

    esp_vfs_fat_sdmmc_mount_config_t mountCfg = {};
    mountCfg.format_if_mount_failed = false;   // a blank/foreign card stays untouched, not reformatted
    mountCfg.max_files = 5;
    mountCfg.allocation_unit_size = 16 * 1024;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

#if SOC_SDMMC_IO_POWER_EXTERNAL
    // Powers ESP_LDO_VO4 / VDDPST_5, the CMD/D0-D3 pull-up rail: @xref{two-separate-power-rails-both-required|why}.
    sd_pwr_ctrl_ldo_config_t ldoCfg = {};
    ldoCfg.ldo_chan_id = 4;
    esp_err_t ldoErr = sd_pwr_ctrl_new_on_chip_ldo(&ldoCfg, &g_pwrCtrl);
    if (ldoErr != ESP_OK) {
        ESP_LOGE(SD_TAG, "SD LDO power control failed: %s", esp_err_to_name(ldoErr));
        gpio_set_level(SD_PWR_GPIO, 1);
        return false;
    }
    host.pwr_ctrl_handle = g_pwrCtrl;
#endif

    // Field-by-field, not SDMMC_SLOT_CONFIG_DEFAULT(): see the file doc's "Not SDMMC_SLOT_CONFIG_DEFAULT()" section for why.
    sdmmc_slot_config_t slotCfg = {};
    slotCfg.clk = GPIO_NUM_43;
    slotCfg.cmd = GPIO_NUM_44;
    slotCfg.d0  = GPIO_NUM_39;
    slotCfg.d1  = GPIO_NUM_40;
    slotCfg.d2  = GPIO_NUM_41;
    slotCfg.d3  = GPIO_NUM_42;
    slotCfg.width = 4;
    // Zero-initialized leaves these at GPIO0/GPIO0, not "none": the driver then reads GPIO0 as a real write-protect line and rejects every write with ESP_ERR_INVALID_STATE, having nothing to do with the card, the DMA, or the LDO rails above.
    // The slot has neither signal wired.
    slotCfg.cd = SDMMC_SLOT_NO_CD;
    slotCfg.wp = SDMMC_SLOT_NO_WP;
    // See @xref{internal-pullups-on-cmdd0-d3|the file doc} for why this is set.
    slotCfg.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slotCfg, &mountCfg, &g_card);
    if (err != ESP_OK) {
        ESP_LOGE(SD_TAG, "SD mount failed: %s", esp_err_to_name(err));
#if SOC_SDMMC_IO_POWER_EXTERNAL
        sd_pwr_ctrl_del_on_chip_ldo(g_pwrCtrl);
        g_pwrCtrl = nullptr;
#endif
        gpio_set_level(SD_PWR_GPIO, 1);   // power back down on a failed mount
        g_card = nullptr;
        return false;
    }

    g_mounted = true;
    ESP_LOGI(SD_TAG, "SD card mounted at %s", SD_MOUNT_POINT);
    return true;
}

void sdUnmount() {
    if (!g_mounted) return;
    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, g_card);
#if SOC_SDMMC_IO_POWER_EXTERNAL
    sd_pwr_ctrl_del_on_chip_ldo(g_pwrCtrl);
    g_pwrCtrl = nullptr;
#endif
    gpio_set_level(SD_PWR_GPIO, 1);   // power down with the mount
    g_card = nullptr;
    g_mounted = false;
}

bool sdMkdir(const char* path) {
    if (!g_mounted) return false;
    char full[128];
    if (!sdTranslate(path, full, sizeof(full))) return false;
    char* p = full + std::strlen(SD_MOUNT_POINT) + 1;
    while (*p) {
        if (*p == '/') {
            *p = 0;
            mkdir(full, 0775);  // ignore errors; could already exist
            *p = '/';
        }
        p++;
    }
    int rc = mkdir(full, 0775);
    return rc == 0 || errno == EEXIST;
}

bool sdExists(const char* path) {
    if (!g_mounted) return false;
    char full[128];
    if (!sdTranslate(path, full, sizeof(full))) return false;
    struct stat st;
    return stat(full, &st) == 0;
}

bool sdRemove(const char* path) {
    if (!g_mounted) return false;
    char full[128];
    if (!sdTranslate(path, full, sizeof(full))) return false;
    struct stat st;
    if (::stat(full, &st) == 0 && S_ISDIR(st.st_mode)) return ::rmdir(full) == 0;
    return ::remove(full) == 0;
}

int sdRead(const char* path, char* buf, size_t maxLen) {
    if (!g_mounted || !buf || maxLen == 0) return -1;
    char full[128];
    if (!sdTranslate(path, full, sizeof(full))) return -1;
    FILE* f = std::fopen(full, "rb");
    if (!f) return -1;
    size_t n = std::fread(buf, 1, maxLen - 1, f);
    std::fclose(f);
    buf[n] = 0;
    return static_cast<int>(n);
}

bool sdWriteAtomic(const char* path, const char* data, size_t len) {
    if (!g_mounted) return false;
    if (len > 0 && !data) return false;
    char full[128];
    char tmp[136];
    if (!sdTranslate(path, full, sizeof(full))) return false;
    int n = std::snprintf(tmp, sizeof(tmp), "%s.tmp", full);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(tmp)) return false;

    FILE* f = std::fopen(tmp, "wb");
    if (!f) return false;
    size_t written = std::fwrite(data, 1, len, f);
    if (written != len) {
        std::fclose(f);
        ::remove(tmp);
        return false;
    }
    std::fflush(f);
    int fd = ::fileno(f);
    if (fd >= 0) ::fsync(fd);
    std::fclose(f);

    if (::rename(tmp, full) != 0) {
        ::remove(tmp);
        return false;
    }
    return true;
}

int sdReadAt(const char* path, long offset, char* buf, size_t len) {
    if (!g_mounted || !buf) return -1;
    char full[128];
    if (!sdTranslate(path, full, sizeof(full))) return -1;
    FILE* f = std::fopen(full, "rb");
    if (!f) return -1;
    if (std::fseek(f, offset, SEEK_SET) != 0) { std::fclose(f); return -1; }
    const size_t n = std::fread(buf, 1, len, f);
    std::fclose(f);
    return static_cast<int>(n);
}

long sdSize(const char* path) {
    if (!g_mounted) return -1;
    char full[128];
    if (!sdTranslate(path, full, sizeof(full))) return -1;
    struct stat st;
    if (::stat(full, &st) != 0 || S_ISDIR(st.st_mode)) return -1;
    return static_cast<long>(st.st_size);
}

bool sdWriteStream(const char* path, FsWriteSrc src, void* user) {
    if (!g_mounted || !src) return false;
    char full[128];
    char tmp[136];
    if (!sdTranslate(path, full, sizeof(full))) return false;
    int n = std::snprintf(tmp, sizeof(tmp), "%s.tmp", full);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(tmp)) return false;

    FILE* f = std::fopen(tmp, "wb");
    if (!f) return false;
    char chunk[1024];
    bool ok = true, abort = false;
    for (;;) {
        // A large SD write runs long enough to trip ESP-IDF's task watchdog on its own (the bench found this at ~10s into a multi-MB transfer; see HttpServerModule.cpp's "Why SD uploads get no hard ceiling").
        platform::feedWatchdog();
        const size_t got = src(chunk, sizeof(chunk), user, &abort);
        if (abort) { ok = false; break; }
        if (got == 0) break;
        if (std::fwrite(chunk, 1, got, f) != got) { ok = false; break; }
    }
    std::fflush(f);
    int fd = ::fileno(f);
    if (fd >= 0) ::fsync(fd);
    std::fclose(f);
    if (!ok || ::rename(tmp, full) != 0) { ::remove(tmp); return false; }
    return true;
}

void sdList(const char* dir, FsListCb cb, void* user) {
    if (!g_mounted || !cb) return;
    char full[128];
    if (!sdTranslate(dir, full, sizeof(full))) return;
    DIR* d = ::opendir(full);
    if (!d) return;
    struct dirent* ent;
    char childPath[400];
    struct stat st;
    while ((ent = ::readdir(d)) != nullptr) {
        std::snprintf(childPath, sizeof(childPath), "%s/%s", full, ent->d_name);
        const bool statOk = stat(childPath, &st) == 0;
        const bool isDir = statOk && S_ISDIR(st.st_mode);
        const uint32_t size = (statOk && !isDir) ? static_cast<uint32_t>(st.st_size) : 0;
        cb(ent->d_name, isDir, size, user);
    }
    ::closedir(d);
}

size_t sdUsed() {
    if (!g_mounted || !g_card) return 0;
    FATFS* fs = nullptr;
    DWORD freeClusters = 0;
    if (f_getfree("0:", &freeClusters, &fs) != FR_OK || !fs) return 0;
    const uint64_t totalSectors = (static_cast<uint64_t>(fs->n_fatent) - 2) * fs->csize;
    const uint64_t freeSectors = static_cast<uint64_t>(freeClusters) * fs->csize;
    return static_cast<size_t>((totalSectors - freeSectors) * fs->ssize);
}

size_t sdTotal() {
    if (!g_mounted || !g_card) return 0;
    return static_cast<size_t>(static_cast<uint64_t>(g_card->csd.capacity) * g_card->csd.sector_size);
}

}  // namespace mm::platform

#else  // !MM_HAS_SDMMC — no SD slot on this target: inert stub.

namespace mm::platform {
bool sdMount() { return false; }
void sdUnmount() {}
bool sdMkdir(const char*) { return false; }
bool sdExists(const char*) { return false; }
bool sdRemove(const char*) { return false; }
int  sdRead(const char*, char*, size_t) { return -1; }
long sdSize(const char*) { return -1; }
int  sdReadAt(const char*, long, char*, size_t) { return -1; }
bool sdWriteAtomic(const char*, const char*, size_t) { return false; }
bool sdWriteStream(const char*, FsWriteSrc, void*) { return false; }
void sdList(const char*, FsListCb, void*) {}
size_t sdUsed() { return 0; }
size_t sdTotal() { return 0; }
}  // namespace mm::platform

#endif  // MM_HAS_SDMMC
