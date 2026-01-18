# WiFi Sonar Next Steps Roadmap

## Phase 1 – Fall Detection Hardening (Week 1–2)
- Collect 150+ fall vs. walk sequences per subject; enforce 120 Hz per link for balance (Thesis p. 205).
- Augment presence detector with two-layer TinyML head (PCA 10-D → 32 → 1 sigmoid) exported via `torch.quantization`.
- Deploy quantized weights to FeatherS3 for on-edge alerting (optional).

## Phase 2 – Welding vs. Grinding Classification (Week 3)
- Label grinder vs. welder segments using synchronized GoPro footage for ground truth (PerCom 2023 Sec. 5.1).
- Train multi-class classifier on PCA features; evaluate confusion matrix, target >90% F1.
- Investigate spectral fingerprints by feeding PCA outputs into 1-D CNN only after baseline DNN is stable.

## Phase 3 – Breathing & Vital Signs (Week 4)
- Collect supine, prone, and seated breathing datasets (60 s each) with spirometer reference.
- Apply band-pass (0.1–0.5 Hz) on PCA component 1 to estimate respiration rate; validate ±1 BPM error.
- Explore phase synchronization upgrade (dual-antenna FeatherS3) once amplitude pipeline saturates (Thesis p. 233).

## Tooling & Ops
- Automate dataset versioning with DVC or Git LFS to keep CSV growth manageable.
- Add pytest-based regression tests for collector parsing + PCA transforms.
- Integrate CI job that lints firmware (`idf.py lint`) and Python (`ruff`, `pytest`) before flashing.

