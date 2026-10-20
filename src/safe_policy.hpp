#pragma once

namespace pib {
// A save decision uses the setting at call time, never a cached attempt-start value.
constexpr bool blockKnownSave(bool safeMode, bool assisted, bool practice,
                              bool editorTesting, bool sameLevel) noexcept {
    return safeMode && assisted && !practice && !editorTesting && sameLevel;
}

// The native save-path audit is incomplete. Fail closed for NEW interventions in
// normal mode with Safe Mode ON. Practice/editor and explicit Safe Mode OFF can
// exercise the early-input implementation without implying account protection.
constexpr bool allowIntervention(bool enabled, int ticks, bool safeMode,
                                 bool practice, bool editorTesting) noexcept {
    return enabled && ticks > 0 && (!safeMode || practice || editorTesting);
}
} // namespace pib
