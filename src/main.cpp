#include <Geode/Geode.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/cocos/CCDirector.h>
#include <Geode/cocos/label_nodes/CCLabelBMFont.h>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GJGameLevel.hpp>
#include <Geode/modify/GameStatsManager.hpp>
#include "buffer_core.hpp"
#include "safe_policy.hpp"
#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
using namespace geode::prelude;
namespace core = predictive_input_buffer;

namespace {
constexpr int overlayTag = 0x504942;
constexpr int allWindows[] = {2, 4, 8, 16, 0}, waveWindows[] = {2, 4, 8, 8, 0};
std::int64_t stage = 1;
bool enabled = true, safeMode = true, showDebug = true, loaded = false;
bool replaying = false, assisted = false, destroyFired = false, resetFired = false;
bool completionGuard = false;
unsigned boundaryDepth = 0;
std::uint64_t fired = 0, queued = 0, expired = 0, cancelled = 0, blocked = 0;
std::uint64_t pushes = 0, releases = 0, commands = 0;
std::uint64_t updates[2] = {}, jumps[2] = {};
float playerDt[2] = {}, commandDt = 0.f, overlayElapsed = 1.f, logElapsed = 0.f;
int nativeStep = -1;
struct Returns { std::uint64_t yes = 0, no = 0; };
std::array<Returns, 8> returns{};
int modeFor(PlayerObject* p) {
    if (p->m_isDart) return 4;
    if (p->m_isShip) return 1;
    if (p->m_isBall) return 2;
    if (p->m_isBird) return 3;
    if (p->m_isRobot) return 5;
    if (p->m_isSpider) return 6;
    if (p->m_isSwing) return 7;
    return 0; // 2.2081 has no m_isCube field.
}
int playerIndex(PlayerObject* p) {
    auto* l = PlayLayer::get();
    if (!l) return -1;
    return p == l->m_player1 ? 0 : (p == l->m_player2 ? 1 : -1);
}
int allTicks() { return enabled ? allWindows[stage - 1] : 0; }
int waveTicks() { return enabled ? waveWindows[stage - 1] : 0; }
bool testing(PlayLayer* l) { return l && (l->m_isEditor || (l->m_isTestMode && !completionGuard)); }
bool blockSave(PlayLayer* l, GJGameLevel* level = nullptr) {
    return l && pib::blockKnownSave(safeMode, assisted, l->m_isPracticeMode,
                                    testing(l), !level || level == l->m_level);
}
bool canAssist(PlayerObject* p) {
    auto* l = PlayLayer::get();
    int const mode = modeFor(p);
    return l && playerIndex(p) >= 0 && (mode == 0 || mode == 5) && !p->m_isPlatformer
        && pib::allowIntervention(enabled, allTicks(), safeMode, l->m_isPracticeMode, testing(l));
}
bool ready(PlayerObject* p) {
    return !p->m_isDead && !p->m_controlsDisabled && !p->m_inputsLocked && !p->m_isLocked
        && (p->m_isOnGround || p->m_isOnGround2);
}
struct ReplayScope {
    bool before = replaying;
    ReplayScope() { replaying = true; }
    ~ReplayScope() { replaying = before; }
};
void resetAttempt() {
    assisted = false;
    fired = queued = expired = cancelled = blocked = 0;
    overlayElapsed = 1.f;
}
}

class $modify(BufferPlayer, PlayerObject) {
public:
    struct Fields {
        core::EarlyPressBuffer buffer;
        core::PhysicsStep tick = 0, processedTick = static_cast<core::PhysicsStep>(-1);
        bool syntheticHold = false, insideUpdate = false;
    };
    void cancelPending() {
        if (m_fields->buffer.hasPendingPress()) ++cancelled;
        m_fields->buffer.cancelPendingPress();
    }
    void clearBuffer() {
        // Raw holds are never released by this helper. Only our temporary replay.
        if (m_fields->syntheticHold) {
            ReplayScope scope;
            this->releaseButton(PlayerButton::Jump);
            m_fields->syntheticHold = false;
        }
        (void)m_fields->buffer.clearAll();
    }
    void syncConfiguration() {
        int const window = canAssist(this) ? allTicks() : 0;
        if (m_fields->buffer.windowTicks() != window) {
            (void)m_fields->buffer.setWindowTicks(static_cast<core::BufferTicks>(window));
            cancelPending();
        }
    }
    bool pushButton(PlayerButton button) {
        ++pushes;
        if (!replaying && playerIndex(this) >= 0 && button == PlayerButton::Jump) {
            syncConfiguration();
            bool const pendingBefore = m_fields->buffer.hasPendingPress();
            bool const touchesOrb = m_touchingRings && m_touchingRings->count() != 0;
            // The raw native press is ALWAYS delivered. Observe and remember it;
            // do not fabricate a return value or interpret bool as acceptance.
            (void)m_fields->buffer.onPhysicalPress(m_fields->tick,
                                                   ready(this) || touchesOrb || !canAssist(this));
            if (!pendingBefore && m_fields->buffer.hasPendingPress()) ++queued;
        }
        bool const result = PlayerObject::pushButton(button);
        auto& r = returns[modeFor(this)];
        result ? ++r.yes : ++r.no;
        return result;
    }
    bool releaseButton(PlayerButton button) {
        ++releases;
        if (!replaying && playerIndex(this) >= 0 && button == PlayerButton::Jump)
            (void)m_fields->buffer.onPhysicalRelease();
        return PlayerObject::releaseButton(button);
    }
    void update(float dt) {
        int const i = playerIndex(this);
        if (i < 0) { PlayerObject::update(dt); return; }
        ++m_fields->tick; // Native player-update CALL clock; cadence still needs proof.
        ++updates[i]; playerDt[i] = dt; // Keep native units; do not guess Hz.
        syncConfiguration();
        if (m_isDead) clearBuffer();
        m_fields->insideUpdate = true;
        PlayerObject::update(dt);
        m_fields->insideUpdate = false;
    }
    void updateJump(float dt) {
        int const i = playerIndex(this);
        if (i >= 0) ++jumps[i];
        core::NativeActions action{};
        if (i >= 0 && m_fields->insideUpdate && m_fields->processedTick != m_fields->tick) {
            m_fields->processedTick = m_fields->tick;
            action = m_fields->buffer.onPhysicsStep(m_fields->tick, canAssist(this) && ready(this));
            if (action.expired) ++expired;
        }
        // A still-held physical press already follows native behavior. Replay
        // only taps released before landing, never an extra down for a raw hold.
        bool const replayTap = action.bufferedFire && !m_fields->buffer.physicalHeld();
        if (replayTap) {
            ReplayScope scope;
            assisted = true; // BEFORE injection / possible nested save calls.
            ++fired;
            m_fields->syntheticHold = true;
            this->pushButton(PlayerButton::Jump);
        }
        // Hold the synthetic edge through native updateJump, not merely through
        // pushButton. Immediate push+release before updateJump can erase a tap.
        PlayerObject::updateJump(dt);
        if (replayTap && m_fields->syntheticHold) {
            ReplayScope scope;
            this->releaseButton(PlayerButton::Jump);
            m_fields->syntheticHold = false;
        }
    }
    void incrementJumps() {
        if (playerIndex(this) >= 0) cancelPending(); // Native action already occurred.
        PlayerObject::incrementJumps();
    }
    void ringJump(RingObject* object, bool skipCheck) {
        // Conservative cancellation avoids replay after a possible orb action.
        // This is not an orb-validity/success predicate; orb replay is unsupported.
        if (playerIndex(this) >= 0) cancelPending();
        PlayerObject::ringJump(object, skipCheck);
    }
};

namespace {
void clearPlayers(PlayLayer* l) {
    if (!l) return;
    if (l->m_player1) static_cast<BufferPlayer*>(l->m_player1)->clearBuffer();
    if (l->m_player2) static_cast<BufferPlayer*>(l->m_player2)->clearBuffer();
}
void settingsChanged() {
    clearPlayers(PlayLayer::get());
    overlayElapsed = 1.f;
    log::info("[InputBuffer] stage={} enabled={} all/wave={}/{} SafeMode={} (incomplete protection)",
              stage, enabled, allTicks(), waveTicks(), safeMode);
}
struct BoundaryScope {
    BoundaryScope(PlayLayer* l) { ++boundaryDepth; clearPlayers(l); }
    ~BoundaryScope() { if (--boundaryDepth == 0) resetAttempt(); }
};
void overlay(PlayLayer* l, float dt) {
    if (!showDebug) {
        if (l->getChildByTag(overlayTag)) l->removeChildByTag(overlayTag, true);
        return;
    }
    overlayElapsed += dt; logElapsed += dt;
    if (overlayElapsed < .2f && l->getChildByTag(overlayTag)) return;
    overlayElapsed = 0.f;
    auto* label = static_cast<cocos2d::CCLabelBMFont*>(l->getChildByTag(overlayTag));
    if (!label) {
        label = cocos2d::CCLabelBMFont::create("Input Buffer", "bigFont.fnt");
        if (!label) { log::error("[InputBuffer] overlay creation failed"); return; }
        label->setAnchorPoint({0.f, 1.f}); l->addChild(label, 10000, overlayTag);
    }
    auto const size = cocos2d::CCDirector::sharedDirector()->getWinSize();
    label->setPosition({8.f, size.height - 12.f});
    char text[1800];
    std::snprintf(text, sizeof(text),
        "INPUT BUFFER 0.3.0 - DEVELOPMENT, NOT A RELEASE\n"
        "Loaded: %s | enabled %s | stage %" PRId64 " | all/wave ticks %d/%d\n"
        "Wave P1/P2: %s/%s | Nearby tested: 0 | overrides: 0\n"
        "Prediction: NOT IMPLEMENTED (exact simulator unavailable)\n"
        "Buffered taps fired: %" PRIu64 " | queued %" PRIu64 " expired %" PRIu64 " cancelled %" PRIu64 "\n"
        "Hooks push/release/update/destroy/reset: %s/%s/%s/%s/%s\n"
        "Commands %" PRIu64 " dt %.6g | native m_currentStep %d\n"
        "Player updates P1/P2 %" PRIu64 "/%" PRIu64 " dt %.6g/%.6g\n"
        "updateJump P1/P2 %" PRIu64 "/%" PRIu64 " (cadence unproven)\n"
        "push true/false cube %" PRIu64 "/%" PRIu64 " ship %" PRIu64 "/%" PRIu64 " ball %" PRIu64 "/%" PRIu64 "\n"
        "ufo %" PRIu64 "/%" PRIu64 " wave %" PRIu64 "/%" PRIu64 " robot %" PRIu64 "/%" PRIu64 " spider %" PRIu64 "/%" PRIu64 " swing %" PRIu64 "/%" PRIu64 "\n"
        "Safe Mode %s | ASSIST ACTIVE %s | known save calls blocked %" PRIu64 "\n"
        "INCOMPLETE protection; new normal assists gated when Safe Mode ON\n"
        "Prototype: classic cube/robot ground taps. Test Practice mode.",
        loaded ? "YES" : "NO", enabled ? "YES" : "NO", stage, allTicks(), waveTicks(),
        l->m_player1 && l->m_player1->m_isDart ? "YES" : "NO",
        l->m_player2 && l->m_player2->m_isDart ? "YES" : "NO",
        fired, queued, expired, cancelled,
        pushes ? "YES" : "NO", releases ? "YES" : "NO", updates[0] || updates[1] ? "YES" : "NO",
        destroyFired ? "YES" : "NO", resetFired ? "YES" : "NO", commands, commandDt, nativeStep,
        updates[0], updates[1], playerDt[0], playerDt[1], jumps[0], jumps[1],
        returns[0].yes, returns[0].no, returns[1].yes, returns[1].no, returns[2].yes, returns[2].no,
        returns[3].yes, returns[3].no, returns[4].yes, returns[4].no, returns[5].yes, returns[5].no,
        returns[6].yes, returns[6].no, returns[7].yes, returns[7].no,
        safeMode ? "ON" : "OFF", assisted ? "YES" : "NO", blocked);
    label->setString(text);
    label->limitLabelWidth(std::max(180.f, size.width - 16.f), .34f, .20f);
    if (logElapsed >= 1.f) { logElapsed = 0.f; log::info("[InputBuffer]\n{}", text); }
}
}

class $modify(BufferGameLayer, GJBaseGameLayer) {
    void processCommands(float dt, bool halfTick, bool lastTick) {
        auto* l = PlayLayer::get();
        if (l && static_cast<GJBaseGameLayer*>(l) == this) {
            ++commands; commandDt = dt; nativeStep = m_currentStep;
        }
        GJBaseGameLayer::processCommands(dt, halfTick, lastTick);
    }
};
class $modify(BufferPlayLayer, PlayLayer) {
    void postUpdate(float dt) { PlayLayer::postUpdate(dt); overlay(this, dt); }
    void onEnterTransitionDidFinish() { resetAttempt(); PlayLayer::onEnterTransitionDidFinish(); }
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        destroyFired = true; clearPlayers(this);
        PlayLayer::destroyPlayer(player, object); // Keep assisted flag for native saves.
    }
    void resetLevel() { resetFired = true; BoundaryScope s(this); PlayLayer::resetLevel(); }
    void onQuit() { BoundaryScope s(this); PlayLayer::onQuit(); }
    void onExit() { BoundaryScope s(this); PlayLayer::onExit(); }
    void levelComplete() {
        // Known guard only, NOT a proven all-rewards/server transaction boundary.
        bool const previous = m_isTestMode;
        bool const priorGuard = completionGuard;
        if (blockSave(this)) { ++blocked; completionGuard = true; m_isTestMode = true; }
        PlayLayer::levelComplete();
        m_isTestMode = previous;
        completionGuard = priorGuard;
    }
};
class $modify(BufferLevelSaves, GJGameLevel) {
    void savePercentage(int percent, bool practice, int clicks, int attempts, bool valid) {
        if (!practice && blockSave(PlayLayer::get(), this)) { ++blocked; return; }
        GJGameLevel::savePercentage(percent, practice, clicks, attempts, valid);
    }
    void saveNewScore(int value, int type, int ticks, int clicks, int coins, gd::string inputs, bool save) {
        if (blockSave(PlayLayer::get(), this)) { ++blocked; return; }
        GJGameLevel::saveNewScore(value, type, ticks, clicks, coins, inputs, save);
    }
    void storeNewLocalScore(int value, int type) {
        if (blockSave(PlayLayer::get(), this)) { ++blocked; return; }
        GJGameLevel::storeNewLocalScore(value, type);
    }
};
class $modify(BufferKnownRewards, GameStatsManager) {
    void completedLevel(GJGameLevel* level) {
        if (blockSave(PlayLayer::get(), level)) { ++blocked; return; }
        GameStatsManager::completedLevel(level);
    }
    void completedStarLevel(GJGameLevel* level) {
        if (blockSave(PlayLayer::get(), level)) { ++blocked; return; }
        GameStatsManager::completedStarLevel(level);
    }
    void completedDemonLevel(GJGameLevel* level) {
        if (blockSave(PlayLayer::get(), level)) { ++blocked; return; }
        GameStatsManager::completedDemonLevel(level);
    }
    void awardCurrencyForLevel(GJGameLevel* level) {
        if (blockSave(PlayLayer::get(), level)) { ++blocked; return; }
        GameStatsManager::awardCurrencyForLevel(level);
    }
    void awardDiamondsForLevel(GJGameLevel* level) {
        if (blockSave(PlayLayer::get(), level)) { ++blocked; return; }
        GameStatsManager::awardDiamondsForLevel(level);
    }
};
$on_mod(Loaded) {
    loaded = true;
    stage = std::clamp<std::int64_t>(Mod::get()->getSettingValue<std::int64_t>("buffer-stage"), 1, 5);
    enabled = Mod::get()->getSettingValue<bool>("enabled");
    safeMode = Mod::get()->getSettingValue<bool>("safe-mode");
    showDebug = Mod::get()->getSettingValue<bool>("show-debug-overlay");
    listenForSettingChanges<std::int64_t>("buffer-stage", [](std::int64_t v) {
        stage = std::clamp<std::int64_t>(v, 1, 5); settingsChanged();
    });
    listenForSettingChanges<bool>("enabled", [](bool v) { enabled = v; settingsChanged(); });
    listenForSettingChanges<bool>("safe-mode", [](bool v) { safeMode = v; settingsChanged(); });
    listenForSettingChanges<bool>("show-debug-overlay", [](bool v) {
        showDebug = v; if (auto* l = PlayLayer::get()) overlay(l, 1.f);
    });
    settingsChanged();
    log::warn("[InputBuffer] Source prototype: exact wave prediction and COMPLETE Safe Mode are unfinished. No on-device behavior claim. Test Practice.");
}
