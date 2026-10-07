#include "UnvrTransition.hpp"

#include <cstdio>

namespace {

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void test_enter_and_leave() {
    comfort::UnvrTransition transition{};

    auto step = transition.update(0.016f, true, 0.20f);
    expect(step.phase == comfort::UnvrPhase::FadeOutWorld, "enter starts the world fade");
    expect(step.fade_to_black, "enter fades to black");
    expect(!step.apply_screen, "the screen waits until the fade finishes");
    expect(step.fade_seconds == 0.20f, "fade command uses the requested duration");

    step = transition.update(0.19f, true, 0.20f);
    expect(step.phase == comfort::UnvrPhase::FadeOutWorld, "a short tick stays in the world fade");
    expect(!step.apply_screen, "the screen is not applied early");

    step = transition.update(0.02f, true, 0.05f);
    expect(step.phase == comfort::UnvrPhase::Screen, "the latched duration completes the world fade");
    expect(step.apply_screen, "the screen is applied at black");
    expect(step.fade_from_black, "the screen fades in from black");
    expect(step.fade_seconds == 0.20f, "the fade-in keeps the duration the fade-out started with");
    expect(!step.release_screen, "apply does not restore");
    expect(transition.screen_is_up(), "the screen phase counts as up");

    step = transition.update(1.0f, true, 0.20f);
    expect(step.phase == comfort::UnvrPhase::Screen, "the screen stays up while wanted");
    expect(!step.fade_to_black && !step.apply_screen, "a held screen issues no new command");

    step = transition.update(0.0f, false, 0.20f);
    expect(step.phase == comfort::UnvrPhase::FadeOutScreen, "leaving starts the screen fade");
    expect(step.fade_to_black, "leaving fades the screen to black");
    expect(transition.screen_is_up(), "the screen stays applied while it fades out");

    step = transition.update(0.20f, false, 0.20f);
    expect(step.phase == comfort::UnvrPhase::FadeInWorld, "the world returns after the screen fade");
    expect(step.release_screen, "UEVR settings are restored at black");
    expect(step.fade_from_black, "the world fades in from black");
    expect(!transition.screen_is_up(), "the screen is down once VR returns");

    step = transition.update(0.20f, false, 0.20f);
    expect(step.phase == comfort::UnvrPhase::Idle, "the transition ends idle");
    expect(!step.release_screen && !step.fade_from_black, "idle issues no extra command");
}

void test_abort_before_the_screen() {
    comfort::UnvrTransition transition{};
    transition.update(0.0f, true, 0.20f);
    auto step = transition.update(0.05f, false, 0.20f);
    expect(step.phase == comfort::UnvrPhase::FadeInWorld, "a cancelled entry returns to VR");
    expect(step.release_screen, "a cancelled entry releases anything it held");
    expect(!step.apply_screen, "a cancelled entry never applies the screen");
    expect(step.fade_from_black, "a cancelled entry fades back in");
}

void test_reenter_during_exit() {
    comfort::UnvrTransition transition{};
    transition.update(0.0f, true, 0.15f);
    transition.update(0.15f, true, 0.15f);
    transition.update(0.0f, false, 0.15f);

    auto step = transition.update(0.04f, true, 0.15f);
    expect(step.phase == comfort::UnvrPhase::Screen, "a new scene during the exit fade keeps the screen");
    expect(step.fade_from_black, "the screen fades back up");
    expect(!step.release_screen, "re-entry does not restore UEVR settings");
    expect(!step.apply_screen, "the screen is already applied");
}

void test_reenter_while_vr_is_fading_back() {
    comfort::UnvrTransition transition{};
    transition.update(0.0f, true, 0.15f);
    transition.update(0.15f, true, 0.15f);
    transition.update(0.0f, false, 0.15f);
    transition.update(0.15f, false, 0.15f);
    expect(transition.phase() == comfort::UnvrPhase::FadeInWorld, "setup reaches the VR fade-in");

    auto step = transition.update(0.01f, true, 0.30f);
    expect(step.phase == comfort::UnvrPhase::FadeOutWorld, "a new scene during the return fades out again");
    expect(step.fade_to_black, "re-entry fades to black");
    expect(step.fade_seconds == 0.30f, "the new fade latches the current duration");
    expect(!step.apply_screen, "the screen waits for the new fade");
}

void test_duration_is_latched() {
    comfort::UnvrTransition transition{};
    transition.update(0.0f, true, 0.20f);
    auto step = transition.update(0.10f, true, 0.05f);
    expect(step.phase == comfort::UnvrPhase::FadeOutWorld, "a shorter slider does not finish the latched fade");
    expect(transition.latched_seconds() == 0.20f, "the in-progress fade keeps its duration");
    step = transition.update(0.10f, true, 0.05f);
    expect(step.apply_screen, "the original duration still completes the fade");
}

void test_huge_delta_advances_one_phase() {
    comfort::UnvrTransition transition{};
    transition.update(0.0f, true, 0.15f);
    auto step = transition.update(5.0f, true, 0.15f);
    expect(step.phase == comfort::UnvrPhase::Screen, "a hitch applies the screen once");
    expect(step.apply_screen && step.fade_from_black, "a hitch still requests the fade-in");
    expect(transition.elapsed() == 0.0f, "the leftover hitch time does not fill the next phase");
}

} // namespace

int main() {
    test_enter_and_leave();
    test_abort_before_the_screen();
    test_reenter_during_exit();
    test_reenter_while_vr_is_fading_back();
    test_duration_is_latched();
    test_huge_delta_advances_one_phase();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d transition checks failed\n", g_failures);
        return 1;
    }

    std::printf("unvr transition checks passed\n");
    return 0;
}
