# 分支模型与发布门禁

## 分支职责

- **main** — 已验证、可交付的固定内容。原型尚未达到产品验收条件，main 暂时以架构基线/工程约定为主。
- **dev** — 日常开发集成主干，所有可交付代码先经 PR 合入 dev，禁止直接把实验原型整体当成完成品合入。
- **prototype/p0-ai-runtime** — AI 牌桌技术切片与边界测量：C++20 GameSession / CharacterDirector / 有界 AI 队列 / Mock 与可选原生后端 / Axmol 对接实验。
- **feature/**、**fix/**、**research/** — 从 dev 派生；功能通过 PR 合入 dev。

## 工作流

```text
prototype/p0-ai-runtime  (独立研发，不直接污染正式分支)
           │ 按经过验证的模块拆 PR
           ▼
feature/* ───────────▶ dev ─── PR + 完整验收 ───▶ main
fix/* ───────────────▶ dev
```

1. 普通开发以 `dev` 为基线；PR 合并至少进行构建和单测。
2. 从 `dev` 晋级 `main` 的门禁：目标平台产物可构建、相关测试通过、依赖与模型许可证核对、清楚区分 Mock / 真实设备基准，附验收证据。
3. `prototype/*` 可以保存未编译的实验性适配器，但必须在 README 标明准确成熟度，且不能自动进入 main。
4. 禁止在主干提交模型权重、编译产物、密钥、个人设备数据；历史可复现性通过 lock/pin、脚本与 manifest 保证。
5. 后续建议在 GitHub Repository Settings 中：**Default branch = dev**；为 main 设置必需 PR 与 CI 门禁；dev 也建议至少要求 PR 和核心 CI。

## 性能证据

任何针对 MNN / sherpa-onnx / Windows / Android / iOS 的性能声明必须注明：

- 完整 commit SHA、第三方 SDK 和模型 SHA；
- CPU/GPU/OS/芯片、内存、线程数、量化格式；
- 输入长度、输出 token 数、TTS 文本长度、运行次数、热机策略；
- p50/p95/p99 延迟、吞吐、RSS/PSS/峰值内存、帧时间、温升；
- 是否为模拟负载；Mock 不能替代真实模型性能。
