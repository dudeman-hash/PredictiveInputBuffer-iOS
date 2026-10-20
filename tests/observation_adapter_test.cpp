#include "buffer_core.hpp"
#include <cassert>
#include <iostream>
using predictive_input_buffer::EarlyPressBuffer;
struct MockAdapter {
    EarlyPressBuffer buffer;
    unsigned rawDowns = 0, rawUps = 0, replayDowns = 0, replayUps = 0, nativeJumps = 0;
    bool nativeHeld = false;
    void press(unsigned tick, bool grounded) {
        (void)buffer.onPhysicalPress(tick, grounded);
        ++rawDowns; nativeHeld = true; // The genuine native call always happens.
    }
    void release() { (void)buffer.onPhysicalRelease(); ++rawUps; nativeHeld = false; }
    void step(unsigned tick, bool grounded) {
        auto action = buffer.onPhysicsStep(tick, grounded);
        bool replay = action.bufferedFire && !buffer.physicalHeld();
        if (replay) { ++replayDowns; nativeHeld = true; }
        // Model a jump consumer between replay push and replay release.
        if (grounded && nativeHeld) { ++nativeJumps; buffer.cancelPendingPress(); }
        if (replay) { ++replayUps; nativeHeld = false; }
    }
};
int main() {
    MockAdapter tap;
    tap.press(10, false); tap.release(); tap.step(11, false); tap.step(12, true);
    assert(tap.rawDowns == 1 && tap.rawUps == 1);
    assert(tap.replayDowns == 1 && tap.replayUps == 1 && tap.nativeJumps == 1);
    assert(!tap.nativeHeld);
    tap.step(13, true); assert(tap.nativeJumps == 1);
    MockAdapter held;
    held.press(10, false); held.step(12, true);
    assert(held.replayDowns == 0 && held.nativeJumps == 1 && held.nativeHeld);
    held.release(); assert(!held.nativeHeld);
    MockAdapter consumed;
    consumed.press(10, false); consumed.buffer.cancelPendingPress(); consumed.release();
    consumed.step(12, true); assert(consumed.replayDowns == 0);
    MockAdapter late;
    late.press(10, false); late.release(); late.step(13, true);
    assert(late.replayDowns == 0 && !late.buffer.hasPendingPress());
    MockAdapter off;
    (void)off.buffer.setWindowTicks(0); off.press(10, false); off.release(); off.step(12, true);
    assert(off.replayDowns == 0);
    std::cout << "Observation adapter: 5 scenarios passed (mock consumer, not GD proof)\n";
}
