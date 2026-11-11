#include "aimj/game.hpp"
#include "aimj/director.hpp"
#include "aimj/runtime.hpp"
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <limits>
#include "aimj/textscore.hpp"
#include "aimj/wav.hpp"
#include "aimj/voice_loop.hpp"
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
      // stays auditable: the raw text and the reason are not lost. Margins are
      // wide enough that a loaded CI runner still dequeues before the deadline.
        AiRuntime rt(std::make_unique<MockLlm>(300ms),std::make_unique<MockTts>(1ms),2);
        auto req=*director.on_event(b[0],100ms);
        req.session_id=1;rt.set_active_session(1);
        CHECK(rt.submit(req));
        std::optional<SpeechResult> late;
        for(int i=0;i<500 && !late;i++) {late=rt.poll();std::this_thread::sleep_for(2ms);}
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
        // A corrupt fmt size must fail as runtime_error, not attempt a 4 GiB
        // allocation. An odd-sized fmt chunk must still honor the RIFF pad byte.
        auto huge=(std::filesystem::temp_directory_path()/"aimj_test_huge_fmt.wav").string();
        {
            std::ofstream f(huge,std::ios::binary);
            f.write("RIFF",4);const char zero[4]={0,0,0,0};f.write(zero,4);f.write("WAVE",4);
            f.write("fmt ",4);const char hugeSize[4]={static_cast<char>(0xF0),-1,-1,-1};
            f.write(hugeSize,4);const char body[16]={};f.write(body,16);
        }
        rejected=false;
        try {read_pcm16_wav(huge);} catch(const std::runtime_error&) {rejected=true;}
        CHECK(rejected);
        std::filesystem::remove(huge);
        auto padded=(std::filesystem::temp_directory_path()/"aimj_test_padded_fmt.wav").string();
        {
            std::ofstream f(padded,std::ios::binary);
            f.write("RIFF",4);const char zero[4]={0,0,0,0};f.write(zero,4);f.write("WAVE",4);
            f.write("fmt ",4);const char oddSize[4]={17,0,0,0};f.write(oddSize,4);
            const char fmt[16]={1,0,1,0,0x40,0x1F,0,0,static_cast<char>(0x80),0x3E,0,0,2,0,16,0};
            f.write(fmt,16);const char bodyExtra=0;f.write(&bodyExtra,1);
            const char pad=0;f.write(&pad,1);
            f.write("data",4);const char dataSize[4]={2,0,0,0};f.write(dataSize,4);
            const char sample[2]={0,0};f.write(sample,2);
        }
        auto paddedRead=read_pcm16_wav(padded);
        CHECK(paddedRead.audio.sample_rate==8000 && paddedRead.audio.samples.size()==1);
        std::filesystem::remove(padded);
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
    { // P2: exact fact matching, sid/speaker binding, repetition control and a
      // closed-set LLM pick that falls back when the id is invalid.
        const auto bank_path = std::filesystem::temp_directory_path() / "aimj_p2_bank.tsv";
        {
            std::ofstream f(bank_path, std::ios::binary);
            f << "# generated by test_main\n"
              << "a1\tdiscard\ttile:9tong\t1\tdu\tplayful\t九筒九筒\t8\ta.wav\t" << std::string(64, 'a') << "\t1\n"
              << "a2\tdiscard\ttile:9tong\t1\tdu\tplayful\t九筒来啦\t8\ta.wav\t" << std::string(64, 'a') << "\t1\n"
              << "a3\tdiscard\ttile:9tong\t1\tdu\tplayful\t九筒好牌\t8\ta.wav\t" << std::string(64, 'a') << "\t1\n"
              << "b1\tdiscard\ttile:1wan\t1\tdu\tplayful\t一万来啦\t8\ta.wav\t" << std::string(64, 'a') << "\t1\n"
              << "c1\tpong\taction:pong:2tiao\t2\tjiji\texcited\t碰！二条\t9\tb.wav\t" << std::string(64, 'a') << "\t1\n";
        }
        std::string error;
        const auto bank = aimj::voice::Bank::load(bank_path.string(), error);
        CHECK(bank.has_value());
        aimj::voice::PublicEvent event;
        event.event_type = "discard";
        event.fact_key = "tile:9tong";
        event.speaker = 1;
        event.persona_id = "du";
        event.public_tiles = {"九筒"};
        CHECK(bank->candidates(event).size() == 3);
        event.fact_key = "tile:1wan";
        event.public_tiles = {"一万"};
        CHECK(bank->candidates(event).size() == 1);
        event.fact_key = "tile:9tong";
        event.speaker = 2;
        event.persona_id = "jiji";
        CHECK(bank->candidates(event).empty()); // no event-type-only fallback
        aimj::voice::Director voice_director(*bank);
        const auto* first = bank->find("a1");
        CHECK(first && aimj::voice::validate(event, *first).ok == false); // speaker/sid mismatch
        aimj::voice::PublicEvent pong;
        pong.event_type = "pong";
        pong.fact_key = "action:pong:2tiao";
        pong.speaker = 2;
        pong.persona_id = "jiji";
        pong.public_tiles = {"二条"};
        CHECK(aimj::voice::validate(pong, *bank->find("c1")).ok);
        aimj::voice::PublicEvent nine = pong;
        nine.event_type = "discard";
        nine.fact_key = "tile:9tong";
        nine.speaker = 1;
        nine.persona_id = "du";
        nine.public_tiles = {"九筒"};
        std::string last;
        int repeats = 0;
        for (int i = 0; i < 6; ++i) {
            const auto selected = voice_director.select(nine, "fixed");
            CHECK(selected.entry);
            if (selected.entry->line_id == last) ++repeats;
            last = selected.entry->line_id;
        }
        CHECK(repeats == 0); // three candidates and a two-line recent window
        // A fresh director: the six fixed picks above filled the four-line
        // recent window, so this pick would otherwise be overridden.
        aimj::voice::Director llm_director(*bank);
        const auto valid = llm_director.select(nine, "llm", [](const auto&, const auto&) { return "2"; });
        CHECK(valid.llm_valid && valid.entry);
        CHECK(valid.entry->line_id == "a2"); // candidates are sorted by id
        const auto invalid = llm_director.select(nine, "llm", [](const auto&, const auto&) { return "9"; });
        CHECK(!invalid.llm_valid && invalid.entry && invalid.method == "llm_fallback");
        std::filesystem::remove(bank_path);
        // A bank that binds sid 9 to speaker 1 must fail to load.
        {
            std::ofstream f(bank_path, std::ios::binary);
            f << "bad\tdiscard\ttile:9tong\t1\tdu\tplayful\t九筒九筒\t9\ta.wav\t" << std::string(64, 'a') << "\t1\n";
        }
        error.clear();
        CHECK(!aimj::voice::Bank::load(bank_path.string(), error).has_value());
        CHECK(error.find("sid") != std::string::npos);
        std::filesystem::remove(bank_path);
    }
    std::cout << "PASS: state ownership, stale commands, session invalidation, bounded queues, pipeline, clean shutdown\n";
    return 0;
}
int main() {
    try { return run(); }
    catch(const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
