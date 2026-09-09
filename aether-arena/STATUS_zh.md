# AetherArena — Build Status

Tracks ADR-149 implementation milestones. "Complete" = benchmark **infrastructure** done,
跟踪ADR-149的实施里程碑。“完成”=基准**基础设施**完成,
tested, CI-gated, deploy-ready, RuView baseline entered, §7 acceptance test passing.
已测试，CI 门控，部署就绪，RuView 基线已输入，第7节验收测试通过。
Model **SOTA** (e.g. MM-Fi PCK@20 ~72%) is a separate long-running ML effort, blocked on
模型 **SOTA**（例如 MM-Fi PCK@20 ~72%）是一个单独的长期运行的机器学习项目，目前受阻于
ADR-079 camera-ground-truth collection — *not* an infra-completion blocker.
ADR-079 相机地面实况收集 — *不是* 红外完成阻塞因素。

| # | Milestone | Status |
| # | 里程碑 | 状态 |
|---|-----------|--------|
| M1 | ADR-149 Accepted + committed | ✅ done |
| M1 | ADR-149 已接受 提交 | ✅ 完成 |
| M2 | Scorer runner (`aa_score_runner`) — **real model scoring** + witness (proof+inputs hash) + **repeatability analysis** | ✅ done — builds `--no-default-features`, determinism gate PASS, repeatable 16/16 |
| M2 | 评分运行器 (`aa_score_runner`) — **真实模型评分**   见证（输入哈希证明）   **可重复性分析** | ✅ 已完成 — 构建 `--no-default-features`，确定性门通过，可重复 16/16 |
| M3 | CI harness-gate workflow (PR runs scorer + repeatability + real-scoring smoke + ledger verify) | ✅ done — `.github/workflows/aether-arena-harness.yml` |
| M3 | CI 装置门工作流（PR 运行评分器   可重复性   实时评分测试   分类账验证） | ✅ 完成 — `.github/workflows/aether-arena-harness.yml` |
| M4 | Scaffold: README + submission schema + VERIFY (acceptance test) | ✅ done |
| M4 | 脚手架：README 提交模式 验证（验收测试） | ✅ 已完成 |
| M5 | Public smoke split (committed) + private MM-Fi held-out split prep | 🟡 smoke split done (`fixtures/smoke_*.json`); private MM-Fi prep pending |
| M5 | 公共 smoke 分割（已提交）   私有 MM-Fi 保留分割准备 | 🟡 smoke 分割完成（`fixtures/smoke_*.json`）；私有 MM-Fi 准备待处理 |
| M6 | HF Space (Gradio) — leaderboard + ledger integrity + submit/verify/about | ✅ deployed → https://huggingface.co/spaces/ruvnet/aether-arena (sandboxed scorer container = later hardening) |
| M6 | HF Space (Gradio) — 排行榜   账本完整性   提交/验证/关于 | ✅ 已部署 → https://huggingface.co/spaces/ruvnet/aether-arena (沙箱评分容器 = 后续加固) |
| M7 | **Witness ledger chain** — append-only, hash-chained, tamper-evident | ✅ done — `ledger/ledger_tools.py` (seed/append/verify); tamper test fails as designed |
| M7 | **见证账本链** — 仅追加、哈希链、篡改可见 | ✅ 完成 — `ledger/ledger_tools.py`（种子/追加/验证）；篡改测试按设计失败 |
| M8 | Public launch | ✅ Space **LIVE** (gradio 5.9.1, serving 200) — **board empty, awaiting first real harness score** (benchmark-first: no seeded numbers) |
| M8 | 公开发布 | ✅ Space **LIVE** (gradio 5.9.1，服务 200) — **板空，等待第一个真实的 harness 分数**（基准测试优先：无种子数字） |

## v0 infrastructure: COMPLETE
Implement ✅ · Test ✅ · Deploy to HF ✅ (https://huggingface.co/spaces/ruvnet/aether-arena) · Instructions+Verification ✅ · PR runs the harness ✅ (PR #874, AA harness gate **passed**).
实现 ✅ · 测试 ✅ · 部署到 HF ✅ (https://huggingface.co/spaces/ruvnet/aether-arena) · 指令验证 ✅ · PR 运行了框架 ✅ (PR #874, AA 框架门 **通过**)。
Remaining = data + hardening, not infra: private MM-Fi held-out split (M5), sandboxed scorer container (M6), privacy-leakage attacker (gated category), and **model SOTA** (separate ML effort, blocked on ADR-079 — explicitly not an infra exit).
剩余 = 数据加固，而不是基础设施：私有 MM-Fi 保留分割（M5）、沙箱评分器容器（M6）、隐私泄露攻击者（受控类别），以及 **模型 SOTA**（单独的机器学习工作，受 ADR-079 阻塞——明确不是基础设施退出）。

## Benchmark-first posture (per user direction)
- **No placeholder numbers on the board.** The ledger seeds to genesis only; every result is a real scoring-pipeline witness. RuView gets no seeded baseline.
- **棋盘上没有占位符数字。**账本只从创世块开始；每个结果都是实际计分流程的见证。RuView 不获得任何预设基线。
- **Witness chain** = `inputs_sha256` (binds witness to exact inputs) + `proof_sha256` (cross-platform-stable score hash) + the append-only hash-chained ledger. Repeatability analysis (`--repeat N`) proves the proof hash is identical across runs.
- **见证链** = `inputs_sha256`（将见证绑定到精确输入） `proof_sha256`（跨平台稳定的分数哈希） 追加式哈希链账本。可重复性分析（`--repeat N`）证明在多次运行中，证明哈希是相同的。

## Blockers / decisions needed
- **HF deploy (M6)** — token is in GCP Secret Manager (`HUGGINGFACE_API_KEY`); creating the public `ruvnet/aether-arena` Space still wants explicit go.
- **HF 部署 (M6)** — 令牌保存在 GCP Secret Manager（`HUGGINGFACE_API_KEY`）；创建公共 `ruvnet/aether-arena` Space 仍然需要明确确认。
- **MM-Fi is CC BY-NC** → AA must stay non-commercial / legally distinct from the commercial RuView product.
- **MM-Fi 是 CC BY-NC** → AA 必须保持非商业性质 / 与商业 RuView 产品在法律上区分开。
- **Private MM-Fi split (M5)** — needs the dataset pulled + a held-out split assembled before real public scoring replaces the smoke fixture.
- **私有 MM-Fi 划分（M5）** — 需要提取数据集，在真正的公共评分替换测试装置之前组装一个保留的划分。
