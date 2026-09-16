# WiFlow v1 — WiFi CSI Pose Estimation Model

Camera-supervised WiFlow pose model trained on real ESP32 CSI + MediaPipe ground truth.

## Metrics
- **PCK@20: 92.9%** (17 COCO keypoints)
- Eval loss: 0.082
- Bone constraint loss: 0.008
- Parameters: 186,946 (974 KB)

## Architecture
- TCN: 2 dilated causal conv blocks (k=3, d=[1,2])
- Input: 35 subcarriers x 20 time steps (ruvector-solver reduced from 70)
- Output: 17 COCO keypoints [x, y] in [0, 1]
- Scale: lite (189K params)

## Training
- 345 paired samples (5 min capture)
- ESP32-S3 CSI: 7,000 frames at 23fps
- Mac camera: 6,470 frames at 22fps via MediaPipe PoseLandmarker
- 50 epochs (33 supervised + 17 refinement)
- ruvector optimizations: O6 subcarrier selection, O7 attention, O8 Stoer-Wagner min-cut, O9 multi-SPSA

## Usage
```javascript
const model = JSON.parse(fs.readFileSync('wiflow-v1.json'));
// Load into WiFlowSupervisedModel and call forward(csiInput)
```

## License
Apache-2.0
