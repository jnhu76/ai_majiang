#include "aimj/director.hpp"
#include <array>
namespace aimj {
std::optional<DialogueRequest> CharacterDirector::on_event(const GameEvent& e,
                                                     std::chrono::milliseconds budget) const {
    if (e.kind != EventKind::DemoTileTapped) return std::nullopt;
    // Three distinct speakers, with bounded context. No hidden tiles are supplied.
    constexpr std::array<const char*,3> personas{"嘟嘟：调皮、喜欢叠词", "急急哥：暴躁但不辱骂", "淡定猫：冷幽默"};
    constexpr std::array<const char*,3> fallbacks{"你这牌打得肥嘟嘟的！", "别磨蹭，快出牌！", "嗯，局势很有趣。"};
    auto idx = static_cast<std::size_t>((e.state_version - 1) % 3);
    auto now = Clock::now();
    DialogueRequest r;
    r.session_id=e.session_id;
    r.event_id=e.event_id;
    r.speaker=static_cast<int>(idx + 1);
    r.persona=personas[idx];
    r.fallback_line=fallbacks[idx];
    r.created=now;
    r.deadline=now+budget;
    r.prompt="你在一个虚构的麻将游戏中扮演"+r.persona+
      "。玩家刚点击了一张演示牌。用中文吐槽一句，20字内，不要解释，不要冒充真人。";
    return r;
}
} // namespace aimj
