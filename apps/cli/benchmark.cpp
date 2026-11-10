#include "aimj/game.hpp"
#include "aimj/director.hpp"
#include "aimj/runtime.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#if defined(__linux__)
#include <sys/resource.h>
#elif defined(__APPLE__)
#include <sys/resource.h>
#elif defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif
using namespace aimj;
using namespace std::chrono_literals;
namespace {
struct Opt {
    int events=60, hz=10, llm_delay=30, tts_delay=20, deadline=2000, queue=4, settle_ms=30000;
    std::string out="benchmarks/results/run.jsonl", llm="mock",tts="mock";
    std::string mnn_config,tts_model,tts_tokens,tts_lexicon;
    int tts_threads=2;
};
std::string quote(const std::string& s) {
    std::string out="\"";
    for (unsigned char c:s) {
        if(c=='"') out+="\\\"";
        else if(c=='\\') out+="\\\\";
        else if(c=='\n') out+="\\n";
        else if(c<0x20) out+='?';
        else out+=static_cast<char>(c);
    }
    return out+'"';
}
Opt parse(int argc,char**argv) {
    Opt o;
    for(int i=1;i<argc;i++) {
        auto key=std::string(argv[i]);
        if(key=="--help") {
            std::cout<<"aimj_bench options: --events N --hz N --llm-delay-ms N --tts-delay-ms N --deadline-ms N --queue N --settle-ms N --out PATH --llm mock|mnn --tts mock|sherpa --mnn-config PATH --tts-model PATH --tts-tokens PATH --tts-lexicon PATH --tts-threads N\n";
            std::exit(0);
        }
        if(i+1>=argc) throw std::invalid_argument("missing value for "+key);
        auto v=std::string(argv[++i]);
        if(key=="--events") o.events=std::stoi(v);
        else if(key=="--hz") o.hz=std::stoi(v);
        else if(key=="--llm-delay-ms") o.llm_delay=std::stoi(v);
        else if(key=="--tts-delay-ms") o.tts_delay=std::stoi(v);
        else if(key=="--deadline-ms") o.deadline=std::stoi(v);
        else if(key=="--queue") o.queue=std::stoi(v);
        else if(key=="--settle-ms") o.settle_ms=std::stoi(v);
        else if(key=="--out") o.out=v;
        else if(key=="--llm") o.llm=v;
        else if(key=="--tts") o.tts=v;
        else if(key=="--mnn-config") o.mnn_config=v;
        else if(key=="--tts-model") o.tts_model=v;
        else if(key=="--tts-tokens") o.tts_tokens=v;
        else if(key=="--tts-lexicon") o.tts_lexicon=v;
        else if(key=="--tts-threads") o.tts_threads=std::stoi(v);
        else throw std::invalid_argument("unknown option "+key);
    }
    if(o.events<1 || o.hz<1 || o.deadline<1 || o.queue<1 || o.settle_ms<1 || o.llm_delay<0 || o.tts_delay<0 || o.tts_threads<1)
        throw std::invalid_argument("invalid numeric options");
    return o;
}
std::uint64_t peak_rss_bytes() {
#if defined(__linux__) || defined(__APPLE__)
    struct rusage ru{};
    if(getrusage(RUSAGE_SELF,&ru)!=0) return 0;
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(ru.ru_maxrss);
#else
    return static_cast<std::uint64_t>(ru.ru_maxrss)*1024;
#endif
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS p{};
    if(GetProcessMemoryInfo(GetCurrentProcess(),&p,sizeof(p))) return p.PeakWorkingSetSize;
    return 0;
#else
    return 0;
#endif
}
double percentile(std::vector<double> values,double p) {
    if(values.empty()) return 0;
    std::sort(values.begin(),values.end());
    auto idx=static_cast<std::size_t>(std::ceil(p*values.size()));
    return values[std::min(values.size()-1,std::max(std::size_t(1),idx)-1)];
}
}
int main(int argc,char**argv) {
    try {
        auto o=parse(argc,argv);
        auto provider_init_start=Clock::now();
        std::unique_ptr<ILlmBackend> llm;
        std::unique_ptr<ITtsBackend> tts;
        if(o.llm=="mock") llm=std::make_unique<MockLlm>(std::chrono::milliseconds(o.llm_delay));
#ifdef AIMJ_WITH_MNN
        else if(o.llm=="mnn") llm=make_mnn_backend(o.mnn_config);
#endif
        else throw std::invalid_argument("LLM provider unavailable: "+o.llm);
        if(o.tts=="mock") tts=std::make_unique<MockTts>(std::chrono::milliseconds(o.tts_delay));
#ifdef AIMJ_WITH_SHERPA
        else if(o.tts=="sherpa") tts=make_sherpa_vits_backend(o.tts_model,o.tts_tokens,o.tts_lexicon,o.tts_threads);
#endif
        else throw std::invalid_argument("TTS provider unavailable: "+o.tts);
        auto init_ms=std::chrono::duration<double,std::milli>(Clock::now()-provider_init_start).count();
        auto llm_name=llm->name(),tts_name=tts->name();
        auto runtime_start=Clock::now();
        AiRuntime runtime(std::move(llm),std::move(tts),static_cast<std::size_t>(o.queue));
        GameSession game(std::make_unique<DemoRuleSet>(),42);
        CharacterDirector director;
        std::ofstream file(o.out);
        if(!file) throw std::runtime_error("cannot open output file: "+o.out+" (create parent directory first)");
        const bool simulated=(o.llm=="mock" || o.tts=="mock");
        file<<"{\"type\":\"run\",\"schema\":1,\"mode\":"<<quote(simulated?"contains_mock":"real")
            <<",\"llm\":"<<quote(llm_name)<<",\"tts\":"<<quote(tts_name)
            <<",\"events\":"<<o.events<<",\"hz\":"<<o.hz<<",\"queue\":"<<o.queue
            <<",\"deadline_ms\":"<<o.deadline<<",\"provider_init_ms\":"<<init_ms<<"}\n";
        auto dispatch_start=Clock::now();
        auto period=std::chrono::duration<double>(1.0/o.hz);
        int rejected=0;
        for(int i=0;i<o.events;i++) {
            std::this_thread::sleep_until(dispatch_start+std::chrono::duration_cast<Clock::duration>(period*i));
            auto events=game.submit(Action{.kind=ActionKind::TapDemoTile,.seat=0,.tile=i%34,.expected_version=game.state().version});
            for (auto& e:events) {
                auto req=director.on_event(e,std::chrono::milliseconds(o.deadline));
                if(req && !runtime.submit(std::move(*req))) ++rejected;
            }
        }
        auto dispatch_end=Clock::now();
        int completed=0,ok=0;
        std::vector<double> e2es,llms,ttss,queues;
        auto collect_until=dispatch_end+std::chrono::milliseconds(o.settle_ms);
        while(Clock::now()<collect_until && completed+rejected<o.events) {
            auto result=runtime.poll();
            if(!result) { std::this_thread::sleep_for(1ms); continue; }
            ++completed;
            if(result->status=="ok") { ++ok;e2es.push_back(result->end_to_end_ms); }
            llms.push_back(result->llm_ms);ttss.push_back(result->tts_ms);queues.push_back(result->queue_llm_ms);
            file<<std::fixed<<std::setprecision(3)
                <<"{\"type\":\"event\",\"session_id\":"<<result->session_id
                <<",\"event_id\":"<<result->event_id<<",\"speaker\":"<<result->speaker
                <<",\"status\":"<<quote(result->status)
                <<",\"queue_llm_ms\":"<<result->queue_llm_ms
                <<",\"llm_ms\":"<<result->llm_ms
                <<",\"queue_tts_ms\":"<<result->queue_tts_ms
                <<",\"tts_ms\":"<<result->tts_ms
                <<",\"end_to_end_ms\":"<<result->end_to_end_ms
                <<",\"pcm_samples\":"<<result->audio.samples.size()<<"}\n";
        }
        auto stats=runtime.counters();
        auto missing=o.events-rejected-completed;
        auto rss=peak_rss_bytes();
        file<<"{\"type\":\"summary\",\"offered\":"<<o.events
            <<",\"accepted\":"<<stats.accepted<<",\"rejected\":"<<stats.queue_rejected
            <<",\"completed\":"<<completed<<",\"ok\":"<<ok<<",\"missing\":"<<missing
            <<",\"expired\":"<<stats.expired<<",\"stale\":"<<stats.stale
            <<",\"failed\":"<<stats.failed<<",\"result_overflow\":"<<stats.result_overflow
            <<",\"e2e_p50_ms\":"<<percentile(e2es,0.5)<<",\"e2e_p95_ms\":"<<percentile(e2es,0.95)
            <<",\"llm_p95_ms\":"<<percentile(llms,0.95)<<",\"tts_p95_ms\":"<<percentile(ttss,0.95)
            <<",\"queue_p95_ms\":"<<percentile(queues,0.95)
            <<",\"peak_rss_bytes\":"<<rss<<"}\n";
        file.flush();
        auto elapsed=std::chrono::duration<double>(Clock::now()-runtime_start).count();
        std::cout<<"mode="<<(simulated?"CONTAINS MOCK - NOT MODEL PERFORMANCE":"REAL PROVIDERS")
                 <<" offered="<<o.events<<" accepted="<<stats.accepted
                 <<" rejected="<<stats.queue_rejected<<" completed="<<completed<<" ok="<<ok
                 <<" missing="<<missing<<" e2e_p95_ms="<<percentile(e2es,.95)
                 <<" peak_rss_bytes="<<rss<<" elapsed_s="<<elapsed<<" out="<<o.out<<"\n";
        return missing==0 ? 0 : 2;
    } catch(const std::exception& e) { std::cerr<<"ERROR: "<<e.what()<<'\n';return 1; }
}
