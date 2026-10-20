# iOS hook diagnostic branch — 0.3.0

This is a **diagnostic-only revision** for Geometry Dash iOS 2.2081 / Geode 5.11.2. It deliberately does not change gameplay. Input buffering, wave prediction, and Safe Mode save-blocking are not active. Do not treat the Safe Mode setting as progress protection in this revision, and do not use this revision for a record attempt.

## What it checks

The on-screen `CCLabelBMFont` overlay should appear in PlayLayer and show `Loaded: YES`, stage/lookahead values, wave state for both players, the live native `pushButton` return values grouped by observed mode, the last `releaseButton` return, and whether `processCommands` and PlayLayer lifecycle hooks have fired. The same overlay snapshot is written to the Geode log once per second. Input-return events are also logged individually.

`processCommands` diagnostics include invocation count, `m_currentStep`, the received `dt`, `isHalfTick`, and `isLastTick`. Those observations are needed before treating it as a once-per-physics-step callback; the static binding only proves its signature and iOS address, not its runtime cadence.

The overlay intentionally reports `Nearby objects tested: 0`, `Last prediction: NOT RUN`, `Input overrides: 0`, and `buffered presses fired: 0`. `Safe Mode` is shown as `enforcement not implemented` because this branch does not block any saves.

## Suggested proof to return

1. Open a level and capture a screenshot showing whether the overlay appears and its `Loaded` and hook states.
2. In a safe, non-record test level, enter cube, ship, ball, UFO, wave, robot, spider, and swing modes where available. Tap/release at least once in each mode, then capture the overlay after each. `cube/other` is an intentionally unresolved group because the binding has no `m_isCube` field.
3. In wave mode, capture the overlay while the level is running. The per-step call count and `m_currentStep` should reveal whether the hook fires and how the observed fields change. No claim is made in advance that each invocation equals one physics tick.
4. Toggle `Show Debug Overlay`, `Enabled`, `Safe Mode`, and the stage slider in mod settings. Confirm the overlay visibility and displayed values update without restarting.
5. Do not evaluate predicted deaths, early-press buffering, progress saves, or Safe Mode on this branch; those behaviors are intentionally absent.

Please return the screenshots, the values shown for native press returns in the tested modes, and whether `m_currentStep` changes in step with `processCommands` calls. Do not send account identifiers or other personal data from Geode logs.
