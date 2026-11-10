#include "aimj/game.hpp"
namespace aimj {
std::vector<GameEvent> DemoRuleSet::apply(TableState& state, const Action& action) const {
    if (action.kind != ActionKind::TapDemoTile || action.seat != 0 || action.tile < 0 || action.tile > 33)
        throw std::invalid_argument("invalid demo action");
    ++state.tap_count;
    ++state.version;
    return {GameEvent{.session_id=state.session_id, .state_version=state.version,
                      .kind=EventKind::DemoTileTapped, .actor_seat=action.seat, .tile=action.tile}};
}
GameSession::GameSession(std::unique_ptr<IRuleSet> rules, std::uint64_t seed) : rules_(std::move(rules)) {
    if (!rules_) throw std::invalid_argument("rule set required");
    state_.seed = seed;
}
std::vector<GameEvent> GameSession::submit(const Action& action) {
    if (action.expected_version != state_.version) throw std::invalid_argument("stale game command");
    auto events = rules_->apply(state_, action);
    for (auto& e : events) { e.session_id=state_.session_id; e.event_id=next_event_id_++; e.state_version=state_.version; }
    return events;
}
void GameSession::new_session(std::uint64_t seed) {
    auto next = state_.session_id + 1;
    state_ = TableState{.session_id=next, .seed=seed};
    next_event_id_ = 1;
}
} // namespace aimj
