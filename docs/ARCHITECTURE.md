# Architecture contract — v0.2

## Non-negotiable properties

1. One native process per app; no HTTP server, no Python runtime, no cloud service. Large authoring models (GPT-6-class “teacher” LLMs) are **development-time tools only**, never a game runtime dependency; runtime ships only declarative, reviewed content packs. See [ADR-0001](adr/0001-npc-offline-authoring-pipeline.md).
2. `GameSession` exclusively owns authoritative game state. All state changes originate from validated game commands (`expected_version`). DemoRuleSet is an intentional placeholder, **not** a partial Mahjong rule engine.
3. Axmol presentation receives events and may animate or skip animations; it never decides rules or invokes inference on the rendering thread.
4. `CharacterDirector` sees **public GameEvent/PlayerObservation only**, never hidden opponent hands. It selects speaker/persona and creates a bounded request.
5. Exactly one LLM worker and one TTS worker per scene for v0.1, with bounded pending queues. Stale session output is discarded. Model call cannot necessarily be preempted mid-generation; results are invalidated on session switch.
6. AI failures must not change scores, rules or turns. Fallback lines are display-only and do not trigger fake TTS success.
7. No network at runtime; voice packs, model resources and content packs are deployed with the app.
8. Benchmark results must identify mock vs real providers. No mock timing is a claim about hardware inference.

## Dependency direction

```text
App/Axmol scene ───────> GameSession ───────> IRuleSet
        │                     │
        │                     └────────> GameEvent
        │                                     │
        └───────────────> CharacterDirector ──┘
                                   │
                                   v
                           Bounded AiRuntime
                            /            \
                     ILlmBackend      ITtsBackend
                      mock/MNN        mock/sherpa
                                   │
                             SpeechResult
                                   │
                                   v
                           App/Axmol scene
```

`GameSession` and the future real mahjong rule engine contain **no Axmol, MNN or sherpa headers**. `AiRuntime` owns provider instances and threads, but not the session. UI polls results on its own thread.

## Chosen over alternatives

- Not client/server even locally: unnecessary server lifecycle, latency and security surface for a four-player offline game.
- Not ECS: no high-entity-count transforms or per-frame gameplay simulation to justify its costs.
- Not free-form LLM game controller: model output is inherently non-deterministic and unsuitable for legality/score authority.
- Not three model instances: model memory and competition are likely worse on mobile; measure shared-instance queue pressure first.

## Known risks

- One worker means queueing under bursts; benchmark event rate vs deadline and quantify rejected/expired outputs.
- Model cold start, TTS warmup, GPU backend contention and platform-specific memory pressure can dominate user experience.
- Provider cancellation stops future queueing but does not necessarily interrupt an in-flight native call. Final UX may need pause/resume and model teardown policies.
- `TableProbeScene` is an **unverified Axmol overlay** until compiled against the exact engine tag.
- The C++ MNN and sherpa adapters are optional. The [Windows native experiment](experiments/2026-11-10-win-native-real/REPORT.md) linked and exercised pinned builds on one host.

## NPC content: offline authoring vs. runtime execution (v0.2)

Formal decision: [ADR-0001](adr/0001-npc-offline-authoring-pipeline.md) —
*NPC Offline Authoring and Runtime Behavior Separation*. Summary:

```text
Authoring path (dev time):  Teacher LLM + human review -> Scenario Catalog + BehaviorPacks (declarative data)
Proposed NPC runtime:       public events / SocialCue -> ScenarioMatcher -> EmotionReducer -> SocialDirector
                            -> IDialoguePolicy (weighted default; optional on-device model ranks offered LineIds only)
Evaluation path:            coverage (nominal/triggerable/delivered) + golden corpus + blind paired tests
```

Boundaries: the teacher model never runs on player devices and never touches rules,
wall, hands, scores or any authoritative state; emotions are deterministic
declarative rules, never model output; the on-device model may only select among
offered candidate line IDs; silence is a first-class outcome. Contracts:
[`docs/npc/SCENARIO-MODEL.md`](npc/SCENARIO-MODEL.md),
[`docs/npc/BEHAVIOR-PACK-V1.md`](npc/BEHAVIOR-PACK-V1.md),
[`docs/npc/AUTHORING-PIPELINE.md`](npc/AUTHORING-PIPELINE.md),
[`docs/npc/EVALUATION-PLAN.md`](npc/EVALUATION-PLAN.md).

Delivery placement: this lands as content capability feeding the review roadmap's
playable-MVP and character-content phases; the immediate next slice is the
headless golden-corpus runner (catalog loader + matcher + emotion reducer +
SocialDirector's minimal scheduling, and weighted policy), delivered in its own PR.
The checked-in Schema and offline validator cover documentation fixtures.

Change history: v0.2 adds the offline authoring/runtime separation (ADR-0001) on
top of v0.1; no v0.1 property was weakened.
