#include "wave_decision.hpp"

#include <cassert>

using wave_decision::Decision;
using wave_decision::Input;
using wave_decision::Result;
using wave_decision::kNoSwitch;

static void current_safe_is_unchanged() {
    int calls = 0;
    const auto simulate = [&calls](Input initial, std::uint8_t switchAt,
                                   std::uint8_t) noexcept {
        ++calls;
        return initial == Input::Hold && switchAt == kNoSwitch;
    };

    const Decision d = wave_decision::decide(Input::Hold, 8, simulate);
    assert(!d.overrideInput);
    assert(d.input == Input::Hold);
    assert(d.switchTick == kNoSwitch);
    assert(d.result == Result::CurrentSafe);
    assert(calls == 1); // Other alternatives must not be queried.
}

static void both_constant_inputs_die_is_unchanged() {
    const auto simulate = [](Input, std::uint8_t, std::uint8_t) noexcept {
        return false;
    };

    const Decision d = wave_decision::decide(Input::Hold, 4, simulate);
    assert(!d.overrideInput);
    assert(d.input == Input::Hold);
    assert(d.switchTick == kNoSwitch);
    assert(d.result == Result::BothDie);
}

static void latest_surviving_switch_is_selected() {
    const auto simulate = [](Input initial, std::uint8_t switchAt,
                             std::uint8_t lookahead) noexcept {
        assert(lookahead == 5);
        // Constant RELEASE survives. Starting HOLD is recoverable only through
        // ticks 0..3; tick 3 must win over every earlier feasible correction.
        if (initial == Input::Release && switchAt == kNoSwitch) {
            return true;
        }
        return initial == Input::Hold && switchAt != kNoSwitch && switchAt <= 3;
    };

    const Decision d = wave_decision::decide(Input::Hold, 5, simulate);
    assert(d.overrideInput);
    assert(d.input == Input::Release);
    assert(d.switchTick == 3);
    assert(d.result == Result::SwitchScheduled);
}

static void immediate_switch_at_zero_is_supported() {
    const auto simulate = [](Input initial, std::uint8_t switchAt,
                             std::uint8_t lookahead) noexcept {
        assert(lookahead == 4);
        if (initial == Input::Release && switchAt == kNoSwitch) {
            return true;
        }
        return initial == Input::Hold && switchAt == 0;
    };

    const Decision d = wave_decision::decide(Input::Hold, 4, simulate);
    assert(d.overrideInput);
    assert(d.input == Input::Release);
    assert(d.switchTick == 0);
    assert(d.result == Result::SwitchScheduled);
}

int main() {
    current_safe_is_unchanged();
    both_constant_inputs_die_is_unchanged();
    latest_surviving_switch_is_selected();
    immediate_switch_at_zero_is_supported();
}
