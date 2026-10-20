#include "buffer_core.hpp"

#include <cstdio>
#include <cstdlib>

namespace pib = predictive_input_buffer;

namespace {

int g_checks = 0;

[[noreturn]] void fail(char const* expression, char const* file, int line) {
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
    std::exit(1);
}

#define CHECK(expression) \
    do { \
        ++g_checks; \
        if (!(expression)) fail(#expression, __FILE__, __LINE__); \
    } while (false)

void checkNoAction(pib::NativeActions action) {
    CHECK(!action.push);
    CHECK(!action.release);
    CHECK(!action.bufferedFire);
    CHECK(!action.expired);
}

void testStageWindows() {
    CHECK(pib::ticksForStage(1) == 2);
    CHECK(pib::ticksForStage(2) == 4);
    CHECK(pib::ticksForStage(3) == 8);
    CHECK(pib::ticksForStage(4) == 16);
    CHECK(pib::ticksForStage(5) == 0);
    CHECK(pib::ticksForStage(0) == 0);
    CHECK(pib::ticksForStage(6) == 0);
    CHECK(pib::isSupportedWindow(0));
    CHECK(pib::isSupportedWindow(2));
    CHECK(pib::isSupportedWindow(4));
    CHECK(pib::isSupportedWindow(8));
    CHECK(pib::isSupportedWindow(16));
    CHECK(!pib::isSupportedWindow(1));
    CHECK(!pib::isSupportedWindow(3));
    CHECK(!pib::isSupportedWindow(17));
}

void testImmediateNativePressAndRelease() {
    pib::EarlyPressBuffer buffer;

    const auto down = buffer.onPhysicalPress(10, true);
    CHECK(down.push);
    CHECK(!down.release);
    CHECK(!down.bufferedFire);
    CHECK(buffer.physicalHeld());
    CHECK(buffer.nativeHeld());
    CHECK(!buffer.hasPendingPress());

    // Duplicate physical-down notification must not make a second native press.
    checkNoAction(buffer.onPhysicalPress(10, true));

    const auto up = buffer.onPhysicalRelease();
    CHECK(!up.push);
    CHECK(up.release);
    CHECK(!up.bufferedFire);
    CHECK(!buffer.physicalHeld());
    CHECK(!buffer.nativeHeld());
}

void testInclusiveBoundAndOneShotFire() {
    pib::EarlyPressBuffer buffer;
    const auto configured = buffer.setWindowTicks(2);
    CHECK(configured.accepted);
    checkNoAction(configured.actions);

    checkNoAction(buffer.onPhysicalPress(100, false));
    CHECK(buffer.physicalHeld());
    CHECK(!buffer.nativeHeld());
    CHECK(buffer.hasPendingPress());
    CHECK(buffer.pendingSince() == 100);

    // Still early one step later.
    checkNoAction(buffer.onPhysicsStep(101, false));
    CHECK(buffer.hasPendingPress());

    // Age == window is eligible: a press made two steps early fires here.
    const auto fire = buffer.onPhysicsStep(102, true);
    CHECK(fire.push);
    CHECK(!fire.release);
    CHECK(fire.bufferedFire);
    CHECK(!fire.expired);
    CHECK(!buffer.hasPendingPress());
    CHECK(buffer.physicalHeld());
    CHECK(buffer.nativeHeld());

    // It cannot fire twice at later physics steps.
    checkNoAction(buffer.onPhysicsStep(103, true));
    CHECK(buffer.nativeHeld());
}

void testExpiryForEverySupportedWindow() {
    constexpr pib::BufferTicks windows[] = {2, 4, 8, 16};

    for (const auto window : windows) {
        pib::EarlyPressBuffer buffer;
        const auto configured = buffer.setWindowTicks(window);
        CHECK(configured.accepted);

        constexpr pib::PhysicsStep start = 1000;
        checkNoAction(buffer.onPhysicalPress(start, false));
        CHECK(buffer.hasPendingPress());

        // At the inclusive boundary it may remain pending if still invalid.
        checkNoAction(buffer.onPhysicsStep(start + window, false));
        CHECK(buffer.hasPendingPress());

        // It is expired before validity can be acted on at N+1.
        const auto expired = buffer.onPhysicsStep(start + window + 1, true);
        CHECK(!expired.push);
        CHECK(!expired.release);
        CHECK(!expired.bufferedFire);
        CHECK(expired.expired);
        CHECK(!buffer.hasPendingPress());
        CHECK(buffer.physicalHeld());
        CHECK(!buffer.nativeHeld());
    }
}

void testEarlyTapReplaysWithoutStuckHold() {
    pib::EarlyPressBuffer buffer;
    CHECK(buffer.setWindowTicks(4).accepted);

    checkNoAction(buffer.onPhysicalPress(200, false));
    CHECK(buffer.physicalHeld());
    CHECK(!buffer.nativeHeld());

    // Release before game-state validity: do not cancel the remembered tap.
    checkNoAction(buffer.onPhysicalRelease());
    CHECK(!buffer.physicalHeld());
    CHECK(!buffer.nativeHeld());
    CHECK(buffer.hasPendingPress());

    const auto fire = buffer.onPhysicsStep(203, true);
    CHECK(fire.push);
    CHECK(fire.release);
    CHECK(fire.bufferedFire);
    CHECK(!fire.expired);
    CHECK(!buffer.hasPendingPress());
    CHECK(!buffer.physicalHeld());
    CHECK(!buffer.nativeHeld());

    // No duplicated native release or replay on future steps.
    checkNoAction(buffer.onPhysicsStep(204, true));
    checkNoAction(buffer.onPhysicalRelease());
}

void testBypassReconcilesHeldStateAndDropsPending() {
    pib::EarlyPressBuffer buffer;
    CHECK(buffer.setWindowTicks(8).accepted);

    checkNoAction(buffer.onPhysicalPress(300, false));
    CHECK(buffer.physicalHeld());
    CHECK(!buffer.nativeHeld());
    CHECK(buffer.hasPendingPress());

    // Switching to a raw/bypassed mode forwards the currently held physical
    // state once and throws away the stale early press.
    const auto bypass = buffer.setBypassed(true);
    CHECK(bypass.push);
    CHECK(!bypass.release);
    CHECK(!bypass.bufferedFire);
    CHECK(!bypass.expired);
    CHECK(buffer.isBypassed());
    CHECK(buffer.physicalHeld());
    CHECK(buffer.nativeHeld());
    CHECK(!buffer.hasPendingPress());

    const auto release = buffer.onPhysicalRelease();
    CHECK(!release.push);
    CHECK(release.release);
    CHECK(!buffer.physicalHeld());
    CHECK(!buffer.nativeHeld());

    // Stage 5/off uses the same bypass reconciliation path.
    const auto rawDown = buffer.onPhysicalPress(301, false);
    CHECK(rawDown.push);
    CHECK(!rawDown.release);
    CHECK(buffer.physicalHeld());
    CHECK(buffer.nativeHeld());
    const auto off = buffer.setWindowTicks(0);
    CHECK(off.accepted);
    checkNoAction(off.actions);
    CHECK(buffer.isBypassed());
    CHECK(!buffer.hasPendingPress());
}

void testLifecycleCleanupActual() {
    pib::EarlyPressBuffer buffer;
    const auto down = buffer.onPhysicalPress(400, true);
    CHECK(down.push);
    CHECK(buffer.nativeHeld());

    const auto clear = buffer.clearAll();
    CHECK(!clear.push);
    CHECK(clear.release);
    CHECK(!clear.bufferedFire);
    CHECK(!clear.expired);
    CHECK(!buffer.physicalHeld());
    CHECK(!buffer.nativeHeld());
    CHECK(!buffer.hasPendingPress());

    // A second lifecycle cleanup cannot produce a second release.
    checkNoAction(buffer.clearAll());
}

void testRejectedConfigurationDoesNotChangeState() {
    pib::EarlyPressBuffer buffer;
    const auto before = buffer.windowTicks();
    const auto result = buffer.setWindowTicks(7);
    CHECK(!result.accepted);
    checkNoAction(result.actions);
    CHECK(buffer.windowTicks() == before);
    CHECK(!buffer.isBypassed());
}

} // namespace

int main() {
    testStageWindows();
    testImmediateNativePressAndRelease();
    testInclusiveBoundAndOneShotFire();
    testExpiryForEverySupportedWindow();
    testEarlyTapReplaysWithoutStuckHold();
    testBypassReconcilesHeldStateAndDropsPending();
    testLifecycleCleanupActual();
    testRejectedConfigurationDoesNotChangeState();

    std::printf("buffer_core_test: PASS (%d checks)\n", g_checks);
    return 0;
}
