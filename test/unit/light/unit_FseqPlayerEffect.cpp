/// @module FseqPlayerEffect
/// @also FseqFile

/// Pins the FSEQ player transport: play/pause/stop/previous/next, shuffle, loop modes. sdXxx always fails on desktop, so these inject state via test seams rather than a real file.

#include "doctest.h"

#include "light/effects/FseqPlayerEffect.h"
#include "light/layouts/GridLayout.h"

#include <string>

using namespace mm;

namespace {

fseq::FseqHeader makeHeader(uint32_t channelCount, uint32_t frameCount, uint8_t stepTimeMs = 25) {
    fseq::FseqHeader h;
    h.channelDataOffset = 32;
    h.channelCount = channelCount;
    h.frameCount = frameCount;
    h.stepTimeMs = stepTimeMs;
    return h;
}

// A real 4x4 Layer the effect is parented to, the same fixture shape unit_DemoReelEffect.cpp uses so elapsed()/buffer() resolve through the parent.
struct Scene {
    Layouts layouts;
    GridLayout grid;
    Layer layer;
    FseqPlayerEffect effect;
    Scene() {
        grid.width = 4; grid.height = 4; grid.depth = 1;
        layouts.addChild(&grid);
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.addChild(&effect);
    }
};

} // namespace

TEST_CASE("setup reports no SD hardware, which is every desktop build") {
    Scene s;
    auto& e = s.effect;
    e.setup();
    REQUIRE(e.status() != nullptr);
    CHECK(std::string(e.status()) == "no SD hardware on this build");
}

TEST_CASE("prepare() starts playing on its own once there is a playlist to play") {
    Scene s;
    auto& e = s.effect;
    // Empty playlist: nothing to start, and no crash from trying anyway.
    e.prepare();
    CHECK(e.stateForTest() == FseqPlayerEffect::PlayState::Stopped);

    uint32_t id = 0;
    e.addListRow(id);
    e.prepare();
    // No SD card on desktop, so the attempt fails — but an error status (not silence) proves prepare() tried to auto-start.
    CHECK(e.stateForTest() == FseqPlayerEffect::PlayState::Stopped);
    REQUIRE(e.status() != nullptr);
    CHECK(e.severity() == MoonModule::Severity::Error);
}

TEST_CASE("release() forces a clean stop, so a later re-enable never resumes into freed buffers") {
    Scene s;
    auto& e = s.effect;
    e.setStateForTest(FseqPlayerEffect::PlayState::Playing);
    e.release();
    CHECK(e.stateForTest() == FseqPlayerEffect::PlayState::Stopped);
}

TEST_CASE("a stopped player owns its background (clears to black), but a paused one leaves the last frame") {
    Scene s;
    auto& e = s.effect;
    uint8_t* buf = const_cast<uint8_t*>(s.layer.buffer().data());
    const size_t bytes = s.layer.buffer().count() * 3;
    constexpr uint8_t kStale = 0xA7;   // a value no real frame would coincidentally produce

    std::memset(buf, kStale, bytes);
    REQUIRE(e.stateForTest() == FseqPlayerEffect::PlayState::Stopped);   // the default, no playlist
    e.tick();
    for (size_t i = 0; i < bytes; i++) CHECK(buf[i] == 0);   // owns its background: goes black

    std::memset(buf, kStale, bytes);
    e.setStateForTest(FseqPlayerEffect::PlayState::Paused);
    e.tick();
    for (size_t i = 0; i < bytes; i++) CHECK(buf[i] == kStale);   // paused: the last frame stays
}

TEST_CASE("the playlist is an editable list: add, delete, move, edit") {
    Scene s;
    auto& e = s.effect;
    uint32_t id1 = 0, id2 = 0, id3 = 0;
    CHECK(e.isEditableList());
    CHECK(e.addListRow(id1));
    CHECK(e.addListRow(id2));
    CHECK(e.listRowCount() == 2);
    CHECK(id1 != id2);

    CHECK(e.deleteListRow(id1));
    CHECK(e.listRowCount() == 1);
    CHECK_FALSE(e.deleteListRow(id1));   // already gone

    CHECK(e.addListRow(id3));
    CHECK(e.listRowCount() == 2);
    CHECK(e.moveListRow(id3, 0));        // id3 now row 0, id2 row 1

    // No SD files have been scanned, so the file picker's option index is out of range.
    CHECK_FALSE(e.setListRowField(id2, "file", "{\"value\":0}"));
    CHECK_FALSE(e.setListRowField(999999, "file", "{\"value\":0}"));   // an unknown id
    CHECK_FALSE(e.setListRowField(id2, "nonsense", "{\"value\":0}"));  // an unknown field
}

TEST_CASE("play with an empty playlist does nothing") {
    Scene s;
    auto& e = s.effect;
    e.onControlChanged("play");
    CHECK(e.stateForTest() == FseqPlayerEffect::PlayState::Stopped);
}

TEST_CASE("play with no SD card fails gracefully rather than pretending to start") {
    Scene s;
    auto& e = s.effect;
    uint32_t id = 0;
    e.addListRow(id);
    e.onControlChanged("play");
    // loadTrack's SD read fails (no card on desktop), so playback never actually starts.
    CHECK(e.stateForTest() == FseqPlayerEffect::PlayState::Stopped);
    REQUIRE(e.status() != nullptr);
    CHECK(e.severity() == MoonModule::Severity::Error);
}

TEST_CASE("pause only takes effect while playing") {
    Scene s;
    auto& e = s.effect;
    e.setStateForTest(FseqPlayerEffect::PlayState::Stopped);
    e.onControlChanged("pause");
    CHECK(e.stateForTest() == FseqPlayerEffect::PlayState::Stopped);

    e.setStateForTest(FseqPlayerEffect::PlayState::Playing);
    e.onControlChanged("pause");
    CHECK(e.stateForTest() == FseqPlayerEffect::PlayState::Paused);

    e.onControlChanged("pause");   // already paused: no further change
    CHECK(e.stateForTest() == FseqPlayerEffect::PlayState::Paused);
}

TEST_CASE("stop resets the frame position and only applies while playing or paused") {
    Scene s;
    auto& e = s.effect;
    e.onControlChanged("stop");   // Stopped -> Stopped: a no-op, not a crash
    CHECK(e.stateForTest() == FseqPlayerEffect::PlayState::Stopped);

    e.setStateForTest(FseqPlayerEffect::PlayState::Playing);
    e.setHeaderForTest(makeHeader(10, 100));
    e.onControlChanged("stop");
    CHECK(e.stateForTest() == FseqPlayerEffect::PlayState::Stopped);
    CHECK(e.frameIndexForTest() == 0);
}

TEST_CASE("previous and next move the cursor only, while the transport is stopped") {
    Scene s;
    auto& e = s.effect;
    uint32_t id1 = 0, id2 = 0, id3 = 0;
    e.addListRow(id1);
    e.addListRow(id2);
    e.addListRow(id3);
    REQUIRE(e.cursorForTest() == 0);

    e.onControlChanged("next");
    CHECK(e.cursorForTest() == 1);
    e.onControlChanged("next");
    CHECK(e.cursorForTest() == 2);
    e.onControlChanged("next");       // wraps past the last row
    CHECK(e.cursorForTest() == 0);
    e.onControlChanged("previous");   // wraps back
    CHECK(e.cursorForTest() == 2);

    // Stopped, so moving the cursor never touches playback state.
    CHECK(e.stateForTest() == FseqPlayerEffect::PlayState::Stopped);
}

TEST_CASE("shuffle's cursor never repeats the row it just left") {
    Scene s;
    auto& e = s.effect;
    uint32_t ids[5] = {};
    for (uint32_t& id : ids) e.addListRow(id);
    e.setShuffleForTest(true);

    uint8_t prev = e.cursorForTest();
    for (int i = 0; i < 200; i++) {
        e.onControlChanged("next");
        const uint8_t now = e.cursorForTest();
        CHECK(now < 5);
        CHECK(now != prev);   // the do-while non-repeat guard in advanceCursor
        prev = now;
    }
}

TEST_CASE("the recursive scan's path join never doubles the slash at the SD root, and truncates safely") {
    char out[32];
    FseqPlayerEffect::joinPathForTest("/", "show.fseq", out, sizeof(out));
    CHECK(std::string(out) == "/show.fseq");

    FseqPlayerEffect::joinPathForTest("/shows", "intro.fseq", out, sizeof(out));
    CHECK(std::string(out) == "/shows/intro.fseq");

    FseqPlayerEffect::joinPathForTest("/shows/", "intro.fseq", out, sizeof(out));
    CHECK(std::string(out) == "/shows/intro.fseq");   // trailing slash on dir: no double "//"

    char small[6];
    FseqPlayerEffect::joinPathForTest("/shows", "averylongname.fseq", small, sizeof(small));
    CHECK(std::strlen(small) < sizeof(small));   // truncated, still NUL-terminated, never overrun
}

TEST_CASE("the fps slider sets the playback rate directly, defaulting to 30, ignoring the file's own step time") {
    Scene s;
    auto& e = s.effect;
    // The file's stepTimeMs (25ms, i.e. 40fps) deliberately differs from the fps default, to prove the slider wins.
    e.setHeaderForTest(makeHeader(10, 50, /*stepTimeMs=*/25));
    CHECK(e.effectiveStepMsForTest() == 33);   // fps_ defaults to 30: 1000/30, rounded down

    e.setFpsForTest(20);
    CHECK(e.effectiveStepMsForTest() == 50);   // 1000/20
    e.setFpsForTest(1);
    CHECK(e.effectiveStepMsForTest() == 1000);
    e.setFpsForTest(120);
    CHECK(e.effectiveStepMsForTest() == 8);    // 1000/120, rounded down, never 0
}

TEST_CASE("loopOne restarts the same track at frame 0 without touching the SD card") {
    Scene s;
    auto& e = s.effect;
    uint32_t id = 0;
    e.addListRow(id);
    e.setStateForTest(FseqPlayerEffect::PlayState::Playing);
    e.setLoopModeForTest(FseqPlayerEffect::kLoopOne);
    e.setHeaderForTest(makeHeader(10, 50));
    e.handleEndOfFileForTest();
    CHECK(e.frameIndexForTest() == 0);
    CHECK(e.stateForTest() == FseqPlayerEffect::PlayState::Playing);   // stays playing: it is the same track
    CHECK(e.cursorForTest() == 0);                                     // never advanced
}

TEST_CASE("loopOff stops once every row has already played this pass, with no SD read needed") {
    Scene s;
    auto& e = s.effect;
    uint32_t ids[3] = {};
    for (uint32_t& id : ids) e.addListRow(id);
    e.setStateForTest(FseqPlayerEffect::PlayState::Playing);
    e.setLoopModeForTest(FseqPlayerEffect::kLoopOff);
    e.setHeaderForTest(makeHeader(10, 50));
    // Simulate a pass that already touched every row (as loadTrack would have, had the reads succeeded).
    e.markVisitedForTest(0);
    e.markVisitedForTest(1);
    e.markVisitedForTest(2);
    REQUIRE(e.allTracksVisitedForTest());

    e.handleEndOfFileForTest();   // this branch returns before ever touching loadTrack/SD
    CHECK(e.stateForTest() == FseqPlayerEffect::PlayState::Stopped);
    CHECK(e.frameIndexForTest() == 0);
    CHECK_FALSE(e.allTracksVisitedForTest());   // the bitset is cleared for the next pass
}

TEST_CASE("loopAll/loopOff advancing to an unvisited track degrades to Stopped when the SD read fails") {
    // Both branches fall through to loadTrack(), which always fails on desktop; this pins the degradation (an error status, not a crash) rather than the hardware-only "wraps forever" behavior.
    Scene s;
    auto& e = s.effect;
    uint32_t id1 = 0, id2 = 0;
    e.addListRow(id1);
    e.addListRow(id2);
    e.setStateForTest(FseqPlayerEffect::PlayState::Playing);
    e.setLoopModeForTest(FseqPlayerEffect::kLoopAll);
    e.setHeaderForTest(makeHeader(10, 50));

    e.handleEndOfFileForTest();
    CHECK(e.stateForTest() == FseqPlayerEffect::PlayState::Stopped);
    REQUIRE(e.status() != nullptr);
    CHECK(e.severity() == MoonModule::Severity::Error);
}

TEST_CASE("a playlist row's own play field jumps to that row and starts a fresh pass") {
    Scene s;
    auto& e = s.effect;
    uint32_t id1 = 0, id2 = 0;
    e.addListRow(id1);
    e.addListRow(id2);
    e.markVisitedForTest(0);   // pretend row 0 already played this pass

    e.setListRowField(id2, "play", "{}");
    // The SD read fails on desktop, but the jump and the fresh-pass reset happen before that failure is discovered.
    CHECK(e.cursorForTest() == 1);
    CHECK(e.stateForTest() == FseqPlayerEffect::PlayState::Stopped);
}
