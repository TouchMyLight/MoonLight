#pragma once

#include "light/effects/EffectBase.h"

#include "light/util/FseqFile.h"     // the FSEQ header/sparse-range parser this effect plays back
#include "platform/platform.h"       // sdXxx + hasSdCard, and zlibInflateAll for a compressed track
#include "core/util/JsonSink.h"      // the playlist's own ListSource serialization
#include "core/util/JsonUtil.h"      // restoreList: recursive reader for the persisted playlist

#include <cstdio>
#include <cstring>

namespace mm {

/// Plays FSEQ animation files from the SD card onto this effect's layer, with a playlist and the standard transport controls.
/// @card FseqPlayerEffect.gif
///
/// FSEQ is the Falcon Player/xLights sequence format: a flat, per-frame byte stream with no pixel semantics of its own.
///
/// @moreinfo
///
/// ## Prior art
///
/// FPP's `FSEQ_Sequence_File_Format.txt` (github.com/FalconChristmas/fpp/blob/master/docs/FSEQ_Sequence_File_Format.txt) is the spec this plays against.
/// The playlist follows `LightPresetsModule`'s editable-list shape.
/// The transport state machine follows `DemoReelEffect`'s cycle/shuffle logic.
///
/// ## The fps slider replaces the file's own rate
///
/// The `fps` slider sets the playback rate directly: 1-120, default 30.
/// The file's own authored `stepTimeMs` is never read once this control exists, so there is no separate "native rate" mode to reach for.
///
/// ## Where animations live
///
/// Every `.fseq` file anywhere on the SD card is found by walking every folder from the root, bounded to `kMaxScanDepth` levels.
/// The scan runs on demand from the `rescan` button and once at `setup()`, not on every tick.
/// Each playlist row picks one of those files by its full path, which also disambiguates two files sharing a name in different folders.
///
/// ## Starts on its own
///
/// Becoming enabled — added to a Layer, re-enabled, or already enabled and persisted at boot — starts playback immediately if the playlist is non-empty, in `prepare()`.
/// There is no separate "press play after adding it" step.
/// `release()` forces a clean stop first, so a later re-enable never resumes into the frame/block buffers that release just freed.
///
/// ## tick(): stopped clears, paused holds
///
/// A stopped transport owns its background by going black every tick, rather than leaving whatever the previous effect painted.
/// A paused transport deliberately leaves the last frame on screen.
///
/// ## Self-gated, not catalog-gated
///
/// Unlike `SdCardModule`, this effect is registered unconditionally and user-added to a Layer like any other effect.
/// It calls `platform::sdMount()` itself in `setup()`; the call is idempotent, so a redundant mount is free.
///
/// ## Frame timing runs synchronous SD reads inside tick()
///
/// Each frame's bytes are one `platform::sdReadAt()` of exactly `channelCount` bytes.
/// A compressed track instead pays one decompression per compression block, larger but done once per block rather than once per frame.
/// This is in tension with the architecture's "no blocking in the hot path" rule, bounded to one frame (or block) of work per tick.
/// Verifying the real cost is a real-hardware tick-timing measurement, not something a desktop test can show.
///
/// ## Compression: zlib yes, zstd not yet
///
/// An FSEQ v2 file may compress each block of frames independently, which is what lets a block decompress without the ones before it.
/// Zlib (type 2) decodes through the ESP32 ROM's `tinfl_decompress` (`platform::zlibInflateAll`) at zero flash cost.
/// Zstd (type 1) has no decoder anywhere in this build, reported as its own status since the fix is a new dependency, not a bug.
///
/// ## The SD scan is an explicit queue, not recursion
///
/// Real call recursion through `platform::sdList()` blew the main task's stack on a card with nested folders.
/// Each level costs a directory handle, a dirent, a stat buffer and a path buffer.
/// The result was a "Stack protection fault" panic every boot, before networking even came up.
/// A found subdirectory is queued in `scanPending_` instead, descended into later from the flat `while` loop in `rescanSdFiles()`.
/// That queue is a member, not a local: this object is heap-allocated, so member storage costs nothing against the stack a local array would.
class FseqPlayerEffect : public EffectBase, public ListSource {
public:
    /// The transport's three states.
    enum class PlayState : uint8_t { Stopped, Playing, Paused };
    /// The three loopMode values, public so a test can select one by name.
    static constexpr uint8_t kLoopOff = 0, kLoopOne = 1, kLoopAll = 2;

    /// Catalog tags: a film reel, for a pre-rendered show rather than a generated one.
    const char* tags() const override { return "🎞️"; }

    /// Bind the transport buttons, the shuffle/loop controls, the playlist and the position gauge.
    void defineControls() override {
        controls_.addButton("play");
        controls_.addButton("pause");
        controls_.addButton("stop");
        controls_.addButton("previous");
        controls_.addButton("next");
        controls_.addControl("shuffle", shuffle_);
        controls_.addSelect("loopMode", loopMode_, kLoopModeOptions, 3);
        controls_.addControl("fps", fps_, 1, 120);
        controls_.addList("playlist", *this);
        controls_.addProgress("position", frameIndex_, frameCount_, /*bytes=*/false);
        controls_.addButton("rescan");
        EffectBase::defineControls();
    }

    /// Mount the card (if this board has one) and scan it for animations once.
    void setup() override {
        EffectBase::setup();
        if constexpr (!platform::hasSdCard) {
            setStatus("no SD hardware on this build", Severity::Status);
            return;
        }
        if (!platform::sdMount()) {
            setStatus("mount failed — check the card is inserted and formatted FAT32", Severity::Error);
            return;
        }
        rescanSdFiles();
    }

    /// Start playing on its own once the effect becomes enabled.
    void prepare() override {
        if (state_ == PlayState::Stopped && playlistCount_ > 0) {
            visited_ = 0;
            if (loadTrack(cursor_)) state_ = PlayState::Playing;
        }
    }

    /// Force a clean stop before the base frees the frame/block buffers.
    void release() override {
        state_ = PlayState::Stopped;
        EffectBase::release();
    }

    /// Advance playback one frame at a time, or hold/clear the buffer while not playing.
    void tick() MM_NONBLOCKING override {
        if (state_ == PlayState::Stopped) {
            uint8_t* buf = buffer();
            if (buf) std::memset(buf, 0, static_cast<size_t>(nrOfLights()) * channelsPerLight());
            return;
        }
        if (state_ != PlayState::Playing) return;   // Paused: leave the last frame showing
        const uint32_t now = elapsed();
        const uint32_t stepMs = effectiveStepMs();
        if (now - lastFrameMs_ < stepMs) return;
        lastFrameMs_ += stepMs;   // advance by one step, not to `now`: a stall catches up one frame at a time
        loadAndBlitOneFrame();
        if (state_ != PlayState::Playing) return;   // a read failure above may have stopped playback
        frameIndex_++;
        if (frameIndex_ >= header_.frameCount) handleEndOfFile();
    }

    /// Dispatch a transport button to the state machine.
    void onControlChanged(const char* controlName) override {
        if (controlName) {
            if (std::strcmp(controlName, "play") == 0) {
                if (state_ == PlayState::Stopped) {
                    if (playlistCount_ > 0) { visited_ = 0; if (loadTrack(cursor_)) state_ = PlayState::Playing; }
                } else if (state_ == PlayState::Paused) {
                    lastFrameMs_ = elapsed();   // rebase, so a long pause does not fire a catch-up burst
                    state_ = PlayState::Playing;
                }
            } else if (std::strcmp(controlName, "pause") == 0) {
                if (state_ == PlayState::Playing) state_ = PlayState::Paused;
            } else if (std::strcmp(controlName, "stop") == 0) {
                if (state_ == PlayState::Playing || state_ == PlayState::Paused) {
                    state_ = PlayState::Stopped;
                    frameIndex_ = 0;
                    visited_ = 0;
                    refreshStatus();
                }
            } else if (std::strcmp(controlName, "previous") == 0) {
                moveCursor(-1);
            } else if (std::strcmp(controlName, "next") == 0) {
                moveCursor(+1);
            } else if (std::strcmp(controlName, "rescan") == 0) {
                rescanSdFiles();
            }
        }
        EffectBase::onControlChanged(controlName);
    }

    // --- ListSource (editable): the playlist -------------------------------------------
    /// How many rows the playlist holds.
    uint8_t listRowCount() const override { return playlistCount_; }

    /// Write one row, which is also the persisted form: the stable id and the chosen file.
    void writeListRow(JsonSink& sink, uint8_t row) const override {
        sink.appendf("{\"id\":%lu,\"file\":", static_cast<unsigned long>(tracks_[row].id));
        sink.writeJsonString(tracks_[row].file);
        sink.append("}");
    }

    /// Emitted once per list: the full SD paths the scan found.
    void writeListOptionSets(JsonSink& sink) const override {
        sink.append("\"sdFiles\":[");
        for (uint8_t i = 0; i < sdFileCount_; i++) {
            if (i) sink.append(",");
            sink.writeJsonString(sdFileOptions_[i]);
        }
        sink.append("]");
    }

    /// One row's editable fields: the file picker and a "play this row now" button.
    void writeListRowDetail(JsonSink& sink, uint8_t row) const override {
        sink.append("{\"fields\":[");
        sink.appendf("{\"name\":\"file\",\"type\":\"select\",\"value\":%u,\"optionsRef\":\"sdFiles\"},",
                     static_cast<unsigned>(sdFileIndexOf(tracks_[row].file)));
        sink.append("{\"name\":\"play\",\"type\":\"button\"}");
        sink.append("]}");
    }

    /// The playlist accepts add, delete, reorder and field edits.
    bool isEditableList() const override { return true; }

    /// Append a row, defaulting to the first scanned file if one exists.
    bool addListRow(uint32_t& outId) override {
        if (playlistCount_ >= kMaxTracks) return false;
        Track& t = tracks_[playlistCount_];
        t = Track{};
        t.id = nextId_++;
        if (sdFileCount_ > 0) copyBounded(t.file, sizeof(t.file), sdFileNames_[0]);
        playlistCount_++;
        outId = t.id;
        return true;
    }

    /// Remove a row by id, clamping the cursor back onto the shrunk list.
    bool deleteListRow(uint32_t id) override {
        const int i = indexOf(id);
        if (i < 0) return false;
        for (uint8_t j = static_cast<uint8_t>(i); j + 1 < playlistCount_; j++) tracks_[j] = tracks_[j + 1];
        playlistCount_--;
        if (cursor_ >= playlistCount_ && playlistCount_ > 0) cursor_ = static_cast<uint8_t>(playlistCount_ - 1);
        return true;
    }

    /// Move a row by id, which never changes that id.
    bool moveListRow(uint32_t id, uint8_t to) override {
        const int i = indexOf(id);
        if (i < 0) return false;
        if (to >= playlistCount_) to = static_cast<uint8_t>(playlistCount_ - 1);
        const Track moved = tracks_[i];
        if (to > i) for (int j = i; j < to; j++) tracks_[j] = tracks_[j + 1];
        else        for (int j = i; j > to; j--) tracks_[j] = tracks_[j - 1];
        tracks_[to] = moved;
        return true;
    }

    /// Edit a row's file, or act on its "play" field: jump to this row and start playing now.
    bool setListRowField(uint32_t id, const char* field, const char* valueJson) override {
        const int i = indexOf(id);
        if (i < 0) return false;
        if (std::strcmp(field, "file") == 0) {
            const int idx = mm::json::parseInt(valueJson, "value");
            if (idx < 0 || idx >= sdFileCount_) return false;
            copyBounded(tracks_[i].file, sizeof(tracks_[i].file), sdFileNames_[idx]);
            return true;
        }
        if (std::strcmp(field, "play") == 0) {
            cursor_ = static_cast<uint8_t>(i);
            visited_ = 0;
            if (loadTrack(cursor_)) state_ = PlayState::Playing;
            return true;
        }
        return false;
    }

    /// Restore the playlist from persisted JSON, keeping each row's stable id.
    bool restoreList(const char* json, const char* key) override {
        mm::json::JsonDoc doc;
        if (!mm::json::parse(json, doc)) return false;
        const mm::json::JsonNode* arr = mm::json::member(doc, doc.rootNode(), key);
        if (!arr || arr->type != mm::json::JsonType::Array) return false;
        playlistCount_ = 0;
        const int n = mm::json::arraySize(doc, arr);
        for (int r = 0; r < n && playlistCount_ < kMaxTracks; r++) {
            const mm::json::JsonNode* row = mm::json::element(doc, arr, r);
            Track& t = tracks_[playlistCount_];
            t = Track{};
            t.id = static_cast<uint32_t>(mm::json::readInt(mm::json::member(doc, row, "id"), 0));
            mm::json::readString(mm::json::member(doc, row, "file"), t.file, sizeof(t.file));
            if (t.id >= nextId_) nextId_ = t.id + 1;   // never reissue a persisted id
            playlistCount_++;
        }
        return true;
    }

    // --- Test seams: inject state directly, since every sdXxx is a failing stub on desktop ---
    /// Test seam: inject a parsed header directly, skipping the real SD read.
    void setHeaderForTest(const fseq::FseqHeader& h) {
        header_ = h;
        frameCount_ = h.frameCount;
        frameBuf_.resize(h.channelCount);
        frameIndex_ = 0;
        currentBlockIndex_ = 0;
        currentBlockFirstFrame_ = 0;
        currentBlockFrameCount_ = 0;
        nextBlockFileOffset_ = h.channelDataOffset;
    }
    /// Force the transport state directly.
    void setStateForTest(PlayState s) { state_ = s; }
    /// The current transport state.
    PlayState stateForTest() const { return state_; }
    /// The playlist row currently loaded (or about to load).
    uint8_t cursorForTest() const { return cursor_; }
    /// The frame position within the current track.
    uint32_t frameIndexForTest() const { return frameIndex_; }
    /// Run end-of-file handling without waiting out the real frame cadence.
    void handleEndOfFileForTest() { handleEndOfFile(); }
    /// Whether a full shuffle+loopOff pass has visited every playlist row.
    bool allTracksVisitedForTest() const { return allTracksVisited(); }
    /// Mark a row visited directly, as if its track had just loaded for real.
    void markVisitedForTest(uint8_t idx) { markVisited(idx); }
    /// Force the loop mode directly, one of kLoopOff/kLoopOne/kLoopAll below.
    void setLoopModeForTest(uint8_t m) { loopMode_ = m; }
    /// Force the shuffle flag directly.
    void setShuffleForTest(bool s) { shuffle_ = s; }
    /// Force the playback rate directly.
    void setFpsForTest(uint8_t fps) { fps_ = fps; }
    /// The milliseconds-per-frame tick() is actually pacing on right now.
    uint32_t effectiveStepMsForTest() const { return effectiveStepMs(); }
    /// Test seam for the scan's directory-join, the one piece of the scan desktop can exercise directly.
    static void joinPathForTest(const char* dir, const char* name, char* out, size_t outCap) {
        joinPath(dir, name, out, outCap);
    }

private:
    static constexpr const char* kLoopModeOptions[] = {"off", "one", "all"};
    static constexpr uint8_t kMaxTracks = 64;         // a playlist, not a media library — bounded, no heap
    static constexpr uint8_t kMaxSdFiles = 64;
    static constexpr uint8_t kMaxScanDepth = 8;       // how deep the scan descends, not a stack bound
    static constexpr uint8_t kMaxPendingDirs = 32;    // the scan's work-queue capacity; see ScanCtx below
    // A ceiling against a corrupt block-length field: real exports ran up to ~300KB/block, so 4MB keeps headroom.
    static constexpr size_t kMaxCompressedBlockBytes = 4 * 1024 * 1024;

    struct Track { uint32_t id = 0; char file[96] = {}; };   ///< the full SD path, not a bare name
    Track    tracks_[kMaxTracks] = {};
    uint8_t  playlistCount_ = 0;
    uint32_t nextId_ = 1;
    uint8_t  cursor_ = 0;

    bool     shuffle_ = false;
    uint8_t  loopMode_ = kLoopAll;
    uint8_t  fps_ = 30;   ///< the playback rate; 1000/fps milliseconds per frame

    PlayState       state_ = PlayState::Stopped;
    fseq::FseqHeader header_ = {};
    char            currentPath_[96] = {};
    uint32_t        frameIndex_ = 0;
    uint32_t        frameCount_ = 0;
    uint32_t        lastFrameMs_ = 0;
    uint64_t        visited_ = 0;   ///< per-pass bitset over playlist rows, for shuffle+loopOff
    Random8         rng_;

    ScratchBuffer<uint8_t> frameBuf_{*this};   ///< one uncompressed frame's raw bytes
    ScratchBuffer<uint8_t> compBuf_{*this};    ///< one compression block's compressed bytes
    ScratchBuffer<uint8_t> blockBuf_{*this};   ///< that block, decompressed
    uint32_t currentBlockIndex_ = 0;           ///< which compression-block-table entry is loaded
    uint32_t currentBlockFirstFrame_ = 0;      ///< that block's first frame
    uint32_t currentBlockFrameCount_ = 0;      ///< how many frames blockBuf_ covers
    size_t   nextBlockFileOffset_ = 0;         ///< where the NEXT block's compressed bytes start

    char        sdFileNames_[kMaxSdFiles][96] = {};   ///< full SD paths, found by the scan
    const char* sdFileOptions_[kMaxSdFiles] = {};
    uint8_t     sdFileCount_ = 0;

    // The scan's work queue: a member, not a rescanSdFiles() local (see @moreinfo above).
    char    scanPending_[kMaxPendingDirs][96] = {};
    uint8_t scanPendingDepth_[kMaxPendingDirs] = {};

    // Sized for "playing: " (9) + a 95-char path + " (999/999)" (10), with headroom for GCC's own worst-case check.
    char statusBuf_[128] = {};   ///< setStatus holds the pointer, so this outlives the call

    // A plain bounded copy, not snprintf: GCC's -Wrestrict flags snprintf between two member arrays of the same `this`.
    static void copyBounded(char* dst, size_t dstCap, const char* src) {
        if (dstCap == 0) return;
        size_t n = std::strlen(src);
        if (n >= dstCap) n = dstCap - 1;
        std::memcpy(dst, src, n);
        dst[n] = '\0';
    }
    // dir + "/" + name, built by hand for the same -Wrestrict reason as copyBounded; skips the slash when dir already ends in one.
    static void joinPath(const char* dir, const char* name, char* out, size_t outCap) {
        const size_t dirLen = std::strlen(dir);
        size_t n = dirLen < outCap ? dirLen : (outCap > 0 ? outCap - 1 : 0);
        std::memcpy(out, dir, n);
        const bool needsSlash = dirLen == 0 || dir[dirLen - 1] != '/';
        if (needsSlash && n < outCap) { out[n] = '/'; n++; }
        if (n < outCap) copyBounded(out + n, outCap - n, name);
        else if (outCap > 0) out[outCap - 1] = '\0';
    }

    /// One pending-scan context, pointing at rescanSdFiles()'s own queue (scanPending_ below).
    struct ScanCtx {
        FseqPlayerEffect* self;
        const char* currentDir;
        uint8_t currentDepth;
        char (*pending)[96];
        uint8_t* pendingDepth;
        uint8_t* pendingCount;
    };

    int indexOf(uint32_t id) const {
        for (uint8_t i = 0; i < playlistCount_; i++) if (tracks_[i].id == id) return i;
        return -1;
    }
    int sdFileIndexOf(const char* file) const {
        for (uint8_t i = 0; i < sdFileCount_; i++) if (std::strcmp(sdFileOptions_[i], file) == 0) return i;
        return 0;
    }
    void markVisited(uint8_t idx) { if (idx < 64) visited_ |= (uint64_t{1} << idx); }

    /// Milliseconds per frame, directly from the fps slider.
    uint32_t effectiveStepMs() const {
        if (fps_ == 0) return 1;   // defensive only: the control's own min bound keeps this unreachable
        const uint32_t ms = 1000u / fps_;
        return ms ? ms : 1;
    }

    bool allTracksVisited() const {
        if (playlistCount_ == 0) return true;
        const uint64_t full = playlistCount_ >= 64 ? ~uint64_t{0} : ((uint64_t{1} << playlistCount_) - 1);
        return (visited_ & full) == full;
    }

    /// Move the cursor by one step, shuffled or sequential.
    void moveCursor(int direction) {
        if (playlistCount_ == 0) return;
        advanceCursor(direction);
        if (state_ == PlayState::Stopped) return;
        loadTrack(cursor_);   // success leaves state_ untouched; failure forces it to Stopped
    }

    /// Pick the next playlist row, shuffled or sequential, the same shape as DemoReelEffect::advance().
    void advanceCursor(int direction) {
        if (playlistCount_ == 0) return;
        uint8_t next;
        if (shuffle_ && playlistCount_ > 1) {
            do { next = rng_.below(playlistCount_); } while (next == cursor_);
        } else {
            next = static_cast<uint8_t>((cursor_ + playlistCount_ + direction) % playlistCount_);
        }
        cursor_ = next;
    }

    /// End of the current track: loop it, advance to the next, or stop a completed pass.
    void handleEndOfFile() {
        if (loopMode_ == kLoopOne) {
            frameIndex_ = 0;
            currentBlockIndex_ = 0;
            currentBlockFirstFrame_ = 0;
            currentBlockFrameCount_ = 0;
            nextBlockFileOffset_ = header_.channelDataOffset;
            lastFrameMs_ = elapsed();
            return;
        }
        if (loopMode_ == kLoopOff && allTracksVisited()) {
            state_ = PlayState::Stopped;
            frameIndex_ = 0;
            visited_ = 0;
            refreshStatus();
            return;
        }
        advanceCursor(+1);
        loadTrack(cursor_);   // loopAll wraps forever; loopOff stops on the NEXT end-of-file once every row has played
    }

    /// Open and parse one playlist row's header; forces Stopped and reports the reason on failure.
    bool loadTrack(uint8_t idx) {
        if (idx >= playlistCount_) { state_ = PlayState::Stopped; return false; }
        cursor_ = idx;
        copyBounded(currentPath_, sizeof(currentPath_), tracks_[cursor_].file);   // file IS the full SD path now
        uint8_t head[4096];   // the fixed header plus the compression-block and sparse-range tables, in virtually any real file
        const int n = platform::sdReadAt(currentPath_, 0, reinterpret_cast<char*>(head), sizeof(head));
        if (n <= 0) {
            setStatus("SD read failed", Severity::Error);
            state_ = PlayState::Stopped;
            frameIndex_ = 0;
            return false;
        }
        fseq::FseqHeader hdr;
        const fseq::ParseError err = fseq::parseHeader(head, static_cast<size_t>(n), hdr);
        const char* errMsg = nullptr;
        switch (err) {
            case fseq::ParseError::BadMagic:               errMsg = "not an FSEQ file"; break;
            case fseq::ParseError::UnsupportedVersion:     errMsg = "unsupported FSEQ version"; break;
            case fseq::ParseError::UnsupportedCompression: errMsg = "compressed (zstd) FSEQ not supported yet"; break;
            case fseq::ParseError::Truncated:              errMsg = "FSEQ header truncated or unreadable"; break;
            default: break;   // None, or TooManySparseRanges (non-fatal, handled below)
        }
        if (errMsg) {
            setStatus(errMsg, Severity::Error);
            state_ = PlayState::Stopped;
            frameIndex_ = 0;
            return false;
        }
        header_ = hdr;
        frameCount_ = header_.frameCount;
        frameBuf_.resize(header_.channelCount);
        currentBlockIndex_ = 0;
        currentBlockFirstFrame_ = 0;
        currentBlockFrameCount_ = 0;
        nextBlockFileOffset_ = header_.channelDataOffset;
        frameIndex_ = 0;
        lastFrameMs_ = elapsed();
        markVisited(cursor_);
        rebuildControls();   // the "position" progress total (frameCount_) just changed
        if (err == fseq::ParseError::TooManySparseRanges) setStatus("FSEQ sparse range table clipped", Severity::Warning);
        else refreshStatus();
        return true;
    }

    /// Read (or decompress) one frame and blit it onto the layer buffer, sparse or dense.
    void loadAndBlitOneFrame() {
        const uint8_t* rawFrame;
        if (header_.compressionType == fseq::CompressionType::Zlib) {
            rawFrame = frameFromCompressedBlock(frameIndex_);
            if (!rawFrame) {
                setStatus("SD read failed mid-file", Severity::Error);
                state_ = PlayState::Stopped;
                frameIndex_ = 0;
                return;
            }
        } else {
            const size_t off = fseq::frameByteOffset(header_, frameIndex_);
            const int n = platform::sdReadAt(currentPath_, static_cast<long>(off),
                                             reinterpret_cast<char*>(frameBuf_.data()), header_.channelCount);
            if (n != static_cast<int>(header_.channelCount)) {
                setStatus("SD read failed mid-file", Severity::Error);
                state_ = PlayState::Stopped;
                frameIndex_ = 0;
                return;
            }
            rawFrame = frameBuf_.data();
        }
        uint8_t* buf = buffer();
        if (!buf) return;
        const size_t bufBytes = static_cast<size_t>(nrOfLights()) * channelsPerLight();
        if (header_.sparseRangeCount > 0) {
            // Unmapped channels go dark: the sparse table only addresses the fixtures a show touches.
            std::memset(buf, 0, bufBytes);
            fseq::expandSparseFrame(header_, rawFrame, header_.channelCount, buf, bufBytes);
        } else {
            const size_t n = static_cast<size_t>(header_.channelCount) < bufBytes ? header_.channelCount : bufBytes;
            std::memcpy(buf, rawFrame, n);
        }
    }

    /// The raw bytes for `frameIndex` out of the current (or a freshly loaded) compression block.
    const uint8_t* frameFromCompressedBlock(uint32_t frameIndex) {
        while (frameIndex >= currentBlockFirstFrame_ + currentBlockFrameCount_) {
            if (!loadNextBlock()) return nullptr;
        }
        // A block table not starting at frame 0 is malformed; fail rather than underflow this subtraction.
        if (frameIndex < currentBlockFirstFrame_) return nullptr;
        const size_t offset = static_cast<size_t>(frameIndex - currentBlockFirstFrame_) * header_.channelCount;
        return blockBuf_.data() + offset;
    }

    /// Read, decompress and install the next compression block.
    bool loadNextBlock() {
        if (currentBlockIndex_ >= header_.compressionBlockCount) return false;
        uint8_t entry[8];
        const size_t entryOffset = 32 + static_cast<size_t>(currentBlockIndex_) * 8;
        if (platform::sdReadAt(currentPath_, static_cast<long>(entryOffset),
                               reinterpret_cast<char*>(entry), sizeof(entry)) != static_cast<int>(sizeof(entry)))
            return false;
        const uint32_t blockFirstFrame = rdU32(entry);
        const uint32_t blockByteLen = rdU32(entry + 4);

        uint32_t blockFrameCount;
        if (currentBlockIndex_ + 1 < header_.compressionBlockCount) {
            uint8_t nextFirst[4];
            if (platform::sdReadAt(currentPath_, static_cast<long>(entryOffset + 8),
                                   reinterpret_cast<char*>(nextFirst), sizeof(nextFirst)) != static_cast<int>(sizeof(nextFirst)))
                return false;
            blockFrameCount = rdU32(nextFirst) - blockFirstFrame;
        } else {
            blockFrameCount = header_.frameCount - blockFirstFrame;
        }
        if (blockByteLen == 0 || blockByteLen > kMaxCompressedBlockBytes || blockFrameCount == 0) return false;

        compBuf_.resize(blockByteLen);
        if (!compBuf_) return false;
        if (platform::sdReadAt(currentPath_, static_cast<long>(nextBlockFileOffset_),
                               reinterpret_cast<char*>(compBuf_.data()), blockByteLen) != static_cast<int>(blockByteLen))
            return false;

        const size_t decompLen = static_cast<size_t>(blockFrameCount) * header_.channelCount;
        blockBuf_.resize(decompLen);
        if (!blockBuf_) return false;
        size_t outLen = 0;
        if (!platform::zlibInflateAll(compBuf_.data(), blockByteLen, blockBuf_.data(), decompLen, outLen) ||
            outLen != decompLen)
            return false;

        nextBlockFileOffset_ += blockByteLen;
        currentBlockFirstFrame_ = blockFirstFrame;
        currentBlockFrameCount_ = blockFrameCount;
        currentBlockIndex_++;
        return true;
    }

    static uint32_t rdU32(const uint8_t* p) {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }

    /// Re-scan every folder on the SD card for `.fseq` files into the shared option set.
    void rescanSdFiles() {
        sdFileCount_ = 0;
        uint8_t pendingCount = 1;
        copyBounded(scanPending_[0], sizeof(scanPending_[0]), "/");
        scanPendingDepth_[0] = 0;

        while (pendingCount > 0 && sdFileCount_ < kMaxSdFiles) {
            pendingCount--;
            char dir[96];   // a local copy: sdListCallback may push into scanPending_ at this same (just-popped) slot
            copyBounded(dir, sizeof(dir), scanPending_[pendingCount]);
            const uint8_t depth = scanPendingDepth_[pendingCount];
            platform::feedWatchdog();   // a big card is many small blocking directory reads
            ScanCtx ctx{this, dir, depth, scanPending_, scanPendingDepth_, &pendingCount};
            platform::sdList(dir, &FseqPlayerEffect::sdListCallback, &ctx);
        }
        for (uint8_t i = 0; i < sdFileCount_; i++) sdFileOptions_[i] = sdFileNames_[i];
        rebuildControls();
        // Fires the resync directly: rebuildControls()'s own change-detection does not hash a List's row/option-set content.
        notifySchemaChanged();
    }

    /// One directory entry: a file is recorded, a subdirectory is queued, anything dot-prefixed is skipped.
    static void sdListCallback(const char* name, bool isDir, uint32_t /*sizeBytes*/, void* userPtr) {
        auto* ctx = static_cast<ScanCtx*>(userPtr);
        if (!name || name[0] == '.' || ctx->self->sdFileCount_ >= kMaxSdFiles) return;
        char childPath[96];
        joinPath(ctx->currentDir, name, childPath, sizeof(childPath));
        if (isDir) {
            if (ctx->currentDepth >= kMaxScanDepth || *ctx->pendingCount >= kMaxPendingDirs) return;
            copyBounded(ctx->pending[*ctx->pendingCount], 96, childPath);
            ctx->pendingDepth[*ctx->pendingCount] = static_cast<uint8_t>(ctx->currentDepth + 1);
            (*ctx->pendingCount)++;
            return;
        }
        if (!hasExtension(name, ".fseq")) return;
        copyBounded(ctx->self->sdFileNames_[ctx->self->sdFileCount_], sizeof(ctx->self->sdFileNames_[0]), childPath);
        ctx->self->sdFileCount_++;
    }

    /// Case-insensitive suffix check, since a card may carry ".FSEQ" from a different export tool.
    static bool hasExtension(const char* name, const char* ext) {
        const size_t nameLen = std::strlen(name);
        const size_t extLen = std::strlen(ext);
        if (nameLen < extLen) return false;
        for (size_t i = 0; i < extLen; i++) {
            char a = name[nameLen - extLen + i], b = ext[i];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
            if (a != b) return false;
        }
        return true;
    }

    /// Show the transport state, the current file, and its position in the playlist.
    void refreshStatus() {
        if (playlistCount_ == 0) { setStatus("no tracks in playlist"); return; }
        const char* label = state_ == PlayState::Playing ? "playing"
                           : state_ == PlayState::Paused  ? "paused"
                                                           : "stopped";
        // Copied to a local first: GCC's -Wrestrict cannot see past two member arrays of the same `this`.
        char fileCopy[sizeof(Track::file)];
        copyBounded(fileCopy, sizeof(fileCopy), tracks_[cursor_].file);
        std::snprintf(statusBuf_, sizeof(statusBuf_), "%s: %s (%u/%u)", label, fileCopy,
                      static_cast<unsigned>(cursor_ + 1), static_cast<unsigned>(playlistCount_));
        setStatus(statusBuf_);
    }
};

} // namespace mm
