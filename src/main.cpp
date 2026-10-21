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
#include "trajectory_simulator.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
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
std::uint64_t fired = 0, queued = 0, expired = 0, cancelled = 0, blocked = 0, overrides = 0;
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
namespace trajectory = trajectory_prediction;
constexpr std::size_t maxPredictionObjects = 256;
enum class PredictionState : std::uint8_t { NotRun, SafeNow, Delay, NoSafeDelay, Unsupported };
enum class PredictionBlock : std::uint8_t {
    None, NotClassicCube, UnsupportedMotion, InvalidPhysics, DynamicGeometry,
    Slope, Interaction, TooManyObjects
};
struct LivePrediction {
    std::uint32_t objectCount = 0;
    std::uint8_t candidateCount = 0;
    std::uint8_t delay = 0;
    PredictionState state = PredictionState::NotRun;
    PredictionBlock block = PredictionBlock::None;
};
char const* predictionText(PredictionState state) {
    switch (state) {
        case PredictionState::SafeNow: return "NOW SAFE";
        case PredictionState::Delay: return "DELAY SELECTED";
        case PredictionState::NoSafeDelay: return "NO SAFE DELAY";
        case PredictionState::Unsupported: return "UNSUPPORTED";
        default: return "NOT RUN";
    }
}
char const* predictionBlockText(PredictionBlock block) {
    switch (block) {
        case PredictionBlock::NotClassicCube: return "cube-only model";
        case PredictionBlock::UnsupportedMotion: return "mode/state unsupported";
        case PredictionBlock::InvalidPhysics: return "invalid native physics";
        case PredictionBlock::DynamicGeometry: return "dynamic geometry nearby";
        case PredictionBlock::Slope: return "slope collision not modeled";
        case PredictionBlock::Interaction: return "orb/pad/mode/speed interaction nearby";
        case PredictionBlock::TooManyObjects: return "object cap reached";
        default: return "none";
    }
}
bool isInteractionType(GameObjectType type) {
    switch (type) {
        case GameObjectType::YellowJumpPad: case GameObjectType::PinkJumpPad:
        case GameObjectType::GravityPad: case GameObjectType::RedJumpPad:
        case GameObjectType::SpiderPad: case GameObjectType::YellowJumpRing:
        case GameObjectType::PinkJumpRing: case GameObjectType::GravityRing:
        case GameObjectType::GreenRing: case GameObjectType::DropRing:
        case GameObjectType::RedJumpRing: case GameObjectType::CustomRing:
        case GameObjectType::DashRing: case GameObjectType::GravityDashRing:
        case GameObjectType::SpiderOrb: case GameObjectType::TeleportOrb:
        case GameObjectType::ShipPortal: case GameObjectType::CubePortal:
        case GameObjectType::BallPortal: case GameObjectType::UfoPortal:
        case GameObjectType::RobotPortal: case GameObjectType::SpiderPortal:
        case GameObjectType::SwingPortal: case GameObjectType::WavePortal:
        case GameObjectType::InverseMirrorPortal: case GameObjectType::NormalMirrorPortal:
        case GameObjectType::RegularSizePortal: case GameObjectType::MiniSizePortal:
        case GameObjectType::DualPortal: case GameObjectType::SoloPortal:
        case GameObjectType::TeleportPortal: case GameObjectType::Modifier:
        case GameObjectType::CollisionObject: case GameObjectType::AnimatedHazard:
            return true;
        default: return false;
    }
}
bool ignoredStaticType(GameObjectType type) {
    return type == GameObjectType::Decoration || type == GameObjectType::Collectible ||
           type == GameObjectType::UserCoin || type == GameObjectType::SecretCoin;
}
LivePrediction predictClassicCube(PlayerObject* player, float dt, int maxDelay,
                                 std::array<trajectory::Object, maxPredictionObjects>& storage) {
    LivePrediction result{};
    result.state = PredictionState::Unsupported;
    auto refuse = [&](PredictionBlock why) {
        result.block = why;
        return result;
    };
    if (modeFor(player) != 0) return refuse(PredictionBlock::NotClassicCube);
    if (!player->m_gameLayer || player->m_isPlatformer || player->m_isDashing ||
        player->m_isSideways || player->m_isOnSlope || player->m_wasOnSlope ||
        player->m_isDead || !std::isfinite(dt) || dt <= 0.f || dt > 1.f)
        return refuse(PredictionBlock::UnsupportedMotion);
    if (!std::isfinite(player->m_yVelocity) || !std::isfinite(player->m_yStart) ||
        !std::isfinite(player->m_gravity) || !std::isfinite(player->m_gravityMod) ||
        !std::isfinite(player->getCurrentXVelocity()) || player->m_yStart <= 0.0 ||
        player->m_gravity <= 0.0 || player->m_gravityMod <= 0.0)
        return refuse(PredictionBlock::InvalidPhysics);

    constexpr std::uint8_t lookahead = 40;
    const auto window = static_cast<std::uint8_t>(std::clamp(maxDelay, 0, 16));
    if (window == 0 || window >= lookahead) return refuse(PredictionBlock::UnsupportedMotion);

    trajectory::State state{};
    auto const position = player->getPosition();
    auto const& playerRect = player->getObjectRect();
    if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(playerRect.size.width) || !std::isfinite(playerRect.size.height) ||
        playerRect.size.width <= 0.f || playerRect.size.height <= 0.f)
        return refuse(PredictionBlock::InvalidPhysics);
    state.position = {position.x, position.y};
    state.velocity = {player->getCurrentXVelocity(), player->m_yVelocity};
    state.halfWidth = playerRect.size.width * 0.5;
    state.halfHeight = playerRect.size.height * 0.5;
    state.gravity = player->m_isUpsideDown ? 1.0 : -1.0;
    state.mode = trajectory::Mode::Cube;
    state.grounded = player->m_isOnGround || player->m_isOnGround2;

    trajectory::PhysicsProfile physics{};
    physics.gravityAcceleration = player->m_gravity * player->m_gravityMod;
    physics.horizontalSpeed = player->getCurrentXVelocity();
    physics.jumpImpulse = player->m_yStart * (player->m_vehicleSize != 1.0f ? 0.8 : 1.0);
    physics.maxAbsVerticalSpeed = 15.0;
    physics.timeStep = dt;
    physics.substeps = 1;
    if (!std::isfinite(physics.jumpImpulse) || physics.jumpImpulse <= 0.0 ||
        !std::isfinite(physics.gravityAcceleration) || physics.gravityAcceleration <= 0.0)
        return refuse(PredictionBlock::InvalidPhysics);

    const double jumpHeight = physics.jumpImpulse * physics.jumpImpulse /
                              (2.0 * physics.gravityAcceleration) + state.halfHeight + 48.0;
    const double reach = std::abs(physics.horizontalSpeed) * dt * lookahead + state.halfWidth + 32.0;
    const double left = state.position.x - state.halfWidth - 8.0;
    const double right = state.position.x + reach;
    const double bottom = state.position.y - std::max(160.0, jumpHeight);
    const double top = state.position.y + std::max(160.0, jumpHeight);

    auto* gameObjects = player->m_gameLayer->m_objects;
    if (!gameObjects) return refuse(PredictionBlock::InvalidPhysics);
    const unsigned int total = gameObjects->count();
    bool lastGroundIncluded = false;
    for (unsigned int i = 0; i < total; ++i) {
        auto* object = static_cast<GameObject*>(gameObjects->objectAtIndex(i));
        if (!object || object->m_isDisabled || object->m_isDisabled2 || object->m_isGroupDisabled ||
            object->m_isGroupDisabledTemp) continue;
        const GameObjectType type = object->getType();
        if (object == player->m_lastGroundObject) lastGroundIncluded = true;
        if (ignoredStaticType(type)) continue;
        auto const& rect = object->getObjectRect();
        const double objectLeft = rect.origin.x;
        const double objectBottom = rect.origin.y;
        const double objectRight = objectLeft + rect.size.width;
        const double objectTop = objectBottom + rect.size.height;
        if (!std::isfinite(objectLeft) || !std::isfinite(objectBottom) ||
            !std::isfinite(objectRight) || !std::isfinite(objectTop) ||
            objectRight < left || objectLeft > right || objectTop < bottom || objectBottom > top)
            continue;

        if (isInteractionType(type)) return refuse(PredictionBlock::Interaction);
        if (type == GameObjectType::Slope) return refuse(PredictionBlock::Slope);
        if (object->m_isTrigger || object->m_groupCount > 0 || object->m_hasGroupParent ||
            object->m_hasAreaParent || object->getHasRotateAction() ||
            std::abs(object->getObjectRotation()) > 0.01f)
            return refuse(PredictionBlock::DynamicGeometry);

        trajectory::Object snapshot{};
        snapshot.bounds = {objectLeft, objectBottom, objectRight, objectTop};
        snapshot.enabled = true;
        if (type == GameObjectType::Hazard) {
            snapshot.kind = trajectory::ObjectKind::Hazard;
        } else if (type == GameObjectType::Solid || type == GameObjectType::Breakable) {
            if (object->m_isPassable || object->m_isNoTouch) continue;
            snapshot.kind = trajectory::ObjectKind::Solid;
        } else if (type == GameObjectType::InverseGravityPortal ||
                   type == GameObjectType::NormalGravityPortal ||
                   type == GameObjectType::GravityTogglePortal) {
            snapshot.kind = trajectory::ObjectKind::GravityPortal;
            snapshot.value = type == GameObjectType::InverseGravityPortal ? 1.0 :
                             type == GameObjectType::NormalGravityPortal ? -1.0 : 0.0;
        } else if (object->m_isTrigger) {
            return refuse(PredictionBlock::Interaction);
        } else {
            continue;
        }
        if (result.objectCount == storage.size())
            return refuse(PredictionBlock::TooManyObjects);
        storage[result.objectCount++] = snapshot;
    }

    // The normal base floor is rendered by GJGroundLayer rather than always
    // appearing in m_objects. When the live player confirms grounded contact
    // with no specific ground object, anchor a flat support surface to its exact
    // current feet height; do not invent one for an airborne player.
    if (!lastGroundIncluded && !player->m_lastGroundObject && state.grounded &&
        state.gravity < 0.0 && player->m_gameLayer->m_groundLayer &&
        result.objectCount < storage.size()) {
        storage[result.objectCount++] = {trajectory::ObjectKind::Solid,
            {left, state.position.y - state.halfHeight - 10000.0,
             right + 64.0, state.position.y - state.halfHeight}, 0.0, true};
    }

    const auto decision = trajectory::latestSurvivingClick(
        window, lookahead, [&](std::uint8_t delay, std::uint8_t horizon) {
            return trajectory::survivesClick(state, physics, storage.data(),
                                             result.objectCount, delay, horizon, 1);
        });
    result.candidateCount = decision.testedCandidates;
    if (decision.delay) {
        result.state = PredictionState::Delay;
        result.delay = decision.delayTicks;
    } else if (trajectory::survivesClick(state, physics, storage.data(),
                                         result.objectCount, 0, lookahead, 1)) {
        result.state = PredictionState::SafeNow;
    } else {
        result.state = PredictionState::NoSafeDelay;
    }
    result.block = PredictionBlock::None;
    return result;
}
PredictionState lastPredictionState[2] = {PredictionState::NotRun, PredictionState::NotRun};
PredictionBlock lastPredictionBlock[2] = {PredictionBlock::None, PredictionBlock::None};
std::uint32_t lastPredictionObjects[2] = {}, lastPredictionCalls = 0;
std::uint8_t lastPredictionCandidates[2] = {}, lastPredictionDelay[2] = {};
void recordPrediction(int player, LivePrediction const& prediction) {
    if (player < 0 || player > 1) return;
    lastPredictionState[player] = prediction.state;
    lastPredictionBlock[player] = prediction.block;
    lastPredictionObjects[player] = prediction.objectCount;
    lastPredictionCandidates[player] = prediction.candidateCount;
    lastPredictionDelay[player] = prediction.delay;
    ++lastPredictionCalls;
}
struct ReplayScope {
    bool before = replaying;
    ReplayScope() { replaying = true; }
    ~ReplayScope() { replaying = before; }
};
void resetAttempt() {
    assisted = false;
    fired = queued = expired = cancelled = blocked = overrides = 0;
    overlayElapsed = 1.f;
}
}

class $modify(BufferPlayer, PlayerObject) {
public:
    struct Fields {
        core::EarlyPressBuffer buffer;
        core::PhysicsStep tick = 0, processedTick = static_cast<core::PhysicsStep>(-1);
        bool syntheticHold = false, insideUpdate = false;
        bool predictionSuppressedPending = false;
        core::PhysicsStep predictionFireTick = 0;
        float lastUpdateDt = 0.f;
        std::array<trajectory::Object, maxPredictionObjects> predictionObjects{};
    };
    void cancelPending() {
        if (m_fields->buffer.hasPendingPress()) {
            ++cancelled;
            (void)m_fields->buffer.clearAll();
            m_fields->predictionSuppressedPending = false;
        }
    }
    void clearBuffer() {
        // Raw holds are never released by this helper. Only our temporary replay.
        if (m_fields->syntheticHold) {
            ReplayScope scope;
            this->releaseButton(PlayerButton::Jump);
            m_fields->syntheticHold = false;
        }
        (void)m_fields->buffer.clearAll();
        m_fields->predictionSuppressedPending = false;
        m_fields->predictionFireTick = 0;
    }
    void syncConfiguration() {
        int const window = canAssist(this) ? allTicks() : 0;
        if (m_fields->buffer.windowTicks() != window) {
            auto const config = m_fields->buffer.setWindowTicks(static_cast<core::BufferTicks>(window));
            if (config.actions.push || config.actions.release) {
                ReplayScope scope;
                if (config.actions.push) (void)PlayerObject::pushButton(PlayerButton::Jump);
                if (config.actions.release) (void)PlayerObject::releaseButton(PlayerButton::Jump);
                if (config.actions.push) assisted = true;
            }
            if (m_fields->predictionSuppressedPending) ++cancelled;
            m_fields->predictionSuppressedPending = false;
            m_fields->predictionFireTick = 0;
        }
    }
    bool pushButton(PlayerButton button) {
        ++pushes;
        if (!replaying && playerIndex(this) >= 0 && button == PlayerButton::Jump) {
            syncConfiguration();
            const int i = playerIndex(this);
            bool const pendingBefore = m_fields->buffer.hasPendingPress();
            bool const touchesOrb = m_touchingRings && m_touchingRings->count() != 0;
            if (pendingBefore) return false; // one outstanding delayed edge per player
            if (canAssist(this) && !touchesOrb && allTicks() > 0) {
                auto prediction = predictClassicCube(this, m_fields->lastUpdateDt, allTicks(),
                                                     m_fields->predictionObjects);
                recordPrediction(i, prediction);
                if (prediction.state == PredictionState::Delay) {
                    const auto firstConsumerTick = m_fields->tick + (m_fields->insideUpdate ? 0 : 1);
                    (void)m_fields->buffer.onPhysicalPress(firstConsumerTick, false);
                    auto const fireAt = firstConsumerTick + prediction.delay;
                    if (m_fields->buffer.schedulePending(fireAt)) {
                        m_fields->predictionSuppressedPending = true;
                        m_fields->predictionFireTick = fireAt;
                        ++queued;
                        ++overrides;
                        return false; // suppress now; the predicted click is injected later
                    }
                    (void)m_fields->buffer.clearAll();
                }
            }
        }
        bool const result = PlayerObject::pushButton(button);
        auto& r = returns[modeFor(this)];
        result ? ++r.yes : ++r.no;
        return result;
    }
    bool releaseButton(PlayerButton button) {
        ++releases;
        if (!replaying && playerIndex(this) >= 0 && button == PlayerButton::Jump) {
            bool const wasPending = m_fields->buffer.hasPendingPress();
            (void)m_fields->buffer.onPhysicalRelease();
            if (wasPending && !m_fields->syntheticHold) return false;
        }
        if (m_fields->syntheticHold) m_fields->syntheticHold = false;
        return PlayerObject::releaseButton(button);
    }
    void update(float dt) {
        int const i = playerIndex(this);
        if (i < 0) { PlayerObject::update(dt); return; }
        ++m_fields->tick; // Native player-update CALL clock; cadence still needs proof.
        ++updates[i]; playerDt[i] = dt; // Keep native units; do not guess Hz.
        m_fields->lastUpdateDt = dt;
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
            if (m_fields->predictionSuppressedPending && m_fields->tick >= m_fields->predictionFireTick &&
                (m_fields->tick != m_fields->predictionFireTick || !canAssist(this) || !ready(this))) {
                cancelPending(); // discard rather than fire on a different simulated tick/state
            } else {
                action = m_fields->buffer.onPhysicsStep(
                    m_fields->tick, m_fields->predictionSuppressedPending && canAssist(this) && ready(this));
            }
            if (action.expired) ++expired;
        }
        bool const replayPredicted = action.bufferedFire && m_fields->predictionSuppressedPending;
        if (replayPredicted) {
            ReplayScope scope;
            assisted = true; // BEFORE injection / possible nested save calls.
            ++fired;
            m_fields->syntheticHold = true;
            this->pushButton(PlayerButton::Jump);
            m_fields->predictionSuppressedPending = false;
        }
        // Keep the predicted edge through native updateJump; a released tap is
        // released only after the game has consumed this physics-step input.
        PlayerObject::updateJump(dt);
        if (replayPredicted && action.release && m_fields->syntheticHold) {
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
        "Wave P1/P2: %s/%s | prediction calls: %" PRIu32 " | overrides: %" PRIu64 "\n"
        "Live cube P1/P2: %s / %s | objects %" PRIu32 "/%" PRIu32 " candidates %u/%u delay %u/%u\n"
        "P1/P2 limits: %s / %s (orb/pad/mode/dynamic cases bypass)\n"
        "Buffered taps fired: %" PRIu64 " | queued %" PRIu64 " expired %" PRIu64 " cancelled %" PRIu64 "\n"
        "Hooks push/release/update/destroy/reset: %s/%s/%s/%s/%s\n"
        "Commands %" PRIu64 " dt %.6g | native m_currentStep %d\n"
        "Player updates P1/P2 %" PRIu64 "/%" PRIu64 " dt %.6g/%.6g\n"
        "updateJump P1/P2 %" PRIu64 "/%" PRIu64 " (cadence unproven)\n"
        "push true/false cube %" PRIu64 "/%" PRIu64 " ship %" PRIu64 "/%" PRIu64 " ball %" PRIu64 "/%" PRIu64 "\n"
        "ufo %" PRIu64 "/%" PRIu64 " wave %" PRIu64 "/%" PRIu64 " robot %" PRIu64 "/%" PRIu64 " spider %" PRIu64 "/%" PRIu64 " swing %" PRIu64 "/%" PRIu64 "\n"
        "Safe Mode %s | ASSIST ACTIVE %s | known save calls blocked %" PRIu64 "\n"
        "INCOMPLETE protection; new normal assists gated when Safe Mode ON\n"
        "Static AABB predictor; intervention only on a selected cube delay. Practice mode.",
        loaded ? "YES" : "NO", enabled ? "YES" : "NO", stage, allTicks(), waveTicks(),
        l->m_player1 && l->m_player1->m_isDart ? "YES" : "NO",
        l->m_player2 && l->m_player2->m_isDart ? "YES" : "NO",
        lastPredictionCalls, overrides,
        predictionText(lastPredictionState[0]), predictionText(lastPredictionState[1]),
        lastPredictionObjects[0], lastPredictionObjects[1],
        lastPredictionCandidates[0], lastPredictionCandidates[1],
        lastPredictionDelay[0], lastPredictionDelay[1],
        predictionBlockText(lastPredictionBlock[0]), predictionBlockText(lastPredictionBlock[1]),
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
