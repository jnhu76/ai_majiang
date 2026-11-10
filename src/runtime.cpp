#include "aimj/runtime.hpp"
#include <stdexcept>
#include <utility>
namespace aimj {
namespace {
double ms(Clock::duration d) { return std::chrono::duration<double,std::milli>(d).count(); }
}
AiRuntime::AiRuntime(std::unique_ptr<ILlmBackend> llm, std::unique_ptr<ITtsBackend> tts, std::size_t cap)
  : llm_(std::move(llm)),tts_(std::move(tts)),capacity_(cap) {
    if (!llm_ || !tts_ || cap==0) throw std::invalid_argument("backends and queue capacity required");
    llm_worker_=std::jthread([this](std::stop_token stop){run_llm(stop);});
    tts_worker_=std::jthread([this](std::stop_token stop){run_tts(stop);});
}
AiRuntime::~AiRuntime() {
    llm_worker_.request_stop(); tts_worker_.request_stop();
    llm_cv_.notify_all(); tts_cv_.notify_all();
    // jthread destructors join; providers may finish a non-interruptible call before returning.
}
bool AiRuntime::submit(DialogueRequest req) {
    bool accepted=false;
    {
        std::lock_guard lock(llm_mu_);
        if(llm_queue_.size()<capacity_) {llm_queue_.push_back(std::move(req));accepted=true;}
    }
    {std::lock_guard lock(counters_mu_);if(accepted) ++counts_.accepted;else ++counts_.queue_rejected;}
    if(accepted) llm_cv_.notify_one();
    return accepted;
}
std::optional<SpeechResult> AiRuntime::poll() {
    std::lock_guard lock(results_mu_);
    if(results_.empty()) return std::nullopt;
    auto r=std::move(results_.front());results_.pop_front();return r;
}
RuntimeCounters AiRuntime::counters() const { std::lock_guard lock(counters_mu_);return counts_; }
std::size_t AiRuntime::backlog() const {
    std::scoped_lock lock(llm_mu_,tts_mu_);
    return llm_queue_.size()+tts_queue_.size();
}
void AiRuntime::publish(SpeechResult r) {
    {
        std::lock_guard lock(counters_mu_);
        ++counts_.completed;
        if(r.status=="timeout") ++counts_.expired;
        else if(r.status=="stale") ++counts_.stale;
        else if(r.status=="error") ++counts_.failed;
    }
    {
        std::lock_guard lock(results_mu_);
        if(results_.size()>=256) {
            results_.pop_front();
            std::lock_guard counter_lock(counters_mu_); ++counts_.result_overflow;
        }
        results_.push_back(std::move(r));
    }
}
void AiRuntime::run_llm(std::stop_token stop) {
    while(!stop.stop_requested()) {
        DialogueRequest req;
        {
            std::unique_lock lock(llm_mu_);
            llm_cv_.wait(lock,stop,[this]{return !llm_queue_.empty();});
            if(stop.stop_requested()) break;
            req=std::move(llm_queue_.front());llm_queue_.pop_front();
        }
        SpeechResult r;
        r.session_id=req.session_id;r.event_id=req.event_id;r.speaker=req.speaker;
        auto start=Clock::now();r.queue_llm_ms=ms(start-req.created);
        if(req.session_id!=active_session_.load()) {
            r.status="stale";r.end_to_end_ms=ms(Clock::now()-req.created);publish(std::move(r));continue;
        }
        if(start>=req.deadline) {
            r.status="timeout";r.text=req.fallback_line;
            r.end_to_end_ms=ms(Clock::now()-req.created);publish(std::move(r));continue;
        }
        try {r.text=llm_->generate(req.prompt,req.speaker);}
        catch(...) {r.status="error";r.text=req.fallback_line;}
        auto done=Clock::now();r.llm_ms=ms(done-start);
        if(req.session_id!=active_session_.load()) r.status="stale";
        else if(done>=req.deadline) {r.status="timeout";r.text=req.fallback_line;}
        if(r.status!="ok") {
            r.end_to_end_ms=ms(done-req.created);publish(std::move(r));continue;
        }
        bool queued=false;
        {
            std::lock_guard lock(tts_mu_);
            if(tts_queue_.size()<capacity_) {
                tts_queue_.push_back(TtsJob{std::move(req),std::move(r),done});queued=true;
            }
        }
        if(queued) tts_cv_.notify_one();
        else {
            r.status="error";r.text=req.fallback_line;
            r.end_to_end_ms=ms(Clock::now()-req.created);publish(std::move(r));
        }
    }
}
void AiRuntime::run_tts(std::stop_token stop) {
    while(!stop.stop_requested()) {
        TtsJob job;
        {
            std::unique_lock lock(tts_mu_);
            tts_cv_.wait(lock,stop,[this]{return !tts_queue_.empty();});
            if(stop.stop_requested()) break;
            job=std::move(tts_queue_.front());tts_queue_.pop_front();
        }
        auto start=Clock::now();job.result.queue_tts_ms=ms(start-job.queued);
        if(job.request.session_id!=active_session_.load()) job.result.status="stale";
        else if(start>=job.request.deadline) job.result.status="timeout";
        if(job.result.status=="ok") {
            try {job.result.audio=tts_->synthesize(job.result.text,job.request.speaker);}
            catch(...) {job.result.status="error";job.result.text=job.request.fallback_line;}
            if(job.request.session_id!=active_session_.load()) job.result.status="stale";
            else if(Clock::now()>=job.request.deadline) job.result.status="timeout";
        }
        job.result.tts_ms=ms(Clock::now()-start);
        job.result.end_to_end_ms=ms(Clock::now()-job.request.created);
        if(job.result.status!="ok") {
            job.result.audio.samples.clear();
            job.result.text=job.result.status=="stale" ? "" : job.request.fallback_line;
        }
        publish(std::move(job.result));
    }
}
} // namespace aimj
