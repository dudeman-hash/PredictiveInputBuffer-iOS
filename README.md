# Predictive Input Buffer — iOS development source

Target: Geometry Dash iOS 2.2081, Geode 5.11.2, arm64. Source version: 0.3.0.

This is not yet an installable release or a gameplay-validated equivalent of the supplied Windows mod. The mod still implements only the conservative classic cube/robot released-tap buffer. A new allocation-free trajectory kernel now models configurable per-step movement, swept hazard checks, conservative solid collision, and orb, pad, gravity-portal, and speed-portal events. Its click planner tests the immediate click first and, only if it dies, selects the latest surviving delayed candidate within the configured window.

The kernel is compiled with the mod and covered by deterministic host tests. It is **not connected to live Geometry Dash state**. The current iOS adapter does not yet extract authoritative 2.2081 player physics, object hitboxes, trigger order, portal parameters, or collision response. The kernel’s physics values and world-space object snapshots must be calibrated against the exact iOS build before its results can safely drive real inputs. Do not treat passing unit tests or a successful compile as in-game proof. Keep testing in Practice mode.

Run standalone tests with:

```sh
bash tests/run_tests.sh
```

These tests compile portable C++ prediction logic, not the game itself. Wave prediction, exact native simulation, complete Safe Mode protection, and gameplay integration remain unfinished. See [PROGRESS.md](PROGRESS.md) for implementation details and limitations.
