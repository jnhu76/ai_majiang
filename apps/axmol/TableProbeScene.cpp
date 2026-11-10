#include "TableProbeScene.h"
#include "aimj/wav.hpp"
#include "audio/AudioEngine.h"
#include <chrono>
#include <string>

bool TableProbeScene::init() {
    if(!ax::Scene::init()) return false;
    auto size=ax::Director::getInstance()->getVisibleSize();
    auto origin=ax::Director::getInstance()->getVisibleOrigin();
    auto background=ax::LayerColor::create(ax::Color4B(19,91,65,255));
    addChild(background,-1);
    auto title=ax::Label::createWithSystemFont("AI Mahjong - Architecture Probe", "Arial", 30);
    title->setPosition(origin+ax::Vec2(size.width/2,size.height*0.85f));
    addChild(title);
    dialogue_label_=ax::Label::createWithSystemFont("Tap the demo tile", "Arial", 22);
    dialogue_label_->setPosition(origin+ax::Vec2(size.width/2,size.height*0.63f));
    addChild(dialogue_label_);
    status_label_=ax::Label::createWithSystemFont("3 roles / 1 shared LLM / 1 TTS", "Arial", 18);
    status_label_->setPosition(origin+ax::Vec2(size.width/2,size.height*0.35f));
    addChild(status_label_);
    auto button_label=ax::Label::createWithSystemFont("[ DEMO TILE ]", "Arial", 30);
    auto button=ax::MenuItemLabel::create(button_label,[this](ax::Ref*){tap_tile();});
    auto menu=ax::Menu::create(button,nullptr);
    menu->setPosition(origin+ax::Vec2(size.width/2,size.height*0.49f));
    addChild(menu);
    // The default is MOCK; replace factories only after native libraries, models
    // and licensing have been validated on each platform.
    ai_=std::make_unique<aimj::AiRuntime>(
        std::make_unique<aimj::MockLlm>(std::chrono::milliseconds(50)),
        std::make_unique<aimj::MockTts>(std::chrono::milliseconds(20)),4);
    ai_->set_active_session(game_.state().session_id);
    scheduleUpdate();
    return true;
}
void TableProbeScene::tap_tile() {
    auto events=game_.submit({.kind=aimj::ActionKind::TapDemoTile,
                             .seat=0,.tile=3,.expected_version=game_.state().version});
    for(const auto& e:events) {
        auto req=director_.on_event(e,std::chrono::milliseconds(2000));
        if(!req) continue;
        if(!ai_->submit(*req)) dialogue_label_->setString("Queue full - fallback voice");
        else dialogue_label_->setString("Role thinking...");
    }
    status_label_->setString("Events: "+std::to_string(game_.state().tap_count));
}
void TableProbeScene::update(float) {
    if(!ai_) return;
    // Non-blocking result polling, never infer model/PCM completion from frame timing.
    while(auto result=ai_->poll()) {
        if(result->session_id!=game_.state().session_id || result->status=="stale") continue;
        dialogue_label_->setString(result->text.empty()?result->status:result->text);
        if(result->status=="ok" && !result->audio.samples.empty()) {
            // First-cut integration: WAV disk bridge. This is NOT the eventual
            // zero-copy/streaming audio path; benchmark its extra latency.
            auto file=ax::FileUtils::getInstance()->getWritablePath()
                +"aimj_"+std::to_string(result->session_id)+"_"+
                std::to_string(result->event_id)+".wav";
            try {
                aimj::write_pcm16_wav(file,result->audio);
                ax::AudioEngine::play2d(file,false,0.65f);
                last_audio_path_=std::move(file);
            } catch(...) {status_label_->setString("Audio write failed");}
        }
    }
}
void TableProbeScene::onExit() {
    unscheduleUpdate();
    ai_.reset();
    ax::Scene::onExit();
}
