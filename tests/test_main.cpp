#include "aimj/game.hpp"
#include "aimj/director.hpp"
#include "aimj/runtime.hpp"
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <limits>
#include "aimj/textscore.hpp"
#include "aimj/wav.hpp"
#define CHECK(expr) do { if (!(expr)) { std::cerr << "CHECK failed: " #expr << " (line " << __LINE__ << ")\n"; throw std::runtime_error("CHECK failed: " #expr); } } while(false)
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
using namespace std::chrono_literals;
using namespace aimj;
static Action button(const GameSession& s,int tile=3) {
    return Action{.kind=ActionKind::TapDemoTile,.seat=0,.tile=tile,.expected_version=s.state().version};
}
static int run() {
    GameSession game(std::make_unique<DemoRuleSet>());
    auto a=game.submit(button(game));
    CHECK(a.size()==1 && a[0].event_id==1 && a[0].state_version==1);
    try {game.submit(Action{.expected_version=0}); CHECK(false);} catch(const std::invalid_argument&) {}
    CharacterDirector director;
    auto d=director.on_event(a[0],1000ms);
    CHECK(d && d->speaker==1 && d->event_id==1);
    game.new_session(99);
    CHECK(game.state().session_id==2 && game.state().version==0 && game.state().seed==99);
    auto b=game.submit(button(game));
    CHECK(b[0].session_id==2 && b[0].event_id==1);
    {
        AiRuntime rt(std::make_unique<MockLlm>(2ms),std::make_unique<MockTts>(2ms),2);
        rt.set_active_session(2);
        CHECK(rt.submit(*director.on_event(b[0],500ms)));
        std::optional<SpeechResult> result;
        for(int i=0;i<200 && !result;i++) {result=rt.poll();std::this_thread::sleep_for(2ms);}
        CHECK(result && result->status=="ok" && !result->audio.samples.empty());
        CHECK(rt.counters().accepted==1);
        auto c=game.submit(button(game));
        auto old=director.on_event(c[0],500ms);
        rt.set_active_session(3);
        CHECK(rt.submit(*old));
        std::optional<SpeechResult> stale;
        for(int i=0;i<200 && !stale;i++) {stale=rt.poll();std::this_thread::sleep_for(2ms);}
        CHECK(stale && stale->status=="stale");
    }
    { // Queue pressure, no unbounded growth.
        AiRuntime rt(std::make_unique<MockLlm>(50ms),std::make_unique<MockTts>(50ms),1);
        auto first=director.on_event(b[0],1000ms);
        for(int i=0;i<40;i++) rt.submit(*first);
        CHECK(rt.counters().queue_rejected>0);
        CHECK(rt.backlog()<=2);
    }
    { // Expired LLM work cannot turn into a late spoken line.
        AiRuntime rt(std::make_unique<MockLlm>(40ms),std::make_unique<MockTts>(1ms),2);
        auto r=*director.on_event(b[0],5ms);
        r.session_id=1;
        rt.set_active_session(1);
        CHECK(rt.submit(r));
        std::optional<SpeechResult> expired;
        for(int i=0;i<200 && !expired;i++) {expired=rt.poll();std::this_thread::sleep_for(2ms);}
        CHECK(expired && expired->status=="timeout" && expired->audio.samples.empty());
    }
    { // In-memory audio export must produce a valid WAV header.
        auto path=(std::filesystem::temp_directory_path()/"aimj_test.wav").string();
        auto buffer=MockTts(0ms).synthesize("hello",1);
        write_pcm16_wav(path,buffer);
        {
            std::ifstream f(path,std::ios::binary);
            char magic[4]{};f.read(magic,4);
            CHECK(std::string(magic,4)=="RIFF");
        }
        // Windows cannot delete a file while a handle is still open.
        std::filesystem::remove(path);
    }
    { // Idle workers must be woken and joined on destruction, without hanging.
        const auto start=Clock::now();
        {
            AiRuntime rt(std::make_unique<MockLlm>(1ms),std::make_unique<MockTts>(1ms),2);
            CHECK(rt.backlog()==0);
        }
        CHECK(Clock::now()-start<2s);
    }
    { // A failing real backend (e.g. corrupt model at runtime) must degrade to the
      // fallback line with status=error, and the runtime must keep serving later events.
        struct ThrowingLlm final : ILlmBackend {
            int calls=0;
            std::string generate(const std::string&,int) override {++calls;throw std::runtime_error("model exploded");}
            std::string name() const override {return "throwing-llm";}
        };
        auto* llm=new ThrowingLlm();
        {
            AiRuntime rt(std::unique_ptr<ILlmBackend>(llm),std::make_unique<MockTts>(1ms),2);
            auto req=*director.on_event(b[0],1000ms);
            const auto fallback=req.fallback_line;
            req.session_id=1;rt.set_active_session(1);
            CHECK(rt.submit(req));
            std::optional<SpeechResult> failed;
            for(int i=0;i<200 && !failed;i++) {failed=rt.poll();std::this_thread::sleep_for(2ms);}
            CHECK(failed && failed->status=="error" && failed->text==fallback);
            CHECK(failed->llm_usage.prompt_tokens==0); // failed calls carry no usage
            auto again=*director.on_event(b[0],1000ms);
            again.session_id=1;
            CHECK(rt.submit(again));
            std::optional<SpeechResult> second;
            for(int i=0;i<200 && !second;i++) {second=rt.poll();std::this_thread::sleep_for(2ms);}
            CHECK(second && second->status=="error"); // runtime survived the first failure
            CHECK(llm->calls==2);
        }
    }
    { // A model line that misses its deadline is replaced by the fallback but
      // stays auditable: the raw text and the reason are not lost.
        AiRuntime rt(std::make_unique<MockLlm>(30ms),std::make_unique<MockTts>(1ms),2);
        auto req=*director.on_event(b[0],5ms);
        req.session_id=1;rt.set_active_session(1);
        CHECK(rt.submit(req));
        std::optional<SpeechResult> late;
        for(int i=0;i<200 && !late;i++) {late=rt.poll();std::this_thread::sleep_for(2ms);}
        CHECK(late && late->status=="timeout");
        CHECK(late->fallback_used && !late->raw_llm_text.empty());
        CHECK(late->text!=late->raw_llm_text);
        CHECK(late->pcm_completed_at!=Clock::time_point{});
    }
    { // P1 audit fields are appended and position-safe.
        LlmUsage usage{.prompt_tokens=5};
        CHECK(usage.prompt_tokens==5 && usage.decode_ms==0.0);
        CHECK(!usage.truncated && usage.status_code==-1);
        SpeechResult result;
        CHECK(!result.fallback_used && result.raw_llm_text.empty() && result.error_message.empty());
    }
    { // Negation-aware scoring: a refusal that quotes a forbidden word is not a
      // violation, while the same word in the prompt only stays visible for review.
        auto refusal=score_text("不能胡，还没结算呢","玩家打出九筒","胡","胡了;结算");
        CHECK(refusal.forbidden.size()==2);
        CHECK(refusal.forbidden[0].raw_hits==0);
        CHECK(refusal.forbidden[1].raw_hits==1 && refusal.forbidden[1].negated_hits==1);
        CHECK(refusal.violating_gate_hits==0);
        auto prompt_echo=score_text("我胡了！","不要猜胜负，比如胡了这类词","","胡了");
        CHECK(prompt_echo.forbidden[0].in_prompt && prompt_echo.violating_gate_hits==0);
        CHECK(prompt_echo.violating_prompt_echo_hits==1);
        auto real_violation=score_text("我胡了！","当前事实：玩家打出九筒。","","胡了");
        CHECK(real_violation.forbidden[0].violating_hits==1 && real_violation.violating_gate_hits==1);
        auto missing=score_text("哦。","","碰;二条","");
        CHECK(missing.required_missing==2);
    }
    { // WAV conversion is counted, and read-back validates the header instead of guessing.
        auto path=(std::filesystem::temp_directory_path()/"aimj_test_clip.wav").string();
        AudioBuffer buffer;buffer.sample_rate=8000;
        buffer.samples={1.5f,-2.0f,std::numeric_limits<float>::quiet_NaN(),0.25f};
        auto written=write_pcm16_wav(path,buffer);
        CHECK(written.clipped_samples==2 && written.non_finite_samples==1);
        auto read=read_pcm16_wav(path);
        CHECK(read.audio.sample_rate==8000 && read.audio.samples.size()==4);
        std::filesystem::remove(path);
        auto bad=(std::filesystem::temp_directory_path()/"aimj_test_bad.wav").string();
        {
            std::ofstream f(bad,std::ios::binary);
            f.write("RIFX0000WAVE",12);
        }
        bool rejected=false;
        try {read_pcm16_wav(bad);} catch(const std::runtime_error&) {rejected=true;}
        CHECK(rejected);
        std::filesystem::remove(bad);
    }
#ifdef AIMJ_WITH_MNN
    { // An OpenCL request must never be answered by silent CPU fallback. On a
      // CPU-only MNN.dll (the current build) initialization fails with
      // backend_unavailable; if an OpenCL DLL is later swapped in, the request
      // still fails before load() instead of running on CPU kernels.
        const auto diag=opencl_diagnostics();
        bool threw=false;std::string message;
        try {make_mnn_backend("this-config-does-not-exist.json",8,"opencl",0);}
        catch(const std::exception& e) {threw=true;message=e.what();}
        CHECK(threw);
        if(message.find("backend_unavailable")!=std::string::npos) {
            CHECK(!diag.mnn_opencl_symbols);
        } else {
            std::cout<<"NOTE: OpenCL-enabled MNN.dll present; backend_unavailable path not exercised\n";
            CHECK(message.find("createLLM")!=std::string::npos || message.find("load")!=std::string::npos);
        }
        bool rejected_backend=false;
        try {make_mnn_backend("x",8,"vulkan",0);} catch(const std::invalid_argument&) {rejected_backend=true;}
        CHECK(rejected_backend);
    }
#endif
    std::cout << "PASS: state ownership, stale commands, session invalidation, bounded queues, pipeline, clean shutdown\n";
    return 0;
}
int main() {
    try { return run(); }
    catch(const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
