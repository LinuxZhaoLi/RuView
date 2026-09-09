# PROOF — reproduce every claim, or find the one we can't yet

This project (RuView / wifi-densepose) has been publicly called "AI slop" and
"fake." This document is the answer: **a skeptic can clone the repo, run one
script, and have every headline claim either verified on their own machine or
shown — explicitly — as "CLAIMED, not yet reproduced (here's exactly what it
needs)."** Nothing below is asserted without a command you can run.

证明——逐一验证每一项主张，或者找出我们目前还无法证实的那一个。 该项目（RuView/ wifi-densepose）曾被外界戏称为“垃圾”和“假的”。而这份文件就是对它的回应：持怀疑态度的人可以克隆该代码库，运行一个脚本，然后每个标题所宣称的内容都能在自己的机器上得到验证，或者明确地显示为“已宣称，尚未重现（这就是它需要的具体内容）”。下面的所有内容都是在有可运行的命令的前提下才被断言的。

```bash
git clone https://github.com/ruvnet/RuView && cd RuView
bash scripts/prove.sh          # core gate + the anti-slop assertion tests
bash scripts/prove.sh --full   # also attempt the feature-gated subset
```

`prove.sh` exits 0 only if every **non-gated** claim passes. Gated claims never
fail the run; they print the prerequisite (a GPU, a dataset, real hardware, a
trained checkpoint) so you can reproduce them yourself.

只有当所有非受限制的请求都通过验证时，prove.sh 才会退出状态码 0。受限制的请求在运行过程中不会失败；它们会打印出所需的先决条件（如 GPU、数据集、实际硬件、训练好的检查点），以便您自行重现这些条件。

## Grading

- **MEASURED** — reproduced on our hardware, with the exact command recorded, and
  pinned by a test that *fails on the pre-fix code*. `prove.sh` re-runs these.
- **CLAIMED** — cited from a source, or measured by the source, but not
  reproduced in this repo's automated harness.
- **DATA-GATED / HARDWARE-GATED** — the *code path* is real and tested, but the
  *accuracy/throughput claim* needs data or hardware we don't ship. We never
  fabricate the number; the code carries a typed error or a `weights_trained`/
  provenance flag instead.
- 

# #分级

- **MEASURED** -在我们的硬件上复制，并记录准确的命令，和
  被一个在前缀代码上失败的测试所固定。‘ prove.sh ’重新运行这些。
- **声称** -引用自一个来源，或由来源衡量，但不是
  复制在这个回购的自动控制。
- **数据门控/硬件门控** - *代码路径*是真实的和经过测试的，但是
  *准确性/吞吐量声明*需要我们不发货的数据或硬件。我们从来没有
  捏造数字；代码带有键入错误或' weights_trained ' /
  代替出处标志。

## The hard gate (run on any machine with Rust + Python) 硬门（运行在任何机器与Rust + Python）

| Claim                                                | Grade              | Reproduce                                                       |
| ---------------------------------------------------- | ------------------ | --------------------------------------------------------------- |
| Rust workspace: 3,128 tests, 0 failed                | **MEASURED** | `cd v2 && cargo test --workspace --no-default-features`       |
| Deterministic CSI pipeline proof (bit-exact SHA-256) | **MEASURED** | `python archive/v1/data/proof/verify.py` → `VERDICT: PASS` |

## Anti-slop assertion tests (each fails on the pre-fix code)

| Claim                                                                                                                                                             | Grade              | Test (run via`cargo test -p <crate> <name>`)                                                |
| ----------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------ | --------------------------------------------------------------------------------------------- |
| Fusion crafted-input DoS panics are closed (ADR-156 §2.2)                                                                                                        | **MEASURED** | `wifi-densepose-ruvector :: triangulation_out_of_range_index_returns_none_no_panic`         |
| **The "Soul Signature" identity claim, honestly bounded:** on WiFi-only cardiac+respiratory channels two people are **not separable** (gap ≈ 0.0005) | **MEASURED** | `wifi-densepose-bfld :: cardiac_alone_cannot_separate_identity_matches_audit`               |
| OccWorld`predict()` is real (input-dependent), not random noise                                                                                                 | **MEASURED** | `wifi-densepose-occworld-candle :: predict_is_deterministic_for_same_input`                 |
| Pose runtime emits frames under its own default config (ADR-159 A1)                                                                                               | **MEASURED** | `cog-pose-estimation :: default_config_emits_frames_with_real_model`                        |
| Person-count flags untrained classes — no count inflation (ADR-159 A2)                                                                                           | **MEASURED** | `cog-person-count :: untrained_class_argmax_is_flagged_low_confidence`                      |
| Medical edge skills carry a "not a medical device" disclaimer (ADR-160 A1)                                                                                        | **MEASURED** | `wifi-densepose-wasm-edge :: a1_med_modules_have_clinical_disclaimer` (`--features std`)  |
| Survivor dedup 3→1, count-inflation killed (ADR-158 §2)                                                                                                         | **MEASURED** | `wifi-densepose-mat :: test_identical_vitals_no_location_dedup_to_one` (`--features mat`) |

## Measured performance (criterion; reproduce on your machine)

| Claim                                                                              | Grade                                                                     | Reproduce                                                                                                     |
| ---------------------------------------------------------------------------------- | ------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------- |
| PSD FFT-planner cache 2.0–3.1×, DTW band 2.4–4.1× (ADR-154)                    | **MEASURED**                                                        | `cd v2 && cargo bench -p wifi-densepose-signal`                                                             |
| fuse() double-clone removed ~2.17× marshalling (ADR-156)                          | **MEASURED**                                                        | `cd v2 && cargo bench -p wifi-densepose-ruvector --bench fusion_bench`                                      |
| zero-copy ORT input ~1.48× (ADR-155)                                              | **MEASURED**                                                        | `cd v2 && cargo bench -p wifi-densepose-nn --features onnx --bench onnx_bench`                              |
| pointcloud splats 9→2 passes ~1.24× (ADR-160 research)                           | **MEASURED**                                                        | `cd v2 && cargo bench -p wifi-densepose-pointcloud --bench splats_bench`                                    |
| native wlanapi multi-BSSID scan 9.74 Hz (vs netsh ~2 Hz)                           | **MEASURED (Windows)**                                              | `cd v2 && cargo test -p wifi-densepose-wifiscan -- --ignored measure_native_scan_rate`                      |
| wasm-edge`process_frame` hot-path latency (host proxy, ADR-163)                  | **MEASURED-on-host** (NOT the ESP32/WASM3 budget — needs hardware) | `cd v2/crates/wifi-densepose-wasm-edge && cargo bench --features std`                                       |
| cog steady-state CPU infer latency ~305 µs (ADR-163; NOT the manifest cold-start) | **MEASURED-on-host**                                                | `cd v2 && cargo bench -p cog-person-count -p cog-pose-estimation --no-default-features --bench infer_bench` |

## What we do NOT claim (the honest negatives — the strongest anti-slop signal)

| Capability                                                  | Status                                                                                                                                                                                                                                                        |
| ----------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Named person-identity from WiFi**                   | **NOT achieved, and measured why.** The §3.6 matcher is real, but identity does not lock on WiFi-only channels (gap 0.0005). DATA-GATED on a real enrollment feeding the AETHER/body-resonance channel — never done. No named-identity claim is made. |
| WiFlow-STD ~96% PCK@20                                      | **CLAIMED-reproduced** on our RTX 5080 (`benchmarks/wiflow-std/RESULTS.md`); HARDWARE-GATED for you (needs an NVIDIA GPU + the MM-Fi dataset). The upstream *shipped checkpoint* was **REFUTED** (0.08% PCK) — we publish that.              |
| OccWorld trajectory accuracy                                | DATA-GATED on a trained checkpoint;`predict()` carries `weights_trained=false` until one is loaded — never silently faked.                                                                                                                               |
| Edge-skill detection accuracy (seizure, weapon, affect, …) | UNVALIDATED — every such module is now disclaimer-gated as experimental/research; the DSP is real, the accuracy is not claimed.                                                                                                                              |
| 802.11bf-2025 OTA conformance                               | No commodity silicon ships a conformant interface as of 2026; ours is a simulation-tested forward-compat protocol model, not a certified implementation.                                                                                                      |

## Provenance

Every claim above traces to a committed ADR (`docs/adr/ADR-154`…`ADR-163`), a
test, a criterion bench, `benchmarks/wiflow-std/RESULTS.md`, or
`benchmarks/edge-latency/RESULTS.md`. The history
includes published **retractions** (the 92.9% PCK retraction; the WiFlow-STD
shipped-checkpoint refutation; the NV-diamond BOM reality check) — a faker hides
failures; we commit them.
