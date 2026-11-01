// Copyright (c) 2026 Hiroshi Matsuyama
// SPDX-License-Identifier: MIT
//
// Unit tests for the widget's timer, panel drawing and graphics helpers.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <vector>

#include "../src/graphics.h"
#include "../src/panel.h"
#include "../src/pomodoro.h"

using namespace std::chrono_literals;

TEST_CASE("Pomodoro starts paused in a full focus phase") {
    Pomodoro p(25min, 5min);
    CHECK_FALSE(p.running());
    CHECK(p.phase() == Pomodoro::Phase::Focus);
    CHECK(p.displaySeconds() == 25 * 60);
    CHECK(p.progress() == doctest::Approx(0.0f));
    CHECK_FALSE(p.advance(10min));  // Paused: time does not pass.
    CHECK(p.displaySeconds() == 25 * 60);
}

TEST_CASE("Pomodoro counts down, rounding the display up") {
    Pomodoro p(25min, 5min);
    p.toggle();
    CHECK_FALSE(p.advance(1500ms));
    CHECK(p.displaySeconds() == 25 * 60 - 1);  // 1498.5 s left shows as 1499.
    CHECK(p.progress() == doctest::Approx(1.5f / (25 * 60)));
    p.toggle();
    CHECK_FALSE(p.advance(1h));
    CHECK(p.displaySeconds() == 25 * 60 - 1);
}

TEST_CASE("Pomodoro alternates focus and break") {
    Pomodoro p(25min, 5min);
    p.toggle();
    CHECK(p.advance(25min));
    CHECK(p.phase() == Pomodoro::Phase::Break);
    CHECK(p.completedFocusPhases() == 1);
    CHECK(p.displaySeconds() == 5 * 60);
    CHECK(p.advance(5min + 1s));
    CHECK(p.phase() == Pomodoro::Phase::Focus);
    CHECK(p.displaySeconds() == 25 * 60 - 1);
    SUBCASE("a long step crosses several phases") {
        CHECK(p.advance(30min));
        CHECK(p.phase() == Pomodoro::Phase::Focus);
        CHECK(p.completedFocusPhases() == 2);
    }
}

TEST_CASE("Pomodoro reset") {
    Pomodoro p(25min, 5min);
    p.toggle();
    p.advance(26min);
    p.reset();
    CHECK_FALSE(p.running());
    CHECK(p.phase() == Pomodoro::Phase::Focus);
    CHECK(p.displaySeconds() == 25 * 60);
}

TEST_CASE("Pomodoro with zero lengths does not hang") {
    Pomodoro p(0ms, 0ms);
    p.toggle();
    p.advance(1s);
    CHECK(p.displaySeconds() == 0);
}

TEST_CASE("Color premultiplies alpha") {
    const Color c = Color::Rgba(200, 100, 0, 128);
    CHECK(c.r == 100);
    CHECK(c.g == 50);
    CHECK(c.b == 0);
    CHECK(c.a == 128);
    CHECK(Color::Rgba(10, 20, 30) == Color{10, 20, 30, 255});
}

TEST_CASE("FillRect clips to the image") {
    Image image(8, 4);
    const Color red = Color::Rgba(255, 0, 0);
    image.FillRect(-2, 2, 4, 10, red);
    CHECK(image.at(0, 2) == red);
    CHECK(image.at(1, 3) == red);
    CHECK(image.at(2, 2) == Color{});
    CHECK(image.at(0, 1) == Color{});
}

TEST_CASE("FillRoundedRect leaves the corners empty") {
    Image image(40, 30);
    const Color c = Color::Rgba(1, 2, 3);
    image.FillRoundedRect(0, 0, 40, 30, 10, c);
    CHECK(image.at(0, 0) == Color{});
    CHECK(image.at(39, 29) == Color{});
    CHECK(image.at(20, 0) == c);   // Middle of the top edge.
    CHECK(image.at(0, 15) == c);   // Middle of the left edge.
    CHECK(image.at(20, 15) == c);  // Center.
}

TEST_CASE("Seven-segment digits light the right segments") {
    const Color on = Color::Rgba(255, 255, 255);
    // Box 20 x 40 at the origin, segments 4 wide. Sample the middle of each segment.
    struct Probe {
        int x, y;
        char segment;
    } probes[] = {{10, 1, 'a'}, {18, 10, 'b'}, {18, 30, 'c'}, {10, 38, 'd'}, {1, 30, 'e'}, {1, 10, 'f'}, {10, 20, 'g'}};
    auto lit = [&](int digit) {
        Image image(20, 40);
        DrawDigit(image, digit, 0, 0, 20, 40, 4, on);
        std::string segments;
        for (const Probe& p : probes) {
            if (image.at(p.x, p.y) == on) segments += p.segment;
        }
        return segments;
    };
    CHECK(lit(8) == "abcdefg");
    CHECK(lit(0) == "abcdef");
    CHECK(lit(1) == "bc");
    CHECK(lit(7) == "abc");
    CHECK(lit(4) == "bcfg");
    CHECK(lit(2) == "abdeg");
}

TEST_CASE("DrawLabel draws known letters and skips unknown ones") {
    Image image(40, 10);
    const Color c = Color::Rgba(9, 9, 9);
    CHECK(DrawLabel(image, "L", 0, 0, 1, c) == 5);
    CHECK(image.at(0, 0) == c);  // The vertical stroke of L.
    CHECK(image.at(4, 6) == c);  // Its foot.
    CHECK(image.at(4, 0) == Color{});
    Image blank(40, 10);
    DrawLabel(blank, "?", 0, 0, 1, c);
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 40; ++x) CHECK(blank.at(x, y) == Color{});
}

TEST_CASE("RenderPanel lays out the clock, countdown and progress") {
    Image image(panel::kWidth, panel::kHeight);
    PanelState state;
    state.hour = 8;
    state.minute = 8;
    state.focus = true;
    state.running = true;
    state.remainingSeconds = 8 * 60 + 8;
    state.progress = 0.5f;
    RenderPanel(state, image);

    CHECK(image.at(0, 0) == Color{});                             // Rounded corner.
    CHECK(image.at(panel::kWidth / 2, 2) == panel::kBackground);  // Panel body.
    // First clock digit "0": its top segment is lit, its middle segment is not.
    const int clockX = (panel::kWidth - panel::kClockStyle.Width()) / 2;
    const int digitMidX = clockX + panel::kClockStyle.digitWidth / 2;
    CHECK(image.at(digitMidX, panel::kClockY + 2) == panel::kClock);
    CHECK(image.at(digitMidX, panel::kClockY + panel::kClockStyle.digitHeight / 2) == panel::kBackground);
    // Progress: the left quarter is filled with the focus color, the right quarter is track.
    const int barY = panel::kBarY + panel::kBarHeight / 2;
    CHECK(image.at(panel::kBarX + panel::kBarWidth / 4, barY) == panel::kFocus);
    CHECK(image.at(panel::kBarX + 3 * panel::kBarWidth / 4, barY) == panel::kTrack);
    // Countdown "08:08": the last digit's middle segment is lit in the focus color.
    const int countdownRight = panel::kWidth - panel::kMargin;
    const int lastDigitMidX = countdownRight - panel::kCountdownStyle.digitWidth / 2;
    CHECK(image.at(lastDigitMidX, panel::kCountdownY + panel::kCountdownStyle.digitHeight / 2) == panel::kFocus);
}

TEST_CASE("RenderPanel shows pause and break in their colors") {
    Image image(panel::kWidth, panel::kHeight);
    PanelState state;
    state.remainingSeconds = 8;
    state.progress = 1.0f;
    const int barY = panel::kBarY + panel::kBarHeight / 2;
    const int barMid = panel::kBarX + panel::kBarWidth / 2;

    state.running = false;
    RenderPanel(state, image);
    CHECK(image.at(barMid, barY) == panel::kPaused);

    state.running = true;
    state.focus = false;
    RenderPanel(state, image);
    CHECK(image.at(barMid, barY) == panel::kBreak);
}

TEST_CASE("SameDisplay ignores progress changes within a bar pixel") {
    PanelState a;
    a.progress = 0.5f;
    PanelState b = a;
    b.progress = 0.5f + 0.4f / panel::kBarWidth;
    CHECK(SameDisplay(a, b));
    b.progress = 0.5f + 2.0f / panel::kBarWidth;
    CHECK_FALSE(SameDisplay(a, b));
    b = a;
    b.remainingSeconds = 1;
    CHECK_FALSE(SameDisplay(a, b));
    b = a;
    b.running = !a.running;
    CHECK_FALSE(SameDisplay(a, b));
}

TEST_CASE("FitPanel keeps the panel's aspect ratio inside the bounds") {
    const float aspect = static_cast<float>(panel::kWidth) / panel::kHeight;
    auto [w, h] = FitPanel(0.48f, 0.288f);
    CHECK(w == doctest::Approx(0.48f));
    CHECK(h == doctest::Approx(0.288f));
    std::tie(w, h) = FitPanel(1.0f, 0.2f);  // Wide box: height limits.
    CHECK(h == doctest::Approx(0.2f));
    CHECK(w == doctest::Approx(0.2f * aspect));
    std::tie(w, h) = FitPanel(0.3f, 1.0f);  // Tall box: width limits.
    CHECK(w == doctest::Approx(0.3f));
    CHECK(h == doctest::Approx(0.3f / aspect));
    std::tie(w, h) = FitPanel(0.0f, 1.0f);
    CHECK(w == 0.0f);
}

TEST_CASE("ChooseSwapchainFormat prefers sRGB, then RGBA over BGRA") {
    const std::vector<int64_t> rgba = {43, 37}, bgra = {50, 44};  // Like Vulkan's SRGB / UNORM codes.
    SwapchainFormat f = ChooseSwapchainFormat({44, 50, 37}, rgba, bgra);
    CHECK(f.format == 50);  // BGRA sRGB beats RGBA UNORM.
    CHECK(f.bgra);
    f = ChooseSwapchainFormat({37, 43}, rgba, bgra);
    CHECK(f.format == 43);
    CHECK_FALSE(f.bgra);
    f = ChooseSwapchainFormat({1, 2}, rgba, bgra);
    CHECK(f.format == 0);
}

TEST_CASE("CopyPixels copies rows and swizzles to BGRA") {
    Image image(2, 2);
    image.FillRect(0, 0, 1, 1, Color{1, 2, 3, 4});
    std::vector<uint8_t> out(2 * 12, 0xEE);  // Row pitch 12: 4 bytes of padding per row.
    CopyPixels(image, true, out.data(), 12);
    CHECK(out[0] == 3);
    CHECK(out[1] == 2);
    CHECK(out[2] == 1);
    CHECK(out[3] == 4);
    CHECK(out[8] == 0xEE);  // Padding untouched.
    CopyPixels(image, false, out.data(), 12);
    CHECK(out[0] == 1);
    CHECK(out[2] == 3);
}
