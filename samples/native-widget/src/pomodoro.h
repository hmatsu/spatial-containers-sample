// Copyright (c) 2026 Hiroshi Matsuyama
// SPDX-License-Identifier: MIT
//
// Pomodoro timer: focus and break phases that alternate. Time is passed in, so the
// logic is independent of any clock and can be unit tested.

#pragma once

#include <chrono>
#include <cstdint>

class Pomodoro {
public:
    using Duration = std::chrono::milliseconds;

    enum class Phase : uint8_t { Focus, Break };

    Pomodoro(Duration focus, Duration breakTime) : focus_(focus), break_(breakTime), remaining_(focus) {}

    Phase phase() const { return phase_; }
    bool running() const { return running_; }
    Duration remaining() const { return remaining_; }
    Duration phaseLength() const { return phase_ == Phase::Focus ? focus_ : break_; }
    int completedFocusPhases() const { return completedFocus_; }

    // Fraction of the current phase that has passed, in [0, 1].
    float progress() const {
        const auto length = phaseLength().count();
        return length > 0 ? 1.0f - static_cast<float>(remaining_.count()) / static_cast<float>(length) : 1.0f;
    }

    void toggle() { running_ = !running_; }

    // Back to the start of a focus phase, paused.
    void reset() {
        phase_ = Phase::Focus;
        remaining_ = focus_;
        running_ = false;
    }

    // Advances by `elapsed` while running. Returns true if a phase ended. A long step
    // may cross several phases.
    bool advance(Duration elapsed) {
        if (!running_) return false;
        bool changed = false;
        while (elapsed >= remaining_) {
            elapsed -= remaining_;
            if (phase_ == Phase::Focus) ++completedFocus_;
            phase_ = phase_ == Phase::Focus ? Phase::Break : Phase::Focus;
            remaining_ = phaseLength();
            changed = true;
            if (remaining_.count() <= 0) break;  // Degenerate lengths: stop rather than spin.
        }
        if (remaining_ > elapsed) remaining_ -= elapsed;
        return changed;
    }

    // Whole seconds left, rounded up, as shown on the display.
    int64_t displaySeconds() const { return (remaining_.count() + 999) / 1000; }

private:
    Duration focus_;
    Duration break_;
    Phase phase_ = Phase::Focus;
    Duration remaining_;
    bool running_ = false;
    int completedFocus_ = 0;
};
