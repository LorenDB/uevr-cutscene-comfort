#pragma once

#include <cstdint>

// Sequences the cutscene "leave VR" transition.
//
// One update advances at most one phase. The fade that just started is given
// a full duration of later ticks before the next phase begins, so a single
// long frame cannot skip the black hold and pop the mode change through.
// The duration is latched when a fade phase begins, so a slider change does
// not shorten a fade that the engine camera fade was already started with.

namespace comfort {

enum class UnvrPhase : std::uint8_t {
    Idle,
    FadeOutWorld,
    Screen,
    FadeOutScreen,
    FadeInWorld,
};

struct UnvrStep {
    UnvrPhase phase{UnvrPhase::Idle};
    bool fade_to_black{false};
    bool fade_from_black{false};
    bool apply_screen{false};
    bool release_screen{false};
    // Duration latched for a fade command issued on this step.
    float fade_seconds{0.2f};
};

class UnvrTransition {
public:
    UnvrStep update(float delta_seconds, bool want_screen, float fade_seconds) {
        if (delta_seconds < 0.0f) {
            delta_seconds = 0.0f;
        }

        UnvrStep step{};
        step.fade_seconds = m_latched;

        switch (m_phase) {
        case UnvrPhase::Idle:
            if (want_screen) {
                begin(UnvrPhase::FadeOutWorld, fade_seconds, step);
                step.fade_to_black = true;
            }
            break;

        case UnvrPhase::FadeOutWorld:
            m_elapsed += delta_seconds;
            if (!want_screen) {
                begin(UnvrPhase::FadeInWorld, fade_seconds, step);
                step.fade_from_black = true;
                step.release_screen = true;
            } else if (m_elapsed >= m_latched) {
                begin(UnvrPhase::Screen, fade_seconds, step);
                step.apply_screen = true;
                step.fade_from_black = true;
            }
            break;

        case UnvrPhase::Screen:
            if (!want_screen) {
                begin(UnvrPhase::FadeOutScreen, fade_seconds, step);
                step.fade_to_black = true;
            }
            break;

        case UnvrPhase::FadeOutScreen:
            m_elapsed += delta_seconds;
            if (want_screen) {
                begin(UnvrPhase::Screen, fade_seconds, step);
                step.fade_from_black = true;
            } else if (m_elapsed >= m_latched) {
                begin(UnvrPhase::FadeInWorld, fade_seconds, step);
                step.release_screen = true;
                step.fade_from_black = true;
            }
            break;

        case UnvrPhase::FadeInWorld:
            m_elapsed += delta_seconds;
            if (want_screen) {
                begin(UnvrPhase::FadeOutWorld, fade_seconds, step);
                step.fade_to_black = true;
            } else if (m_elapsed >= m_latched) {
                begin(UnvrPhase::Idle, fade_seconds, step);
            }
            break;
        }

        step.phase = m_phase;
        return step;
    }

    UnvrPhase phase() const { return m_phase; }
    float elapsed() const { return m_elapsed; }
    float latched_seconds() const { return m_latched; }

    bool screen_is_up() const {
        return m_phase == UnvrPhase::Screen || m_phase == UnvrPhase::FadeOutScreen;
    }

private:
    void begin(UnvrPhase next, float fade_seconds, UnvrStep& step) {
        m_phase = next;
        m_elapsed = 0.0f;
        if (next != UnvrPhase::Screen && next != UnvrPhase::Idle) {
            m_latched = fade_seconds < 0.05f ? 0.05f : fade_seconds;
        }
        step.fade_seconds = m_latched;
    }

    UnvrPhase m_phase{UnvrPhase::Idle};
    float m_elapsed{0.0f};
    float m_latched{0.2f};
};

} // namespace comfort
