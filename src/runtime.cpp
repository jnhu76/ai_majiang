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
    llm_worker_=std::thread([this]{run_llm();});
    tts_worker_=std::thread([this]{run_tts();});
}
AiRuntime::~AiRuntime() {
    {
        // Take the cv mutexes so a worker cannot miss the stop between its
        // predicate check and blocking in wait().
        std::scoped_lock lock(llm_mu_,tts_mu_);
        stopping_.store(true);
    }
    llm_cv_.notify_all(); tts_cv_.notify_all();
    // std::thread must be joined explicitly; providers may finish a non-interruptible call first.
    if(llm_worker_.joinable()) llm_worker_.join();
    if(tts_worker_.joinable()) tts_worker_.join();
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
    // Every published result carries the moment it became consumable; ok rows
    // already stamped this right after TTS, so only failure paths fill it here.
    if(r.pcm_completed_at==Clock::time_point{}) r.pcm_completed_at=Clock::now();
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
void AiRuntime::run_llm() {
    while(!stopping_.load()) {
        DialogueRequest req;
        {
            std::unique_lock lock(llm_mu_);
            llm_cv_.wait(lock,[this]{return stopping_.load() || !llm_queue_.empty();});
            if(stopping_.load()) break;
            req=std::move(llm_queue_.front());llm_queue_.pop_front();
        }
        SpeechResult r;
        r.session_id=req.session_id;r.event_id=req.event_id;r.speaker=req.speaker;
        auto start=Clock::now();r.queue_llm_ms=ms(start-req.created);
        if(req.session_id!=active_session_.load()) {
            r.status="stale";r.end_to_end_ms=ms(Clock::now()-req.created);publish(std::move(r));continue;
        }
        if(start>=req.deadline) {
            r.status="timeout";r.text=req.fallback_line;r.fallback_used=true;
            r.end_to_end_ms=ms(Clock::now()-req.created);publish(std::move(r));continue;
        }
        try {r.text=llm_->generate(req.prompt,req.speaker);r.llm_usage=llm_->last_usage();}
        catch(const std::exception& e) {
            // Ask the provider for usage once more: a backend that failed after
            // generation started may still report the status that ended the call.
            r.llm_usage=llm_->last_usage();
            r.status="error";r.error_message=e.what();r.text=req.fallback_line;r.fallback_used=true;
        }
        catch(...) {r.llm_usage=llm_->last_usage();r.status="error";r.error_message="unknown llm exception";r.text=req.fallback_line;r.fallback_used=true;}
        auto done=Clock::now();r.llm_ms=ms(done-start);
        // A model line that arrives after its turn is over must not be spoken,
        // but it stays auditable in raw_llm_text rather than being discarded.
        if(req.session_id!=active_session_.load()) {r.status="stale";r.raw_llm_text=r.text;r.text.clear();}
        else if(done>=req.deadline) {r.status="timeout";r.raw_llm_text=r.text;r.text=req.fallback_line;r.fallback_used=true;}
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
            r.status="error";r.error_message="tts queue full";
            r.raw_llm_text=r.text;r.text=req.fallback_line;r.fallback_used=true;
            r.end_to_end_ms=ms(Clock::now()-req.created);publish(std::move(r));
        }
    }
}
void AiRuntime::run_tts() {
    while(!stopping_.load()) {
        TtsJob job;
        {
            std::unique_lock lock(tts_mu_);
            tts_cv_.wait(lock,[this]{return stopping_.load() || !tts_queue_.empty();});
            if(stopping_.load()) break;
            job=std::move(tts_queue_.front());tts_queue_.pop_front();
        }
        auto start=Clock::now();job.result.queue_tts_ms=ms(start-job.queued);
        if(job.request.session_id!=active_session_.load()) job.result.status="stale";
        else if(start>=job.request.deadline) job.result.status="timeout";
        if(job.result.status=="ok") {
            try {job.result.audio=tts_->synthesize(job.result.text,job.request.speaker);}
            catch(const std::exception& e) {job.result.status="error";job.result.error_message=e.what();}
            catch(...) {job.result.status="error";job.result.error_message="unknown tts exception";}
            if(job.request.session_id!=active_session_.load()) job.result.status="stale";
            else if(Clock::now()>=job.request.deadline) job.result.status="timeout";
        }
        job.result.tts_ms=ms(Clock::now()-start);
        job.result.end_to_end_ms=ms(Clock::now()-job.request.created);
        job.result.pcm_completed_at=Clock::now();
        if(job.result.status!="ok") {
            job.result.audio.samples.clear();
            if(!job.result.text.empty()) job.result.raw_llm_text=job.result.text;
            if(job.result.status=="stale") job.result.text.clear();
            else {job.result.text=job.request.fallback_line;job.result.fallback_used=true;}
        }
        publish(std::move(job.result));
    }
}
} // namespace aimj
