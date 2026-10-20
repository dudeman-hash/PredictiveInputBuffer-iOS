#include "trajectory_simulator.hpp"
#include <cassert>
#include <cmath>
#include <iostream>
using namespace trajectory_prediction;

static bool near(double a, double b) { return std::abs(a - b) < 1e-9; }

int main() {
    // A currently safe click is never delayed.
    auto safeNow = latestSurvivingClick(4, 8, [](std::uint8_t delay, std::uint8_t) {
        return delay == 0;
    });
    assert(!safeNow.delay && safeNow.testedCandidates == 1);

    // Immediate input dies; choose the latest safe delay within the window.
    auto delayed = latestSurvivingClick(4, 8, [](std::uint8_t delay, std::uint8_t) {
        return delay == 2 || delay == 4;
    });
    assert(delayed.delay && delayed.delayTicks == 4 && delayed.testedCandidates == 5);
    auto none = latestSurvivingClick(3, 8, [](std::uint8_t, std::uint8_t) { return false; });
    assert(!none.delay);

    PhysicsProfile profile{};
    profile.horizontalSpeed = 1.0;
    profile.gravityAcceleration = 0.0;
    profile.jumpImpulse = 3.0;
    profile.defaultOrbImpulse = 5.0;
    profile.defaultPadImpulse = 7.0;
    profile.waveVerticalSpeed = 2.0;
    profile.substeps = 1;

    State start{};
    start.halfWidth = 0.1;
    start.halfHeight = 0.1;

    Object hazard{ObjectKind::Hazard, {0.8, -0.2, 1.2, 0.2}, 0, true};
    auto hit = advance(start, {}, profile, &hazard, 1);
    assert(!hit.state.alive);

    // This synthetic ceiling hazard kills the immediate jump but not a tap
    // delayed until the player has passed it. The planner selects the latest
    // surviving tick in the configured window.
    State clickStart{};
    clickStart.halfWidth = 0.1;
    clickStart.halfHeight = 0.1;
    PhysicsProfile clickPhysics = profile;
    clickPhysics.jumpImpulse = 3.0;
    Object ceiling{ObjectKind::Hazard, {0.5, 2.0, 1.5, 4.0}, 0, true};
    auto clickChoice = latestSurvivingClick(2, 5,
        [&](std::uint8_t delay, std::uint8_t horizon) {
            return survivesClick(clickStart, clickPhysics, &ceiling, 1,
                                 delay, horizon, 1);
        });
    assert(clickChoice.delay && clickChoice.delayTicks == 2);

    Object orb{ObjectKind::Orb, {0.5, -1.0, 1.5, 5.0}, 0, true};
    auto tappedOrb = advance(start, {true, true}, profile, &orb, 1);
    assert(tappedOrb.orbActivated && near(tappedOrb.state.velocity.y, 5.0));
    auto missedOrb = advance(start, {false, false}, profile, &orb, 1);
    assert(!missedOrb.orbActivated);

    Object pad{ObjectKind::Pad, {0.5, -1.0, 1.5, 1.0}, 0, true};
    auto bounced = advance(start, {}, profile, &pad, 1);
    assert(bounced.padActivated && near(bounced.state.velocity.y, 7.0));

    Object gravity{ObjectKind::GravityPortal, {0.5, -1.0, 1.5, 1.0}, 0, true};
    auto inverted = advance(start, {}, profile, &gravity, 1);
    assert(inverted.portalActivated && near(inverted.state.gravity, 1.0));

    Object speed{ObjectKind::SpeedPortal, {0.5, -1.0, 1.5, 1.0}, 2.0, true};
    auto accelerated = advance(start, {}, profile, &speed, 1);
    assert(accelerated.portalActivated && near(accelerated.state.speed, 2.0));

    State wave = start;
    wave.mode = Mode::Wave;
    wave.gravity = 1.0;
    auto rising = advance(wave, {true, false}, profile, nullptr, 0);
    auto falling = advance(wave, {false, false}, profile, nullptr, 0);
    assert(rising.state.position.y > start.position.y);
    assert(falling.state.position.y < start.position.y);

    std::cout << "Trajectory kernel: 10 checks passed\n";
}
