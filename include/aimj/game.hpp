#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

namespace aimj {
using EventId = std::uint64_t;
using SessionId = std::uint64_t;
enum class ActionKind { TapDemoTile };
enum class EventKind { DemoTileTapped };
struct Action {
    ActionKind kind{ActionKind::TapDemoTile};
    int seat{0};
    int tile{0};  // 0..33; display-only in DemoRuleSet
    std::uint64_t expected_version{0};
};
struct TableState {
    SessionId session_id{1};
    std::uint64_t version{0};
    std::uint64_t tap_count{0};
    std::uint64_t seed{0};
};
struct GameEvent {
    SessionId session_id{};
    EventId event_id{};
    std::uint64_t state_version{};
    EventKind kind{};
    int actor_seat{};
    int tile{};
};
class IRuleSet {
public:
    virtual ~IRuleSet() = default;
    virtual std::vector<GameEvent> apply(TableState& state, const Action& action) const = 0;
};
// Deliberate proof-of-architecture rule: NOT a mahjong rule engine.
class DemoRuleSet final : public IRuleSet {
public:
    std::vector<GameEvent> apply(TableState& state, const Action& action) const override;
};
class GameSession {
public:
    GameSession(std::unique_ptr<IRuleSet> rules, std::uint64_t seed = 42);
    std::vector<GameEvent> submit(const Action& action);
    const TableState& state() const noexcept { return state_; }
    void new_session(std::uint64_t seed);
private:
    std::unique_ptr<IRuleSet> rules_;
    TableState state_{};
    EventId next_event_id_{1};
};
} // namespace aimj
