#include <Geode/Geode.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace geode::prelude;

namespace {
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

    struct BufferedPress {
        PlayerObject* player;
        PlayerButton button;
        int pressedAtStep;
        int maxTicks;
        bool releaseRequested;
    };

    struct DeferredRelease {
        PlayerObject* player;
        PlayerButton button;
        int releaseAtStep;
    };

    std::int64_t currentStage = 1;
    std::vector<BufferedPress> bufferedPresses;
    std::vector<DeferredRelease> deferredReleases;
    bool replayingBufferedInput = false;
    int lastProcessedStep = -1;

    void logStage() {
        auto const& stage = stages[currentStage - 1];
        if (!stage.enabled) {
            log::info("[InputBuffer] Stage 5: OFF (100% Raw Input)");
            return;
        }
        log::info(
            "[InputBuffer] Stage {}: {} Ticks All | {} Ticks Wave",
            currentStage,
            stage.allTicks,
            stage.waveTicks
        );
    }

    void clearBuffers() {
        bufferedPresses.clear();
        deferredReleases.clear();
    }

    bool isActivePlayer(PlayLayer* layer, PlayerObject* player) {
        return layer && player && (layer->m_player1 == player || layer->m_player2 == player);
    }

    int bufferLimitFor(PlayerObject* player) {
        if (currentStage < 1 || currentStage > 5) return 0;
        auto const& stage = stages[currentStage - 1];
        if (!stage.enabled || !player) return 0;
        return player->m_isDart ? stage.waveTicks : stage.allTicks;
    }

    auto findBufferedPress(PlayerObject* player, PlayerButton button) {
        return std::find_if(
            bufferedPresses.begin(), bufferedPresses.end(),
            [player, button](BufferedPress const& press) {
                return press.player == player && press.button == button;
            }
        );
    }

    void applyStage(std::int64_t value) {
        currentStage = std::clamp<std::int64_t>(value, 1, 5);
        if (currentStage == 5) {
            // Switching to raw input drops any still-pending presses.
            clearBuffers();
        }
        logStage();
    }

    void processBufferedInput(PlayLayer* layer, int step) {
        // Release any buffered tap one simulation step after its delayed press.
        for (auto it = deferredReleases.begin(); it != deferredReleases.end();) {
            if (!isActivePlayer(layer, it->player) || it->player->m_isDead) {
                it = deferredReleases.erase(it);
                continue;
            }
            if (step < it->releaseAtStep) {
                ++it;
                continue;
            }

            replayingBufferedInput = true;
            it->player->releaseButton(it->button);
            replayingBufferedInput = false;
            it = deferredReleases.erase(it);
        }

        for (auto it = bufferedPresses.begin(); it != bufferedPresses.end();) {
            if (!isActivePlayer(layer, it->player) || it->player->m_isDead) {
                it = bufferedPresses.erase(it);
                continue;
            }

            int const age = step - it->pressedAtStep;
            if (age <= 0) {
                ++it;
                continue;
            }
            if (age > it->maxTicks) {
                it = bufferedPresses.erase(it);
                continue;
            }

            // An already-held button means the press landed; don't send it twice.
            bool accepted = it->player->buttonDown(it->button);
            if (!accepted) {
                // Retry at most once per game step; the native bool result tells us
                // whether PlayerObject accepted the press, preventing duplicate fires.
                replayingBufferedInput = true;
                accepted = it->player->pushButton(it->button);
                replayingBufferedInput = false;
            }

            if (!accepted) {
                ++it;
                continue;
            }

            if (it->releaseRequested) {
                deferredReleases.push_back({it->player, it->button, step + 1});
            }
            it = bufferedPresses.erase(it);
        }
    }
}

class $modify(PredictiveInputBufferPlayer, PlayerObject) {
    bool pushButton(PlayerButton button) {
        if (replayingBufferedInput) {
            return PlayerObject::pushButton(button);
        }

        auto* layer = PlayLayer::get();
        if (!isActivePlayer(layer, this) || currentStage == 5 || this->m_isDead) {
            return PlayerObject::pushButton(button);
        }

        // Do not create another pending copy for a button already waiting.
        if (findBufferedPress(this, button) != bufferedPresses.end()) {
            return false;
        }

        int const limit = bufferLimitFor(this);
        if (limit <= 0 || this->buttonDown(button)) {
            return PlayerObject::pushButton(button);
        }

        bool const accepted = PlayerObject::pushButton(button);
        if (!accepted) {
            // A rejected early press is retried for at most the selected number
            // of subsequent game steps, separately for each player and button.
            bufferedPresses.push_back({this, button, layer->m_currentStep, limit, false});
        }
        return accepted;
    }

    bool releaseButton(PlayerButton button) {
        if (!replayingBufferedInput) {
            auto pending = findBufferedPress(this, button);
            if (pending != bufferedPresses.end()) {
                // Preserve a short tap: release only after its delayed press lands.
                pending->releaseRequested = true;
                return false;
            }
        }
        return PlayerObject::releaseButton(button);
    }
};

class $modify(PredictiveInputBufferGameLayer, GJBaseGameLayer) {
    void update(float dt) {
        GJBaseGameLayer::update(dt);

        auto* layer = PlayLayer::get();
        if (!layer || static_cast<GJBaseGameLayer*>(layer) != this) return;

        int const step = this->m_currentStep;
        if (step == lastProcessedStep) return;
        lastProcessedStep = step;
        processBufferedInput(layer, step);
    }
};

class $modify(PredictiveInputBufferPlayLayer, PlayLayer) {
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        // A death invalidates every pending press in the current attempt.
        clearBuffers();
        PlayLayer::destroyPlayer(player, object);
    }

    void resetLevel() {
        clearBuffers();
        PlayLayer::resetLevel();
        lastProcessedStep = this->m_currentStep;
    }

    void onQuit() {
        clearBuffers();
        PlayLayer::onQuit();
    }

    void onExit() {
        clearBuffers();
        PlayLayer::onExit();
    }
};

$on_mod(Loaded) {
    applyStage(Mod::get()->getSettingValue<std::int64_t>("buffer-stage"));
    listenForSettingChanges<std::int64_t>("buffer-stage", [](std::int64_t value) {
        applyStage(value);
    });
    log::info("[InputBuffer] Loaded. Set the stage in this mod's settings.");
}
