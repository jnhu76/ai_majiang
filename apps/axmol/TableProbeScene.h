#pragma once
#include "axmol.h"
#include "aimj/game.hpp"
#include "aimj/director.hpp"
#include "aimj/runtime.hpp"
#include <memory>
#include <string>

// Source overlay for an Axmol 2.11.x C++ template project.
// Deliberately uses the SAME game/character/worker core as the CLI harness.
class TableProbeScene final : public ax::Scene {
public:
    bool init() override;
    void update(float dt) override;
    void onExit() override;
    AX_CREATE_FUNC(TableProbeScene);
private:
    void tap_tile();
    aimj::GameSession game_{std::make_unique<aimj::DemoRuleSet>()};
    aimj::CharacterDirector director_{};
    std::unique_ptr<aimj::AiRuntime> ai_;
    ax::Label* dialogue_label_{nullptr};
    ax::Label* status_label_{nullptr};
    std::string last_audio_path_;
};
