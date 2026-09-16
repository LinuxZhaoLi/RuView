---
license: mit
tags:
  - wifi-sensing
  - vital-signs
  - presence-detection
  - pose-estimation
  - edge-ai
  - esp32
  - self-supervised
  - camera-supervised
  - cognitum
  - through-wall
  - privacy-preserving
  - spiking-neural-network
  - ruvector
  - wiflow
language:
  - en
library_name: onnxruntime
pipeline_tag: other
---

# RuView — WiFi Sensing Models

**Turn WiFi signals into spatial intelligence.** Detect people, measure breathing and heart rate, estimate pose, track movement, and monitor rooms — through walls, in the dark, with no cameras. Just radio physics.

## What This Does

WiFi signals bounce off people. When someone breathes, their chest moves the air, which subtly changes the WiFi signal. When they walk, the changes are bigger. These models learned to read those changes from a $9 ESP32 chip.

| What it senses | How well | Without |
|----------------|----------|---------|
| **Pose estimation** | **92.9% PCK@20** (17 COCO keypoints) | No camera at deployment |
| **Is someone there?** | 100% accuracy | No camera needed |
| **Are they moving?** | Detects typing vs walking vs standing | No wearable needed |
| **Breathing rate** | 6-30 BPM, contactless | No chest strap |
| **Heart rate** | 40-120 BPM, through clothes | No smartwatch |
| **How many people?** | 1-4, via subcarrier graph analysis | No headcount camera |
| **Through walls** | Works through drywall, wood, fabric | No line of sight |
| **Sleep quality** | Deep/Light/REM/Awake classification | No mattress sensor |
| **Fall detection** | <2 second alert | No pendant |

## Benchmarks

| Metric | Result | Context |
|--------|--------|---------|
| **Pose PCK@20** | **92.9%** | Camera-supervised WiFlow model (v0.7.0) |
| **Presence accuracy** | **100%** | Never misses, never false alarms |
| **Inference speed** | **0.008 ms** | 125,000x faster than real-time |
| **Throughput** | **164,183 emb/sec** | One laptop handles 1,600+ sensors |
| **Model size (pose)** | **974 KB** | WiFlow lite, 189K params |
| **Model size (sensing)** | **8 KB** (4-bit quantized) | Fits in ESP32 SRAM |
| **Training time (pose)** | **19 minutes** | 5-min data collection + lite training |
| **Training time (sensing)** | **12 minutes** | On Mac Mini M4 Pro, no GPU needed |
| **Hardware cost** | **$9** | Single ESP32-S3 |

## Models in This Repo

### WiFlow Pose Model (v0.7.0) — NEW

| File | Size | Description |
|------|------|-------------|
| `wiflow-v1/wiflow-v1.json` | 974 KB | **Camera-supervised pose model** — 92.9% PCK@20, 17 COCO keypoints |
| `wiflow-v1/training-log.json` | 13 KB | Loss curves per training phase |
| `wiflow-v1/baseline-report.json` | 1 KB | Pre-training baseline metrics |
| `wiflow-v1/MODEL_CARD.md` | 1 KB | Model documentation |

Trained on real ESP32 CSI (7,000 frames) + real webcam keypoints via MediaPipe (6,470 frames). 5-minute data collection, 19-minute training. See [ADR-079](https://github.com/ruvnet/RuView/blob/main/docs/adr/ADR-079-camera-ground-truth-training.md).

### Contrastive Sensing Model (v0.6.0)

| File | Size | Description |
|------|------|-------------|
| `model.safetensors` | 48 KB | Full contrastive encoder (128-dim embeddings) |
| `model-q4.bin` | 8 KB | **Recommended** — 4-bit quantized, 8x compression |
| `model-q2.bin` | 4 KB | Ultra-compact for ESP32 edge inference |
| `model-q8.bin` | 16 KB | High quality 8-bit |
| `presence-head.json` | 2.6 KB | Presence detection head (100% accuracy) |
| `node-1.json` | 21 KB | LoRA adapter for room/node 1 |
| `node-2.json` | 21 KB | LoRA adapter for room/node 2 |
| `config.json` | 586 B | Model configuration |

## Quick Start

```bash
# Download all models
pip install huggingface_hub
huggingface-cli download ruv/ruview --local-dir models/

# Use with RuView sensing pipeline
git clone https://github.com/ruvnet/RuView.git
cd RuView

# Flash an ESP32-S3 ($9 on Amazon/AliExpress)
python -m esptool --chip esp32s3 --port COM9 --baud 460800 \
  write_flash 0x0 bootloader.bin 0x8000 partition-table.bin \
  0xf000 ota_data_initial.bin 0x20000 esp32-csi-node.bin

# Train your own pose model (5 min data + 19 min training)
python scripts/collect-ground-truth.py --duration 300 --preview
python scripts/record-csi-udp.py --duration 300
node scripts/align-ground-truth.js --gt data/ground-truth/*.jsonl --csi data/recordings/*.csi.jsonl
node scripts/train-wiflow-supervised.js --data data/paired/*.jsonl --scale lite
```

## Architecture

### WiFlow Pose Model
```
CSI amplitude [35, 20] (ruvector-selected subcarriers)
    |
TCN: 2 dilated causal conv blocks (k=3, d=[1,2])
    35 -> 32 -> 32 channels
    |
Flatten [640] -> FC [256] -> FC [34] -> Sigmoid
    |
17 COCO keypoints [x, y] in [0, 1]
```

### Contrastive Sensing Model
```
WiFi signals -> ESP32-S3 ($9) -> 8-dim features @ 1 Hz -> Encoder -> 128-dim embedding
    |
Presence head (threshold 0.3) -> person/no-person
Activity head -> stationary/walking/typing
Vitals extraction -> breathing BPM, heart rate BPM
```

## Training Your Own Pose Model

The camera is only needed during a one-time 5-minute training session. After that, the model runs on CSI alone — no camera at deployment.

1. **Collect** — Run `collect-ground-truth.py` + `record-csi-udp.py` simultaneously for 5 minutes
2. **Align** — `align-ground-truth.js` pairs camera keypoints with CSI windows by timestamp
3. **Train** — `train-wiflow-supervised.js` trains WiFlow with curriculum learning + bone constraints
4. **Deploy** — Load `wiflow-v1.json` and run inference on CSI only

See the [v0.7.0 release](https://github.com/ruvnet/RuView/releases/tag/v0.7.0) for details.

## License

MIT — see [LICENSE](https://github.com/ruvnet/RuView/blob/main/LICENSE)

## Links

- [GitHub](https://github.com/ruvnet/RuView)
- [v0.7.0 Release](https://github.com/ruvnet/RuView/releases/tag/v0.7.0)
- [ADR-079: Camera Ground-Truth Training](https://github.com/ruvnet/RuView/blob/main/docs/adr/ADR-079-camera-ground-truth-training.md)
- [Cognitum.one](https://cognitum.one)
