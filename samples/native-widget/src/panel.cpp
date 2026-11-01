// Copyright (c) 2026 Hiroshi Matsuyama
// SPDX-License-Identifier: MIT

#include "panel.h"

#include <algorithm>
#include <cmath>
#include <cstring>

void Image::Clear(Color color) {
    std::fill(pixels_.begin(), pixels_.end(), color);
}

void Image::FillRect(int x, int y, int w, int h, Color color) {
    const int x0 = std::max(x, 0), y0 = std::max(y, 0);
    const int x1 = std::min(x + w, width_), y1 = std::min(y + h, height_);
    for (int py = y0; py < y1; ++py) {
        std::fill(pixels_.begin() + static_cast<ptrdiff_t>(py) * width_ + x0,
                  pixels_.begin() + static_cast<ptrdiff_t>(py) * width_ + x1, color);
    }
}

void Image::FillRoundedRect(int x, int y, int w, int h, int radius, Color color) {
    radius = std::min({radius, w / 2, h / 2});
    for (int row = 0; row < h; ++row) {
        // Inset of this row at the rounded corners.
        // Columns outside the corner circle, measured from the circle's center.
        int inset = 0;
        const int fromEdge = std::min(row, h - 1 - row);
        if (fromEdge < radius) {
            const int dy = radius - fromEdge;
            while (inset < radius && (radius - inset) * (radius - inset) + dy * dy > radius * radius) ++inset;
        }
        FillRect(x + inset, y + row, w - 2 * inset, 1, color);
    }
}

namespace {

// Segments a..g as bits 0..6.
constexpr uint8_t kSegments[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

// 5x7 glyphs, one byte per row, the five low bits from left to right.
struct Glyph {
    char c;
    uint8_t rows[7];
};
constexpr Glyph kFont[] = {
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}, {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}}, {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}}, {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}, {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}}, {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
};

const Glyph* FindGlyph(char c) {
    for (const Glyph& glyph : kFont) {
        if (glyph.c == c) return &glyph;
    }
    return nullptr;
}

}  // namespace

void DrawDigit(Image& image, int digit, int x, int y, int w, int h, int t, Color color) {
    if (digit < 0 || digit > 9) return;
    const uint8_t segments = kSegments[digit];
    const int mid = y + h / 2;
    const int upper = mid - t / 2 - (y + t);                                       // Height of the upper vertical segments.
    const int lower = (y + h - t) - (mid + t / 2);                                 // Height of the lower ones.
    if (segments & 0x01) image.FillRect(x + t, y, w - 2 * t, t, color);            // a
    if (segments & 0x02) image.FillRect(x + w - t, y + t, t, upper, color);        // b
    if (segments & 0x04) image.FillRect(x + w - t, mid + t / 2, t, lower, color);  // c
    if (segments & 0x08) image.FillRect(x + t, y + h - t, w - 2 * t, t, color);    // d
    if (segments & 0x10) image.FillRect(x, mid + t / 2, t, lower, color);          // e
    if (segments & 0x20) image.FillRect(x, y + t, t, upper, color);                // f
    if (segments & 0x40) image.FillRect(x + t, mid - t / 2, w - 2 * t, t, color);  // g
}

void DrawColon(Image& image, int x, int y, int w, int h, int t, Color color) {
    const int cx = x + (w - t) / 2;
    image.FillRect(cx, y + h / 3 - t / 2, t, t, color);
    image.FillRect(cx, y + 2 * h / 3 - t / 2, t, t, color);
}

int DrawText(Image& image, const std::string& text, int x, int y, int scale, Color color) {
    const int advance = 6 * scale;
    for (size_t i = 0; i < text.size(); ++i) {
        const Glyph* glyph = FindGlyph(text[i]);
        if (glyph == nullptr) continue;
        const int gx = x + static_cast<int>(i) * advance;
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if (glyph->rows[row] & (0x10 >> col)) image.FillRect(gx + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
    return text.empty() ? 0 : static_cast<int>(text.size()) * advance - scale;
}

void DrawTwoPairs(Image& image, int left, int right, int x, int y, const DigitStyle& s, Color color) {
    left = std::clamp(left, 0, 99);
    right = std::clamp(right, 0, 99);
    int cx = x;
    auto digit = [&](int d) {
        DrawDigit(image, d, cx, y, s.digitWidth, s.digitHeight, s.thickness, color);
        cx += s.digitWidth;
    };
    digit(left / 10);
    cx += s.gap;
    digit(left % 10);
    cx += s.gap;
    DrawColon(image, cx, y, s.colonWidth, s.digitHeight, s.thickness, color);
    cx += s.colonWidth + s.gap;
    digit(right / 10);
    cx += s.gap;
    digit(right % 10);
}

void RenderPanel(const PanelState& state, Image& image) {
    using namespace panel;
    image.Clear();
    image.FillRoundedRect(0, 0, image.width(), image.height(), 24, kBackground);

    DrawTwoPairs(image, state.hour, state.minute, (kWidth - kClockStyle.Width()) / 2, kClockY, kClockStyle, kClock);

    const Color accent = !state.running ? kPaused : state.focus ? kFocus : kBreak;
    DrawText(image, state.focus ? "FOCUS" : "BREAK", kMargin, kCountdownY, 4, state.focus ? kFocus : kBreak);
    if (!state.running) DrawText(image, "PAUSED", kMargin, kCountdownY + 40, 3, kPaused);

    const int64_t seconds = std::max<int64_t>(state.remainingSeconds, 0);
    DrawTwoPairs(image, static_cast<int>(std::min<int64_t>(seconds / 60, 99)), static_cast<int>(seconds % 60),
                 kWidth - kMargin - kCountdownStyle.Width(), kCountdownY, kCountdownStyle, accent);

    image.FillRoundedRect(kBarX, kBarY, kBarWidth, kBarHeight, kBarHeight / 2, kTrack);
    const int filled = static_cast<int>(std::lround(static_cast<float>(kBarWidth) * std::clamp(state.progress, 0.0f, 1.0f)));
    if (filled > 0) image.FillRoundedRect(kBarX, kBarY, filled, kBarHeight, kBarHeight / 2, accent);
}

bool SameDisplay(const PanelState& a, const PanelState& b) {
    auto barEnd = [](float progress) {
        return static_cast<int>(std::lround(static_cast<float>(panel::kBarWidth) * std::clamp(progress, 0.0f, 1.0f)));
    };
    return a.hour == b.hour && a.minute == b.minute && a.focus == b.focus && a.running == b.running &&
           a.remainingSeconds == b.remainingSeconds && barEnd(a.progress) == barEnd(b.progress);
}

std::pair<float, float> FitPanel(float boundsWidth, float boundsHeight) {
    const float aspect = static_cast<float>(panel::kWidth) / static_cast<float>(panel::kHeight);
    if (boundsWidth <= 0.0f || boundsHeight <= 0.0f) return {0.0f, 0.0f};
    if (boundsWidth / boundsHeight > aspect) return {boundsHeight * aspect, boundsHeight};
    return {boundsWidth, boundsWidth / aspect};
}
