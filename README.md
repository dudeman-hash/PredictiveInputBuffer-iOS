# Predictive Input Buffer — iOS development source

Target: Geometry Dash iOS 2.2081, Geode 5.11.2, arm64. Source version: 0.3.0.

This is an unfinished development build, not an installable release or a gameplay-validated equivalent of the supplied Windows mod. A conservative live adapter now reads the current player physics and nearby static level objects for **classic cube taps**. It simulates the immediate click and later timings; if the immediate trajectory intersects a modeled hazard but a later candidate survives, it suppresses that click and injects it on the selected update tick. New interventions remain Practice/editor-only while Safe Mode is ON.

The live model is deliberately narrow. It uses native `PlayerObject` position/velocity, current `dt`, gravity and jump-start values, basic axis-aligned static solids/hazards, and gravity portals. It fails closed (passes the input through without prediction) around orb/pad interactions, mode/size/speed/teleport portals, slopes, rotated or grouped/dynamic geometry, unsupported player states, and non-cube modes. The collision model is not a clone of Geometry Dash's native collision system: hazard rectangles can be conservative, the solid response covers only simple axis-aligned contacts, and the fallback floor is inferred only from a grounded live player. Wave prediction, comprehensive orb/pad behavior, and other movement modes are not implemented.

**Do not use this experimental intervention for a progress attempt.** Safe Mode's save protection is incomplete. The prediction is not yet calibrated on-device, and even its supported subset may be wrong on real gameplay. Test only in Practice mode, with the debug overlay visible, and report the prediction status, candidate count, nearby object count, and whether an override fired.

Run the portable tests with:

```sh
bash tests/run_tests.sh
```

These tests cover deterministic model and input scheduling logic; they do not compile or validate the game. A successful CI build is not in-game proof. See [PROGRESS.md](PROGRESS.md) for the current implementation, test status, and remaining work.
