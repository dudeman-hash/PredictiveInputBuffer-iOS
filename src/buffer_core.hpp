#pragma once

// Allocation-free, game-API-free early-press buffer.
//
// Time unit: PhysicsStep is an opaque, monotonically increasing physics-step
// counter supplied by the caller. It is deliberately not a frame count, time
// in seconds, or PlayerObject::m_currentStep: the integration selects and
// passes the counter at its once-per-physics-step hook.
//
// A pending press with window N is eligible at ages 0..N inclusive. It expires
// before it can fire at age N+1. Thus a press made N physics steps early is
// still replayed, while a press made more than N steps early is not.

#include <cstdint>

namespace predictive_input_buffer {

using PhysicsStep = std::uint64_t;
using BufferTicks = std::uint8_t;

inline constexpr BufferTicks kBufferOff = 0;
inline constexpr BufferTicks kStage1Ticks = 2;
inline constexpr BufferTicks kStage2Ticks = 4;
inline constexpr BufferTicks kStage3Ticks = 8;
inline constexpr BufferTicks kStage4Ticks = 16;

// Maps the existing 1..5 setting stages for non-wave buffering. Invalid input
// maps to off rather than inventing an arbitrary duration.
constexpr BufferTicks ticksForStage(std::uint8_t stage) noexcept {
    switch (stage) {
        case 1: return kStage1Ticks;
        case 2: return kStage2Ticks;
        case 3: return kStage3Ticks;
        case 4: return kStage4Ticks;
        case 5: return kBufferOff;
        default: return kBufferOff;
    }
}

constexpr bool isSupportedWindow(BufferTicks ticks) noexcept {
    return ticks == kBufferOff || ticks == kStage1Ticks ||
           ticks == kStage2Ticks || ticks == kStage3Ticks ||
           ticks == kStage4Ticks;
}

// The adapter performs pushButton first and releaseButton second when both
// bits are set. That only happens for an early tap that was released before
// it became valid; it preserves the tap without leaving native input held.
struct NativeActions {
    bool push = false;
    bool release = false;

    // Telemetry only. bufferedFire is true exactly once for each pending press
    // that fires; it is never true for an immediate or bypassed raw press.
    bool bufferedFire = false;
    bool expired = false;
};

struct ConfigurationResult {
    bool accepted = false;
    NativeActions actions{};
};

// One instance buffers one player/button stream. It has no ownership of a
// PlayerObject and never calls game code. It contains no container, callback,
// allocation, clock, or static mutable state.
class EarlyPressBuffer final {
public:
    constexpr EarlyPressBuffer() noexcept = default;

    // Accepts only 0, 2, 4, 8, or 16. Selecting 0 enters bypass mode and
    // drops any pending press while reconciling native held state to the
    // physical state. A non-zero selection leaves bypass mode.
    [[nodiscard]] ConfigurationResult setWindowTicks(BufferTicks ticks) noexcept {
        if (!isSupportedWindow(ticks)) {
            return {};
        }

        m_windowTicks = ticks;
        ConfigurationResult result{};
        result.accepted = true;
        result.actions = setBypassed(ticks == kBufferOff);
        return result;
    }

    // Use this for the master switch, modes deliberately not buffered, or an
    // adapter handoff. Entering bypass clears pending state and brings the
    // native held state to the current physical state so a held finger is not
    // lost and a released finger cannot remain stuck. A zero tick window is
    // always bypassed even if false is requested here.
    [[nodiscard]] NativeActions setBypassed(bool bypassed) noexcept {
        if (m_windowTicks == kBufferOff) {
            bypassed = true;
        }

        if (bypassed) {
            m_pending = false;
        }
        m_bypassed = bypassed;

        if (m_nativeHeld == m_physicalHeld) {
            return {};
        }

        NativeActions result{};
        if (m_physicalHeld) {
            result.push = true;
        } else {
            result.release = true;
        }
        m_nativeHeld = m_physicalHeld;
        return result;
    }

    // Call on the physical down edge. validNow is the adapter's independently
    // determined game-state validity, not PlayerObject::pushButton's return.
    // Duplicate down notifications are inert.
    [[nodiscard]] NativeActions onPhysicalPress(PhysicsStep step,
                                                 bool validNow) noexcept {
        if (m_physicalHeld) {
            return {};
        }
        m_physicalHeld = true;

        if (m_bypassed || m_windowTicks == kBufferOff) {
            return reconcileNativeToPhysical();
        }

        if (m_nativeHeld) {
            return {};
        }

        if (validNow) {
            m_nativeHeld = true;
            return NativeActions{true, false, false, false};
        }

        // This is a single bounded slot, not an unbounded input queue. A
        // physical edge can create at most one replayable press.
        m_pending = true;
        m_pendingSince = step;
        return {};
    }

    // Call on the physical up edge, including when a press is still pending.
    // A pending early tap intentionally survives this release: when valid it
    // emits push then release in one action bundle. Consequently it replays
    // once but cannot create a stuck native hold.
    [[nodiscard]] NativeActions onPhysicalRelease() noexcept {
        m_physicalHeld = false;
        return reconcileNativeToPhysical();
    }

    // Call once per selected physics step for this player/button stream.
    // validNow should be true only when the adapter's conservative native
    // validity condition is satisfied at that physics step.
    [[nodiscard]] NativeActions onPhysicsStep(PhysicsStep step,
                                               bool validNow) noexcept {
        if (m_bypassed || !m_pending) {
            return {};
        }

        const PhysicsStep age = step - m_pendingSince;
        if (age > m_windowTicks) {
            m_pending = false;
            return NativeActions{false, false, false, true};
        }

        if (!validNow) {
            return {};
        }

        // Clear first, so re-entrancy or another step cannot double-fire.
        m_pending = false;
        m_nativeHeld = true;

        NativeActions result{true, false, true, false};
        if (!m_physicalHeld) {
            // The caller must execute push before release. Final modeled
            // native state is false, matching the physical state.
            result.release = true;
            m_nativeHeld = false;
        }
        return result;
    }

    // Observation-only adapters use this when a native jump/orb path has already
    // acted on a raw press. Cancellation must not change physical/native holds.
    constexpr void cancelPendingPress() noexcept {
        m_pending = false;
    }

    // Hard attempt/lifecycle cleanup: death, restart, level exit, or player
    // destruction. It cancels a pending press, discards physical state for the
    // old attempt, and releases only a native hold this core previously owns.
    [[nodiscard]] NativeActions clearAll() noexcept {
        m_pending = false;
        m_physicalHeld = false;

        if (!m_nativeHeld) {
            return {};
        }

        m_nativeHeld = false;
        return NativeActions{false, true, false, false};
    }

    [[nodiscard]] constexpr BufferTicks windowTicks() const noexcept {
        return m_windowTicks;
    }

    [[nodiscard]] constexpr bool isBypassed() const noexcept {
        return m_bypassed;
    }

    // These intentionally expose two different concepts. physicalHeld tracks
    // the user's current finger/button state. nativeHeld tracks only whether
    // this core has modeled a matching native press not yet released.
    [[nodiscard]] constexpr bool physicalHeld() const noexcept {
        return m_physicalHeld;
    }

    [[nodiscard]] constexpr bool nativeHeld() const noexcept {
        return m_nativeHeld;
    }

    [[nodiscard]] constexpr bool hasPendingPress() const noexcept {
        return m_pending;
    }

    [[nodiscard]] constexpr PhysicsStep pendingSince() const noexcept {
        return m_pendingSince;
    }

private:
    [[nodiscard]] NativeActions reconcileNativeToPhysical() noexcept {
        if (m_nativeHeld == m_physicalHeld) {
            return {};
        }

        if (m_physicalHeld) {
            m_nativeHeld = true;
            return NativeActions{true, false, false, false};
        }

        m_nativeHeld = false;
        return NativeActions{false, true, false, false};
    }

    PhysicsStep m_pendingSince = 0;
    BufferTicks m_windowTicks = kStage1Ticks;
    bool m_pending = false;
    bool m_physicalHeld = false;
    bool m_nativeHeld = false;
    bool m_bypassed = false;
};

} // namespace predictive_input_buffer
