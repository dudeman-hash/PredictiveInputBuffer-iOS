# Predictive Input Buffer — development source

Target: Geometry Dash iOS 2.2081, Geode 5.11.2, arm64. Source version: 0.3.0.

This is not an installable mod file or a finished wave predictor. See `PROGRESS.md` for implemented features, limitations, tested behavior, and the precise incomplete Safe Mode coverage. No package was built or sent in this continuation.

The source adds a conservative classic cube/robot released-tap ground buffer and keeps genuine native input return values. The wave decision planner is standalone and is not connected to native movement/collision. New normal-mode assists are gated while Safe Mode is ON because complete save protection is not proven. Test in Practice mode; do not use this development revision for a record attempt.

Run standalone tests with:

```sh
bash tests/run_tests.sh
```

These are C++ core/mock tests, not an iOS SDK compile or in-game proof.

To apply this work, replace the corresponding files in your existing repository with this source bundle. Preserve any unrelated repository files and the existing SDK-selection mechanism. Do not download the source ZIP and import it as a `.geode` file. The included iOS workflow can be manually run on a source branch by you, as in the prior task; it also builds on pushes to `main`. This session did not publish a branch or run that workflow.
