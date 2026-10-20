# Predictive Input Buffer — source progress, 20 October 2026

This is an unfinished **source-only development revision**, not a working wave-assist release. The ground-tap adapter has been written, but it has not been compiled with the iOS SDK or run on your iPad. No new `.geode` file was built or sent. The private GitHub repository was not changed because GitHub access was not enabled for this session.

## What changed

The early-input logic no longer waits for `PlayerObject::pushButton` to return `false`. Every raw press/release still calls the native function and returns its genuine result. The adapter independently remembers an airborne jump press, with one bounded slot for each player. A short tap released before landing can be replayed at a conservative grounded state within the selected window. A still-held raw press is not replayed a second time.

The source prototype covers classic cube/fallback and robot ground taps. It excludes platformer, ball, ship, UFO, spider, swing, wave, and orb/pad-specific replay. Native jump-counter calls and possible ring activation cancel remembered input conservatively, so an input the game may already have used is not deliberately replayed later. These gates are conservative prototypes, not a verified contract for every game action.

The replay press stays held through the native `updateJump` call and is released afterward. It is not immediately pushed and released before the game has a chance to consume it. Replays use a guard to avoid being mistaken for physical input, clear their pending slot before injecting, and mark the attempt assisted before the injection. Cleanup drops pending taps and releases only a temporary hold owned by the adapter; it does not manufacture releases for raw user holds.

Buffer work now runs from the native player `update` / `updateJump` path, not the frame-level game-layer `update`. Each player has a mod-owned update-call counter. The overlay also shows `processCommands`, `m_currentStep`, each player's update count, `updateJump` count, and the native dt values. This makes cadence measurable without assuming a frequency or converting between the different native dt units. **The one-update-per-physics-step relationship still needs on-device confirmation.**

The stage table remains 2/2, 4/4, 8/8, 16/8, and off. Both metadata and CMake use version 0.3.0. All four settings are present and read live: `buffer-stage`, `enabled`, `safe-mode` defaulting to true, and `show-debug-overlay`. Stage 5 and the master switch disable new replay; raw inputs remain forwarded. An already-assisted attempt remains assisted when the stage is changed, until the next attempt or exit.

## Wave prediction is not connected

`src/wave_decision.hpp` implements and tests the bounded latest-saving-switch planner. It leaves a safe current input alone, leaves both constant inputs alone when both die, and otherwise picks the latest switch whose caller-provided simulation survives. It has no guessed movement constants and performs no allocation of its own.

It is **not** a wave simulator. No hazard, solid, slope, floor, ceiling, portal, gravity, size, speed, moving-object, or rotating-object lookahead is enabled. Nearby-object and wave-override counts therefore remain zero, and the overlay says prediction is not implemented.

The recovered movement and collision decompilations are labeled 2.2, not verified 2.2081. Slopes depend on collision history, orientation, gravity, and slide state, so replacing them with “all slopes kill” would be wrong. The exact 2.2081 bindings supply callable APIs, but do not document enough section-index or collision-state semantics to construct an exact, isolated predictor.

I also inspected QOLMod's native shadow-player approach. Its trajectory code calls native player update and collision checks, but it also requires substantial hooks to suppress real world/trigger side effects, copies dynamic state containers, and allocates a collision-filter vector. That is not an established side-effect-free, allocation-free simulation boundary for this request. I did not copy it into your mod or run prediction on the live player. [1] [2]

To finish the exact predictor, the missing evidence is verified 2.2081 movement/collision/section behavior or a verified isolated native simulation boundary, including every effectful path. Another screenshot proving `pushButton` fires is not sufficient; your existing screenshots already establish that.

## Safe Mode is incomplete and guarded

New interventions in normal play are disabled while Safe Mode is ON. Practice/editor testing can exercise the ground-buffer prototype without save blocking. Safe Mode OFF allows the prototype's new interventions in normal play and does not block saving. This temporary gating is a deliberate limitation, **not** the complete Safe Mode behavior you requested. Use Practice mode for tests rather than a real progress attempt.

The following known calls have source hooks, conditional on an assisted attempt, Safe Mode ON at call time, the current level, and neither practice nor real editor/test mode:

- `GJGameLevel::savePercentage`: skips the native normal-percentage call.
- `GJGameLevel::saveNewScore` and `storeNewLocalScore`: skip those local-score calls.
- `PlayLayer::levelComplete`: temporarily adds the native test-mode flag and restores the previous flag afterward. It never forces an existing test-mode flag off. This approach has an existing open-source reference, but is not proof of full protection. [2]
- `GameStatsManager::completedLevel`, `completedStarLevel`, and `completedDemonLevel`: skip those completion-record calls.
- `GameStatsManager::awardCurrencyForLevel` and `awardDiamondsForLevel`: skip those award calls.

These are direct guards, not snapshot/restore. The assisted flag is preserved through death and nested reset/quit/exit calls until native attempt-end work returns. The overlay counts known save calls blocked, but does not claim all progress is blocked.

**Not covered or proven:** all coin persistence, direct stat-counter mutations, achievements, daily/weekly/event/list/map-pack rewards, every server/leaderboard submission, asynchronous callbacks after leaving the level, and rollback of progress awarded before assistance or before a live Safe Mode toggle. There is no proven whole-result transaction or complete snapshot restoration. Toggling Safe Mode ON after an assisted Safe Mode OFF attempt only enables the known guards; it does not retroactively establish full protection. The all-or-none requirement is therefore still unmet.

## Which original suspicions were real

**1a:** The old source gated its queue on a false native return. Your cube screenshot recorded true results and no false results; the wave screenshot confirmed the wave input hooks also fire. That invalidates using this return as the sole signal to enqueue in the observed cases. It does not prove the return semantics in every other mode.

**1b:** The old queue was driven from frame-level `GJBaseGameLayer::update` and checked `m_currentStep` there. The recovered 2.2 update source has multiple player updates/collision passes inside a frame. Your separate command-count/current-step screenshots do not establish an exact callback-to-step ratio. The revised source uses player-update calls and exposes matching counts rather than treating `m_currentStep` as the scheduler.

**1c and 1d:** The original source contained no hazard/solid prediction and no debug overlay. The diagnostic branch supplied the overlay, and your screenshots proved it appeared. This revision retains diagnostics and exposes actual queued/fired/expired tap state, but exact wave prediction is still unfinished.

**1e:** Your screenshots established that the tested input and diagnostic hooks loaded and fired. A local audit of the supplied 2.2081 binding file found iOS addresses for all 22 native methods hooked by this revision. That is static binding evidence only; it is not proof that every new hook applies at runtime or escapes inlining.

The main fields used are `m_isDart`, the other vehicle flags, `m_isOnGround`, `m_isOnGround2`, `m_isDead`, `m_controlsDisabled`, `m_inputsLocked`, `m_isLocked`, `m_touchingRings`, `m_isPlatformer`, `m_player1`, `m_player2`, `m_level`, `m_isEditor`, `m_isPracticeMode`, `m_isTestMode`, and `m_currentStep`. Exact checked declarations are included in `HOOK-DECLARATIONS.txt`.

## Tests actually run

Run `bash tests/run_tests.sh` from the source bundle to reproduce the current standalone tests. They compile only the game-independent tests, not the iOS mod.

The buffer suite passed 212 checks for stage windows, inclusive expiry boundaries, once-only firing, released taps, held-state separation, bypass, and cleanup. The observation-adapter mock passed five scenarios: replayed tap, raw held press without duplicate replay, cancellation after native consumption, late expiry, and stage-off behavior. The wave planner passed four cases: current-safe unchanged, both-die unchanged, latest surviving switch, and immediate switch. The Safe Mode policy passed 13 checks. Source metadata checks passed.

No iOS SDK compile or package inspection was performed in this continuation. No new code was tested in Geometry Dash. I did not measure FPS or allocation behavior of the native game methods. I did not run the tight-wave, zig-zag, mini-wave, spike, ceiling, slope, dual-player, per-stage gameplay tests, or any of the six on-device Safe Mode tests. The only real-iPad evidence available is your earlier diagnostic screenshots.

The next meaningful gameplay check is a Practice-mode cube/robot tap just before landing, comparing queued/fired counters with stages 1–4 and stage 5, and comparing player-update versus command counts over the same interval. This requires your own build of the source; do not treat the source ZIP as an installable `.geode`. The preserved workflow has manual dispatch and main-only push builds, but no workflow was run here. The finished-release proof condition is still not met.

## References

[1]: https://geode-sdk.org/mods/thesillydoggo.qolmod "QOLMod listing and 2.2081 compatibility history"
[2]: https://github.com/TheSillyDoggo/GeodeMenu/tree/2c21863548f38484bd7c968b95ffe71f5cd4ceaa/src "QOLMod source inspected for native trajectory side effects and Safe Mode guards"
