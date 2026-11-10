# Architecture contract — v0.1

## Non-negotiable properties

1. One native process per app; no HTTP server, no Python runtime, no cloud service.
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
- The C++ MNN and sherpa adapters are **optional sources and have not yet been linked/tested** against pinned external SDK builds.
