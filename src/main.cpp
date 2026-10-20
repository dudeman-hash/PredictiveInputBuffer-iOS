#include <Geode/Geode.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/cocos/CCDirector.h>
#include <Geode/cocos/label_nodes/CCLabelBMFont.h>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>

using namespace geode::prelude;

namespace {
    constexpr int kOverlayTag = 0x504942;

    struct Stage {
        int allTicks;
        int waveTicks;
        bool enabled;
    };

    constexpr Stage stages[] = {
        {2, 2, true},
        {4, 4, true},
        {8, 8, true},
        {16, 8, true},
        {0, 0, false},
    };

    enum class InputMode : std::size_t {
        CubeOrOther,
        Ship,
        Ball,
        Ufo,
        Wave,
        Robot,
        Spider,
        Swing,
        Count,
    };

    constexpr char const* modeNames[] = {
        "cube/other", "ship", "ball", "ufo", "wave", "robot", "spider", "swing"
    };

    struct ReturnCounts {
        std::uint64_t yes = 0;
        std::uint64_t no = 0;
    };

    struct HookTelemetry {
        bool pushButton = false;
        bool releaseButton = false;
        bool perStep = false;
        bool destroyPlayer = false;
        bool resetLevel = false;
        std::uint64_t stepCalls = 0;
        std::uint64_t pushCalls = 0;
        std::uint64_t releaseCalls = 0;
        int lastStep = -1;
        float lastStepDt = 0.f;
        bool lastHalfTick = false;
        bool lastTick = false;
        std::array<ReturnCounts, static_cast<std::size_t>(InputMode::Count)> pushReturns{};
        std::array<ReturnCounts, static_cast<std::size_t>(InputMode::Count)> releaseReturns{};
        char lastPush[96] = "none";
        char lastRelease[96] = "none";
    };

    std::int64_t currentStage = 1;
    bool modEnabled = true;
    bool safeMode = true;
    bool showOverlay = true;
    bool modLoaded = false;
    HookTelemetry telemetry;
    std::uint64_t attemptOverrides = 0;
    std::uint64_t bufferedPressesFired = 0;
    std::uint64_t nearbyObjectsTested = 0;
    bool attemptAssisted = false;
    char predictionResult[96] = "NOT RUN: exact wave physics unavailable";
    float overlayAccumulator = 0.f;

    int stageAllTicks() {
        if (currentStage < 1 || currentStage > 5) return 0;
        auto const& stage = stages[currentStage - 1];
        return stage.enabled && modEnabled ? stage.allTicks : 0;
    }

    int stageWaveTicks() {
        if (currentStage < 1 || currentStage > 5) return 0;
        auto const& stage = stages[currentStage - 1];
        return stage.enabled && modEnabled ? stage.waveTicks : 0;
    }

    InputMode modeFor(PlayerObject* player) {
        if (!player) return InputMode::CubeOrOther;
        if (player->m_isDart) return InputMode::Wave;
        if (player->m_isShip) return InputMode::Ship;
        if (player->m_isBall) return InputMode::Ball;
        if (player->m_isBird) return InputMode::Ufo;
        if (player->m_isRobot) return InputMode::Robot;
        if (player->m_isSpider) return InputMode::Spider;
        if (player->m_isSwing) return InputMode::Swing;
        // 2.2081 exposes no m_isCube field. Keep this fallback explicit rather
        // than asserting every remaining mode is cube.
        return InputMode::CubeOrOther;
    }

    char const* modeName(InputMode mode) {
        return modeNames[static_cast<std::size_t>(mode)];
    }

    void recordReturn(std::array<ReturnCounts, static_cast<std::size_t>(InputMode::Count)>& counts,
                      InputMode mode, bool result) {
        auto& entry = counts[static_cast<std::size_t>(mode)];
        result ? ++entry.yes : ++entry.no;
    }

    void logStage() {
        log::info(
            "[InputBuffer] loaded={} enabled={} stage={} allTicks={} waveTicks={} safeMode={} overlay={}",
            modLoaded ? "YES" : "NO",
            modEnabled,
            currentStage,
            stageAllTicks(),
            stageWaveTicks(),
            safeMode,
            showOverlay
        );
    }

    void applyStage(std::int64_t value) {
        currentStage = std::clamp<std::int64_t>(value, 1, 5);
        logStage();
    }

    void resetAttemptTelemetry() {
        attemptOverrides = 0;
        bufferedPressesFired = 0;
        nearbyObjectsTested = 0;
        attemptAssisted = false;
        std::snprintf(predictionResult, sizeof(predictionResult), "%s", "NOT RUN: exact wave physics unavailable");
    }

    void updateOverlay(PlayLayer* layer, float dt) {
        if (!layer) return;
        if (!showOverlay) {
            if (layer->getChildByTag(kOverlayTag)) {
                layer->removeChildByTag(kOverlayTag, true);
                log::info("[InputBuffer] debug overlay hidden by setting");
            }
            return;
        }

        overlayAccumulator += dt;
        if (overlayAccumulator < 1.0f && layer->getChildByTag(kOverlayTag)) return;
        overlayAccumulator = 0.f;

        auto* label = static_cast<cocos2d::CCLabelBMFont*>(layer->getChildByTag(kOverlayTag));
        if (!label) {
            label = cocos2d::CCLabelBMFont::create("Predictive Input Buffer", "bigFont.fnt");
            if (!label) {
                log::error("[InputBuffer] Failed to create CCLabelBMFont using bigFont.fnt");
                return;
            }
            label->setAnchorPoint({0.f, 1.f});
            label->setScale(0.34f);
            label->setColor({255, 255, 255});
            layer->addChild(label, 10000, kOverlayTag);
            log::info("[InputBuffer] on-screen diagnostic overlay created");
        }

        auto const winSize = cocos2d::CCDirector::sharedDirector()->getWinSize();
        label->setPosition({8.f, winSize.height - 12.f});

        auto* p1 = layer->m_player1;
        auto* p2 = layer->m_player2;
        auto const p1Wave = p1 && p1->m_isDart;
        auto const p2Wave = p2 && p2->m_isDart;
        int const allTicks = stageAllTicks();
        int const waveTicks = stageWaveTicks();
        auto const safeState = safeMode ? "ON (enforcement not implemented)" : "OFF";
        auto const assistedState = attemptAssisted ? "YES" : "NO";
        char text[1200];
        std::snprintf(
            text,
            sizeof(text),
            "PREDICTIVE INPUT BUFFER - DIAGNOSTICS\n"
            "Loaded: %s | Enabled: %s | Stage: %" PRId64 " | lookahead all/wave: %d/%d\n"
            "Wave mode: P1 %s, P2 %s\n"
            "Nearby objects tested: %" PRIu64 " (predictor not running)\n"
            "Last prediction: %s\n"
            "Input overrides: %" PRIu64 " | buffered presses fired: %" PRIu64 "\n"
            "pushButton: calls %" PRIu64 " | last %s\n"
            "push returns by mode (true/false): cube/other %" PRIu64 "/%" PRIu64 " ship %" PRIu64 "/%" PRIu64 " ball %" PRIu64 "/%" PRIu64 "\n"
            "ufo %" PRIu64 "/%" PRIu64 " wave %" PRIu64 "/%" PRIu64 " robot %" PRIu64 "/%" PRIu64 " spider %" PRIu64 "/%" PRIu64 " swing %" PRIu64 "/%" PRIu64 "\n"
            "releaseButton: calls %" PRIu64 " | last %s\n"
            "Hooks fired: push %s release %s per-step %s destroyPlayer %s resetLevel %s\n"
            "Per-step calls: %" PRIu64 " | m_currentStep %d | dt %.6g | halfTick %s lastTick %s\n"
            "Safe Mode: %s | ASSIST ACTIVE: %s (no input overrides in diagnostic branch)",
            modLoaded ? "YES" : "NO",
            modEnabled ? "YES" : "NO",
            currentStage,
            allTicks,
            waveTicks,
            p1Wave ? "YES" : "NO",
            p2Wave ? "YES" : "NO",
            nearbyObjectsTested,
            predictionResult,
            attemptOverrides,
            bufferedPressesFired,
            telemetry.pushCalls,
            telemetry.lastPush,
            telemetry.pushReturns[0].yes, telemetry.pushReturns[0].no,
            telemetry.pushReturns[1].yes, telemetry.pushReturns[1].no,
            telemetry.pushReturns[2].yes, telemetry.pushReturns[2].no,
            telemetry.pushReturns[3].yes, telemetry.pushReturns[3].no,
            telemetry.pushReturns[4].yes, telemetry.pushReturns[4].no,
            telemetry.pushReturns[5].yes, telemetry.pushReturns[5].no,
            telemetry.pushReturns[6].yes, telemetry.pushReturns[6].no,
            telemetry.pushReturns[7].yes, telemetry.pushReturns[7].no,
            telemetry.releaseCalls,
            telemetry.lastRelease,
            telemetry.pushButton ? "YES" : "NO",
            telemetry.releaseButton ? "YES" : "NO",
            telemetry.perStep ? "YES" : "NO",
            telemetry.destroyPlayer ? "YES" : "NO",
            telemetry.resetLevel ? "YES" : "NO",
            telemetry.stepCalls,
            telemetry.lastStep,
            telemetry.lastStepDt,
            telemetry.lastHalfTick ? "YES" : "NO",
            telemetry.lastTick ? "YES" : "NO",
            safeState,
            assistedState
        );
        label->setString(text);
        label->limitLabelWidth(std::max(180.f, winSize.width - 16.f), 0.34f, 0.20f);
        log::info("[InputBuffer] overlay state:\n{}", text);
    }
}

class $modify(PredictiveInputBufferPlayer, PlayerObject) {
    bool pushButton(PlayerButton button) {
        telemetry.pushButton = true;
        ++telemetry.pushCalls;
        auto const mode = modeFor(this);
        auto* layer = PlayLayer::get();
        bool const isPlayer1 = layer && layer->m_player1 == this;
        bool const isPlayer2 = layer && layer->m_player2 == this;
        bool const result = PlayerObject::pushButton(button);
        recordReturn(telemetry.pushReturns, mode, result);
        std::snprintf(
            telemetry.lastPush,
            sizeof(telemetry.lastPush),
            "%s %s p%d button=%d",
            modeName(mode), result ? "true" : "false",
            isPlayer1 ? 1 : (isPlayer2 ? 2 : 0), static_cast<int>(button)
        );
        log::info("[InputBuffer] pushButton mode={} player={} button={} native-return={}",
                  modeName(mode), isPlayer1 ? 1 : (isPlayer2 ? 2 : 0),
                  static_cast<int>(button), result);
        return result;
    }

    bool releaseButton(PlayerButton button) {
        telemetry.releaseButton = true;
        ++telemetry.releaseCalls;
        auto const mode = modeFor(this);
        auto* layer = PlayLayer::get();
        bool const isPlayer1 = layer && layer->m_player1 == this;
        bool const isPlayer2 = layer && layer->m_player2 == this;
        bool const result = PlayerObject::releaseButton(button);
        recordReturn(telemetry.releaseReturns, mode, result);
        std::snprintf(
            telemetry.lastRelease,
            sizeof(telemetry.lastRelease),
            "%s %s p%d button=%d",
            modeName(mode), result ? "true" : "false",
            isPlayer1 ? 1 : (isPlayer2 ? 2 : 0), static_cast<int>(button)
        );
        log::info("[InputBuffer] releaseButton mode={} player={} button={} native-return={}",
                  modeName(mode), isPlayer1 ? 1 : (isPlayer2 ? 2 : 0),
                  static_cast<int>(button), result);
        return result;
    }
};

class $modify(PredictiveInputBufferGameLayer, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (auto* layer = PlayLayer::get(); layer && static_cast<GJBaseGameLayer*>(layer) == this) {
            telemetry.perStep = true;
            ++telemetry.stepCalls;
            telemetry.lastStep = this->m_currentStep;
            telemetry.lastStepDt = dt;
            telemetry.lastHalfTick = isHalfTick;
            telemetry.lastTick = isLastTick;
            if (telemetry.stepCalls == 1 || telemetry.stepCalls % 120 == 0) {
                log::info("[InputBuffer] processCommands call={} m_currentStep={} dt={} halfTick={} lastTick={}",
                          telemetry.stepCalls, telemetry.lastStep, dt, isHalfTick, isLastTick);
            }
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

class $modify(PredictiveInputBufferPlayLayer, PlayLayer) {
    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        updateOverlay(this, dt);
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        telemetry.destroyPlayer = true;
        log::info("[InputBuffer] PlayLayer::destroyPlayer hook fired");
        PlayLayer::destroyPlayer(player, object);
    }

    void onEnterTransitionDidFinish() {
        resetAttemptTelemetry();
        PlayLayer::onEnterTransitionDidFinish();
    }

    void resetLevel() {
        telemetry.resetLevel = true;
        log::info("[InputBuffer] PlayLayer::resetLevel hook fired");
        resetAttemptTelemetry();
        PlayLayer::resetLevel();
    }

    void onQuit() {
        resetAttemptTelemetry();
        PlayLayer::onQuit();
    }

    void onExit() {
        resetAttemptTelemetry();
        PlayLayer::onExit();
    }
};

$on_mod(Loaded) {
    modLoaded = true;
    applyStage(Mod::get()->getSettingValue<std::int64_t>("buffer-stage"));
    modEnabled = Mod::get()->getSettingValue<bool>("enabled");
    safeMode = Mod::get()->getSettingValue<bool>("safe-mode");
    showOverlay = Mod::get()->getSettingValue<bool>("show-debug-overlay");

    listenForSettingChanges<std::int64_t>("buffer-stage", [](std::int64_t value) {
        applyStage(value);
    });
    listenForSettingChanges<bool>("enabled", [](bool value) {
        modEnabled = value;
        logStage();
    });
    listenForSettingChanges<bool>("safe-mode", [](bool value) {
        safeMode = value;
        logStage();
    });
    listenForSettingChanges<bool>("show-debug-overlay", [](bool value) {
        showOverlay = value;
        logStage();
    });

    log::info("[InputBuffer] Loaded. Diagnostic-only revision: prediction, buffering, and Safe Mode enforcement are not active.");
    logStage();
}
