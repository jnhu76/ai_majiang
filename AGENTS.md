# AI Mahjong Lab contributor constraints

- Maintain C++20, one authoritative `GameSession`, typed events, bounded async workers.
- Never let LLM affect legal moves, scores, wall order, hidden state, or rule validity.
- `DemoRuleSet` is not Mahjong. Replace it with real rules behind IRuleSet; never advertise the demo as a legal game.
- Core must remain buildable without Axmol, MNN or sherpa; don't add an unnecessary HTTP service or dependency-injection framework.
- Never describe mock delay/PCM numbers as model throughput or real voice quality.
- Add tests for one important failure mechanism rather than duplicated fixtures/branch-by-branch tests.
- Don't push model files, personal data, unlicensed celebrity voice samples, or undocumented third-party binary artifacts.
- All 3 target platforms use a native app; real hardware CI is a separate evidence gate from headless core CI.
