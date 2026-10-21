#pragma once

// Deterministic, allocation-free trajectory kernel. This code deliberately
// knows nothing about Geometry Dash's native constants or object IDs. The iOS
// adapter must provide a per-step physics profile and conservative object
// snapshots from the exact game version before using its decisions in play.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace trajectory_prediction {

struct Vec2 { double x = 0.0, y = 0.0; };
struct Rect { double left = 0.0, bottom = 0.0, right = 0.0, top = 0.0; };

enum class Mode : std::uint8_t { Cube, Wave, Ship, Ball, Ufo, Robot, Spider, Swing };
enum class ObjectKind : std::uint8_t { Hazard, Solid, Orb, Pad, GravityPortal, SpeedPortal };

struct Object {
    ObjectKind kind = ObjectKind::Hazard;
    Rect bounds{};
    // Orb/pad impulse in native velocity units. Speed portals use value as a
    // multiplier. Gravity portal values are +1=inverted, -1=normal, 0=toggle.
    double value = 0.0;
    bool enabled = true;
};

struct State {
    Vec2 position{};
    Vec2 velocity{};
    double halfWidth = 0.0;
    double halfHeight = 0.0;
    double gravity = -1.0; // +1 accelerates toward +Y; -1 toward -Y.
    double speed = 1.0;
    Mode mode = Mode::Cube;
    bool grounded = false;
    bool alive = true;
};

struct PhysicsProfile {
    // Velocity and acceleration use native PlayerObject units. timeStep is
    // copied from PlayerObject::update(dt); substeps split that native step.
    double gravityAcceleration = 0.0;
    double horizontalSpeed = 0.0;
    double jumpImpulse = 0.0;
    double waveVerticalSpeed = 0.0;
    double maxAbsVerticalSpeed = 0.0;
    double defaultOrbImpulse = 0.0;
    double defaultPadImpulse = 0.0;
    double timeStep = 1.0;
    std::uint8_t substeps = 4;
};

struct InputFrame {
    bool held = false;
    bool pressed = false; // rising edge; used to activate an orb once
};

struct StepResult {
    State state{};
    bool orbActivated = false;
    bool padActivated = false;
    bool portalActivated = false;
};

constexpr bool overlaps(Rect const& a, Rect const& b) noexcept {
    return a.left <= b.right && a.right >= b.left &&
           a.bottom <= b.top && a.top >= b.bottom;
}

inline Rect playerBounds(State const& s) noexcept {
    return {s.position.x - s.halfWidth, s.position.y - s.halfHeight,
            s.position.x + s.halfWidth, s.position.y + s.halfHeight};
}

inline bool containsPoint(Rect const& r, Vec2 p) noexcept {
    return p.x >= r.left && p.x <= r.right && p.y >= r.bottom && p.y <= r.top;
}

// Advance one caller-defined simulation step. Objects are sampled from a
// caller-owned array; no allocations or game callbacks occur here. Trigger
// objects are processed in array order, so the adapter should pass them sorted
// by distance along the predicted segment. Hazards are checked after movement.
inline StepResult advance(State initial, InputFrame input, PhysicsProfile const& p,
                          Object const* objects, std::uint32_t objectCount) noexcept {
    StepResult out{};
    out.state = initial;
    auto& s = out.state;
    if (!s.alive || p.substeps == 0) return out;

    if (!std::isfinite(p.timeStep) || p.timeStep <= 0.0) return out;
    const double dt = p.timeStep / static_cast<double>(p.substeps);
    const double speedX = p.horizontalSpeed * s.speed;
    const bool directionalMode = s.mode == Mode::Wave;
    if (directionalMode) {
        s.velocity.x = speedX;
        s.velocity.y = (input.held ? 1.0 : -1.0) * p.waveVerticalSpeed * s.gravity;
    } else {
        s.velocity.x = speedX;
        if (input.pressed && s.grounded &&
            (s.mode == Mode::Cube || s.mode == Mode::Robot)) {
            s.velocity.y = -s.gravity * p.jumpImpulse;
            s.grounded = false;
        }
    }

    for (std::uint8_t sub = 0; sub < p.substeps && s.alive; ++sub) {
        const Vec2 before = s.position;
        if (!directionalMode && !input.pressed) {
            s.velocity.y += s.gravity * p.gravityAcceleration * dt;
            if (p.maxAbsVerticalSpeed > 0.0)
                s.velocity.y = std::clamp(s.velocity.y, -p.maxAbsVerticalSpeed,
                                         p.maxAbsVerticalSpeed);
        }
        s.position.x += s.velocity.x * dt;
        s.position.y += s.velocity.y * dt;

        // Trigger checks use swept point sampling at each substep. This avoids
        // skipping small triggers at ordinary speeds; native hitboxes remain an
        // adapter responsibility.
        for (std::uint32_t i = 0; i < objectCount; ++i) {
            auto const& o = objects[i];
            if (!o.enabled || o.kind == ObjectKind::Hazard || o.kind == ObjectKind::Solid)
                continue;
            Rect expanded{o.bounds.left - s.halfWidth, o.bounds.bottom - s.halfHeight,
                          o.bounds.right + s.halfWidth, o.bounds.top + s.halfHeight};
            if (!containsPoint(expanded, s.position)) continue;
            switch (o.kind) {
                case ObjectKind::Orb:
                    if (input.pressed && !out.orbActivated) {
                        s.velocity.y = -s.gravity * (o.value != 0.0 ? o.value : p.defaultOrbImpulse);
                        s.grounded = false;
                        out.orbActivated = true;
                    }
                    break;
                case ObjectKind::Pad:
                    if (!out.padActivated) {
                        s.velocity.y = -s.gravity * (o.value != 0.0 ? o.value : p.defaultPadImpulse);
                        s.grounded = false;
                        out.padActivated = true;
                    }
                    break;
                case ObjectKind::GravityPortal:
                    if (!out.portalActivated) {
                        if (o.value == 0.0) s.gravity = -s.gravity;
                        else s.gravity = o.value > 0.0 ? 1.0 : -1.0;
                        s.velocity.y = -s.velocity.y;
                        out.portalActivated = true;
                    }
                    break;
                case ObjectKind::SpeedPortal:
                    if (!out.portalActivated && o.value > 0.0) {
                        s.speed *= o.value;
                        out.portalActivated = true;
                    }
                    break;
                default: break;
            }
        }

        Rect swept{std::min(before.x, s.position.x) - s.halfWidth,
                   std::min(before.y, s.position.y) - s.halfHeight,
                   std::max(before.x, s.position.x) + s.halfWidth,
                   std::max(before.y, s.position.y) + s.halfHeight};
        s.grounded = false;
        for (std::uint32_t i = 0; i < objectCount; ++i) {
            auto const& o = objects[i];
            if (!o.enabled) continue;
            if (o.kind == ObjectKind::Hazard && overlaps(swept, o.bounds)) {
                s.alive = false;
                break;
            }
            if (o.kind != ObjectKind::Solid || !overlaps(playerBounds(s), o.bounds)) continue;

            // Axis-aligned cube landing and head contacts. This intentionally
            // does not model walls, slopes, rotated blocks, one-way blocks,
            // breakable blocks or native snap thresholds; live adapters must
            // fail closed when any of those can affect this prediction.
            Rect const beforeBounds{before.x - s.halfWidth, before.y - s.halfHeight,
                                   before.x + s.halfWidth, before.y + s.halfHeight};
            Rect const afterBounds = playerBounds(s);
            const double eps = 1e-6;
            if (s.gravity < 0.0 && s.velocity.y <= 0.0 &&
                beforeBounds.bottom >= o.bounds.top - eps && afterBounds.bottom <= o.bounds.top + eps &&
                afterBounds.right > o.bounds.left && afterBounds.left < o.bounds.right) {
                s.position.y = o.bounds.top + s.halfHeight;
                s.velocity.y = 0.0;
                s.grounded = true;
            } else if (s.gravity > 0.0 && s.velocity.y >= 0.0 &&
                       beforeBounds.top <= o.bounds.bottom + eps && afterBounds.top >= o.bounds.bottom - eps &&
                       afterBounds.right > o.bounds.left && afterBounds.left < o.bounds.right) {
                s.position.y = o.bounds.bottom - s.halfHeight;
                s.velocity.y = 0.0;
                s.grounded = true;
            } else if (overlaps(afterBounds, o.bounds)) {
                s.alive = false; // unsupported side/underside/embedded contact
                break;
            }
        }
    }
    s.grounded = s.grounded && s.alive;
    return out;
}

} // namespace trajectory_prediction

namespace trajectory_prediction {

struct ClickDecision {
    bool delay = false;
    std::uint8_t delayTicks = 0;
    std::uint8_t testedCandidates = 0;
};

// Test a tap at the current frame (delay 0), then each later frame through the
// selected buffer window. Returns the latest surviving candidate only if the
// immediate candidate dies. A fully specified initial state and frozen object
// snapshot are mandatory. The live adapter currently calls this only for its
// narrow classic-cube/static-geometry subset; this does not imply a full native
// collision simulator or support for orb/pad and other game modes.
template <class Stepper>
[[nodiscard]] inline ClickDecision latestSurvivingClick(
    std::uint8_t maxDelay, std::uint8_t lookahead, Stepper&& stepper) noexcept {
    if (lookahead == 0 || lookahead > 64 || maxDelay > 64 || maxDelay >= lookahead)
        return {};
    bool const immediateSurvives = stepper(std::uint8_t{0}, lookahead);
    if (immediateSurvives) return {false, 0, 1};
    bool found = false;
    std::uint8_t latest = 0;
    std::uint8_t tested = 1;
    for (std::uint8_t delay = 1; delay <= maxDelay; ++delay) {
        ++tested;
        if (stepper(delay, lookahead)) {
            found = true;
            latest = delay;
        }
    }
    return {found, latest, tested};
}

// Run a candidate tap through a deterministic world snapshot. The caller's
// object array must contain correctly transformed world-space bounds in the
// same coordinate system as state. A tap generates one pressed edge and stays
// held for holdTicks, allowing orb activation and held-state movement tests.
inline bool survivesClick(State state, PhysicsProfile const& profile,
                          Object const* objects, std::uint32_t objectCount,
                          std::uint8_t delay, std::uint8_t lookahead,
                          std::uint8_t holdTicks) noexcept {
    if (lookahead == 0 || delay >= lookahead) return false;
    for (std::uint8_t tick = 0; tick < lookahead; ++tick) {
        if (tick == delay && !state.grounded) return false;
        bool const active = tick >= delay &&
            static_cast<std::uint16_t>(tick - delay) < holdTicks;
        auto const result = advance(state, {active, tick == delay}, profile,
                                    objects, objectCount);
        state = result.state;
        if (!state.alive) return false;
    }
    return state.alive;
}

} // namespace trajectory_prediction
