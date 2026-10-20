#pragma once

#include <cstdint>

// This file deliberately contains no Geometry Dash movement or collision constants.
// The caller must provide a side-effect-free simulation of the real game state.
namespace wave_decision {

constexpr std::uint8_t kMaxLookaheadTicks = 8;
constexpr std::uint8_t kNoSwitch = 0xff;

enum class Input : std::uint8_t {
    Release = 0,
    Hold = 1,
};

enum class Result : std::uint8_t {
    CurrentSafe,
    BothDie,
    SwitchScheduled,
    InvalidLookahead,
};

struct Decision {
    // False means leave the player's actual input alone.
    bool overrideInput = false;
    Input input = Input::Release;

    // When overrideInput is true, switch to input at the start of this simulated
    // tick. Zero therefore means switch immediately. It is always < lookahead.
    std::uint8_t switchTick = kNoSwitch;
    Result result = Result::InvalidLookahead;
};

constexpr Input opposite(Input input) noexcept {
    return input == Input::Hold ? Input::Release : Input::Hold;
}

// Simulator contract:
//   bool simulate(Input initial, std::uint8_t switchAt, std::uint8_t lookahead)
//
// The simulator returns true only if the complete lookahead survives. switchAt is
// applied at the BEGINNING of that simulated tick; kNoSwitch means never switch.
// It must not mutate live game/world state. It is intentionally a template rather
// than std::function, so this planner itself performs no allocation or dispatch.
template <class Simulator>
[[nodiscard]] inline Decision decide(
    Input current,
    std::uint8_t lookahead,
    Simulator&& simulate
) noexcept(noexcept(simulate(current, kNoSwitch, lookahead))) {
    if (lookahead == 0 || lookahead > kMaxLookaheadTicks) {
        return { false, current, kNoSwitch, Result::InvalidLookahead };
    }

    if (simulate(current, kNoSwitch, lookahead)) {
        return { false, current, kNoSwitch, Result::CurrentSafe };
    }

    const Input other = opposite(current);
    // The user's policy says not to intervene when both constant choices die.
    // Checking this before mixed sequences enforces that rule exactly.
    if (!simulate(other, kNoSwitch, lookahead)) {
        return { false, current, kNoSwitch, Result::BothDie };
    }

    // Keep replacing the candidate: the final surviving tick is the latest one.
    std::uint8_t latest = kNoSwitch;
    for (std::uint8_t tick = 0; tick < lookahead; ++tick) {
        if (simulate(current, tick, lookahead)) {
            latest = tick;
        }
    }

    // A compliant simulator makes tick zero equivalent to the already-surviving
    // constant other input. Retaining this guard makes failure safe if it does not.
    if (latest == kNoSwitch) {
        return { false, current, kNoSwitch, Result::BothDie };
    }
    return { true, other, latest, Result::SwitchScheduled };
}

} // namespace wave_decision
