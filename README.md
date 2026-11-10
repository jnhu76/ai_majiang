# AI Mahjong · Offline Native

基于 **C++20 / Axmol** 的 Windows、Android、iOS 原生离线 AI 麻将项目。

## 当前状态

本仓库处于架构与技术验证阶段。目前尚无可发布的完整麻将产品；**main** 仅保存已接受的项目基线与稳定产物，不包含尚未验证的试验性代码。

## 分支约定

| 分支 | 作用 | 合入门禁 |
| --- | --- | --- |
| `main` | 稳定、已实现且验收的内容 | 从 `dev` 通过 PR 晋级；测试通过、有验收证据 |
| `dev` | 日常主要开发与集成分支 | 功能 PR；CI 通过；禁止直接混入未验证原型 |
| `prototype/p0-ai-runtime` | 纯原生架构技术原型与性能边界实验 | 独立演进，待验证后按模块拆分 PR → `dev` |
| `feature/*`, `fix/*` | 功能和修复短期分支 | PR → `dev` |

**禁止将 Mock 基准数据当作 MNN-Pocket-0.3B 或 sherpa-onnx 的真实性能。** 
原型分支允许含实验性适配器，但应标识哪些仅完成接口或 Linux 编译，哪些已在目标设备实测。

## 产品基线（已确认的架构决策）

- Windows x64 / Android arm64 / iOS arm64；原生离线运行。
- 游戏、规则与角色系统：C++20；渲染与场景：Axmol 2.x LTS。
- 权威对局：确定性状态机；Bot 只接收可见信息；语言模型不参与规则裁决。
- 角色语音：异步 LLM / TTS、过期结果丢弃、有界队列与降级。
- 目标模型：MNN-Pocket-0.3B、sherpa-onnx；实际 SDK 集成与端侧性能尚待验证。

## 日常工作方式

1. 以 `dev` 为工作基线，新建 `feature/*` / `fix/*` 分支。
2. 在专题原型分支开展硬件与系统边界测试，不直接合并进 `main`。
3. 验收后拆成可审阅的小 PR 合入 `dev`；稳定里程碑从 `dev` 提 PR 至 `main`。
4. 真实设备测试需要报告 CPU/GPU、模型 SHA、配置、功耗温度、峰值内存与 p50/p95 延迟。

详细分支门禁见 [docs/BRANCHING.md](docs/BRANCHING.md)。
