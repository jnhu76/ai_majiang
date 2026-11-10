#pragma once
#include "aimj/game.hpp"
#include <chrono>
#include <optional>
#include <string>
namespace aimj {
using Clock = std::chrono::steady_clock;
struct DialogueRequest {
    SessionId session_id{};
    EventId event_id{};
    int speaker{};                 // 1..3 are the AI players
    std::string persona;
    std::string prompt;
    std::string fallback_line;
    int priority{1};
    Clock::time_point created{};
    Clock::time_point deadline{};
};
class CharacterDirector {
public:
    std::optional<DialogueRequest> on_event(const GameEvent& event,
                                             std::chrono::milliseconds deadline) const;
};
} // namespace aimj
