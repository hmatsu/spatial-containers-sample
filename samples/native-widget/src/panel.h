// Copyright (c) 2026 Hiroshi Matsuyama
// SPDX-License-Identifier: MIT
//
// The widget's panel, drawn on the CPU into an RGBA image: a clock, the pomodoro
// phase and countdown, and a progress bar. No fonts or GPU code are involved, so the
// same image can be uploaded by any graphics backend and checked in unit tests.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Premultiplied-alpha RGBA color, 8 bits per channel, sRGB encoded.
struct Color {
    uint8_t r = 0, g = 0, b = 0, a = 0;

    static Color Rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
        auto premultiply = [a](uint8_t c) { return static_cast<uint8_t>((c * a + 127) / 255); };
        return {premultiply(r), premultiply(g), premultiply(b), a};
    }
    bool operator==(const Color& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
};

class Image {
public:
    Image(int width, int height) : width_(width), height_(height), pixels_(static_cast<size_t>(width) * height) {}

    int width() const { return width_; }
    int height() const { return height_; }
    const Color& at(int x, int y) const { return pixels_[static_cast<size_t>(y) * width_ + x]; }
    const uint8_t* data() const { return &pixels_[0].r; }
    size_t rowPitch() const { return static_cast<size_t>(width_) * sizeof(Color); }

    void Clear(Color color = {});
    // Rectangles are clipped to the image.
    void FillRect(int x, int y, int w, int h, Color color);
    void FillRoundedRect(int x, int y, int w, int h, int radius, Color color);

private:
    int width_, height_;
    std::vector<Color> pixels_;
};

static_assert(sizeof(Color) == 4, "Color must be tightly packed RGBA8");

// A seven-segment digit in the box (x, y, w, h) with segments `thickness` wide.
void DrawDigit(Image& image, int digit, int x, int y, int w, int h, int thickness, Color color);
// The two dots between hours and minutes, centered in a box `w` wide.
void DrawColon(Image& image, int x, int y, int w, int h, int thickness, Color color);
// Uppercase text in a 5x7 pixel font, each font pixel `scale` image pixels. Characters
// the font does not have are drawn as spaces. Returns the width drawn.
int DrawText(Image& image, const std::string& text, int x, int y, int scale, Color color);

// "HH:MM"-style pair of two-digit numbers in seven-segment digits.
struct DigitStyle {
    int digitWidth, digitHeight, thickness, gap, colonWidth;
    int Width() const { return 4 * digitWidth + 2 * gap + colonWidth + 2 * gap; }
};
void DrawTwoPairs(Image& image, int left, int right, int x, int y, const DigitStyle& style, Color color);

// What the panel shows.
struct PanelState {
    int hour = 0, minute = 0;
    bool focus = true;  // Focus phase, otherwise break.
    bool running = false;
    int64_t remainingSeconds = 0;
    float progress = 0.0f;  // Of the current phase, in [0, 1].
};

namespace panel {
constexpr int kWidth = 480;
constexpr int kHeight = 288;
const Color kBackground = Color::Rgba(24, 26, 32, 235);
const Color kClock = Color::Rgba(230, 232, 238);
const Color kFocus = Color::Rgba(232, 131, 58);
const Color kBreak = Color::Rgba(76, 175, 80);
const Color kPaused = Color::Rgba(128, 132, 142);
const Color kTrack = Color::Rgba(60, 62, 70);
// Layout, in pixels.
constexpr int kMargin = 32;
constexpr int kBarX = kMargin, kBarY = 236, kBarWidth = kWidth - 2 * kMargin, kBarHeight = 20;
constexpr DigitStyle kClockStyle{56, 100, 12, 12, 24};
constexpr int kClockY = 24;
constexpr DigitStyle kCountdownStyle{36, 64, 8, 8, 16};
constexpr int kCountdownY = 148;
}  // namespace panel

void RenderPanel(const PanelState& state, Image& image);

// True if both states draw the same image, so the panel need not be redrawn.
// Progress only matters to the pixel of the bar it ends at.
bool SameDisplay(const PanelState& a, const PanelState& b);

// The largest width x height with the panel's aspect ratio that fits in the
// container's front face, in meters.
std::pair<float, float> FitPanel(float boundsWidth, float boundsHeight);
