# Retrieval Benchmarks

Measures DAWN's memory retrieval quality against three published benchmarks,
using the same datasets and metrics that other memory systems report on.

## Architecture

A C binary (`bench_retrieval`) exercises DAWN's real embedding engine and
document search scoring. A Python script (`run_benchmark.py`) loads benchmark
datasets, drives the C binary via JSON-lines on stdin/stdout, and computes
metrics. The binary uses an in-memory SQLite database — it never touches
DAWN's production data.

## Build

```bash
cmake --preset debug && make -C build-debug bench_retrieval
```

## Datasets

### LongMemEval (500 questions)

Source: [xiaowu0162/longmemeval-cleaned](https://huggingface.co/datasets/xiaowu0162/longmemeval-cleaned) on HuggingFace.

```bash
mkdir -p ~/datasets/longmemeval
curl -fsSL -o ~/datasets/longmemeval/longmemeval_s_cleaned.json \
  https://huggingface.co/datasets/xiaowu0162/longmemeval-cleaned/resolve/main/longmemeval_s_cleaned.json
```

### LoCoMo (10 conversations, ~2000 QA pairs)

Source: [snap-research/locomo](https://github.com/snap-research/locomo) on GitHub.

```bash
git clone --depth 1 https://github.com/snap-research/locomo.git ~/datasets/locomo
```

### ConvoMem (75K+ QA pairs)

Source: [Salesforce/ConvoMem](https://huggingface.co/datasets/Salesforce/ConvoMem) on HuggingFace.

```bash
mkdir -p ~/datasets/convomem
curl -fsSL -o ~/datasets/convomem/user_evidence_sample.json \
  "https://huggingface.co/datasets/Salesforce/ConvoMem/resolve/main/core_benchmark/evidence_questions/user_evidence/1_evidence/0050e213-5032-42a0-8041-b5eef2f8ab91_Telemarketer.json"
```

For the full dataset (all 6 categories), use `huggingface-cli`:

```bash
huggingface-cli download Salesforce/ConvoMem --repo-type dataset --local-dir ~/datasets/convomem
```

## Current Baselines (May 2026)

These are the numbers to beat. Every implementation commit that touches the
retrieval pipeline should re-verify against all three benchmarks before merging.
Pipeline: `bge-small-en-v1.5-int8` bi-encoder + hybrid scoring (cosine + keyword
+ temporal-query proximity + proper-noun boost).

| Benchmark | Score | Config |
|---|---|---|
| **LongMemEval R@5** | **97.0%** | turn-level, official scoring, top-K=5 |
| **LongMemEval NDCG@5** | **92.5%** | turn-level, official scoring |
| **LoCoMo overall** | **81.6%** | session granularity (argparse default), top-K=10 |
| LoCoMo cat-1 (profile facts) | 69.3% | — |
| LoCoMo cat-2 (temporal) | 84.9% | — |
| LoCoMo cat-3 (inference) | 64.4% | — |
| **ConvoMem avg recall** | **99.0%** | 100 items |

**Granularity note for LoCoMo.** The argparse default is `--granularity session`
(one doc per session, all dialog turns concatenated). The standard run command
below produces the published 81.6% / 64.4% numbers at session granularity. At
true dialog granularity (`--granularity dialog`, one doc per dialog turn — closer
to production retrieval), the same pipeline scores **64.9% overall / 44.6% cat-3**.
The dialog-level number is what session-neighbor boost (below) targets and lifts.

**Top-K depth ceiling** (recall if all top-K positions are returned — informs
whether a wider candidate pool would help):

| top-K | LoCoMo overall | Cat-1 | Cat-3 |
|---|---|---|---|
| 10 | 81.6% | 69.3% | 64.4% |
| 15 | 89.9% | 82.8% | 77.8% |

## Running

> **Important — required flags.**  Always pass `--temporal-weight 0.20` and
> `--proper-noun-boost 1.0` for standard runs.  Omitting them gives the
> pre-S7 (April 2026) baseline and looks like a regression.  These signals
> are part of DAWN's hybrid scoring; the orchestrator defaults them to 0
> (disabled) so ablation runs are also possible without code changes.

### Standard LoCoMo run (~2 min)

```bash
python3 benchmarks/run_benchmark.py \
    --binary ./build-debug/tests/bench_retrieval \
    --benchmark locomo \
    --dataset ~/datasets/locomo/data/locomo10.json \
    --temporal-weight 0.20 \
    --proper-noun-boost 1.0
```

### Standard LongMemEval run (~95 min on Jetson Orin, turn granularity)

```bash
python3 benchmarks/run_benchmark.py \
    --binary ./build-debug/tests/bench_retrieval \
    --benchmark longmemeval \
    --dataset ~/datasets/longmemeval/longmemeval_s_cleaned.json \
    --granularity turn --turn-scoring official \
    --temporal-weight 0.20 \
    --proper-noun-boost 1.0
```

### Standard ConvoMem run (~2 min)

```bash
python3 benchmarks/run_benchmark.py \
    --binary ./build-debug/tests/bench_retrieval \
    --benchmark convomem \
    --dataset ~/datasets/convomem/user_evidence_sample.json \
    --limit 100 \
    --temporal-weight 0.20 \
    --proper-noun-boost 1.0
```

### Quick smoke test (subset of LongMemEval, ~4 min)

```bash
python3 benchmarks/run_benchmark.py \
    --binary ./build-debug/tests/bench_retrieval \
    --benchmark longmemeval \
    --dataset ~/datasets/longmemeval/longmemeval_s_cleaned.json \
    --granularity turn --turn-scoring official \
    --temporal-weight 0.20 --proper-noun-boost 1.0 \
    --limit 50
```

## Experiments

### Test a different ONNX model without recompiling

`bench_retrieval` honors `DAWN_ONNX_MODEL` for the embedding provider's model
path. Useful for A/B-testing model swaps before changing `MODEL_PATH` in
`src/memory/memory_embed_onnx.c`.

```bash
DAWN_ONNX_MODEL=models/embeddings/bge-base-en-v1.5.onnx \
python3 benchmarks/run_benchmark.py \
    --binary ./build-debug/tests/bench_retrieval \
    --benchmark locomo \
    --dataset ~/datasets/locomo/data/locomo10.json \
    --temporal-weight 0.20 --proper-noun-boost 1.0
```

### Session-neighbor boost (LoCoMo dialog only)

Mitigates session-fragmentation in LoCoMo dialog-granularity runs: top-N items
by cosine become session anchors (split `dia_id` on `':'`), and chunks sharing
an anchor's session prefix get an additive boost before keyword re-ranking.
Off by default. No-op for any dataset whose doc IDs don't contain `':'`
(LoCoMo session, ConvoMem, LongMemEval all reproduce baseline to four
decimal places when these flags are set).

Validated settings (May 2026 sweep): `window=3, boost=0.03` →
LoCoMo dialog overall **+3.0pp** (cat-3 +2.5, cat-4 +3.6, cat-5 +5.5,
cat-1 -0.6 within noise on 282 questions).

```bash
python3 benchmarks/run_benchmark.py \
    --binary ./build-debug/tests/bench_retrieval \
    --benchmark locomo \
    --dataset ~/datasets/locomo/data/locomo10.json \
    --granularity dialog \
    --temporal-weight 0.20 --proper-noun-boost 1.0 \
    --session-neighbor-window 3 --session-neighbor-boost 0.03
```

### Top-K depth experiment

Diagnoses whether the right evidence is being retrieved at all (just buried
deeper in the candidate list). Confirms whether a wider candidate pool or
reranker has headroom to help on this dataset.

```bash
python3 benchmarks/run_benchmark.py \
    --binary ./build-debug/tests/bench_retrieval \
    --benchmark locomo \
    --dataset ~/datasets/locomo/data/locomo10.json \
    --temporal-weight 0.20 --proper-noun-boost 1.0 \
    --top-k 15
```

### Temporal-weight sweep

Re-tune the temporal boost when the embedding model changes. The current 0.20
default was calibrated on bge-small-en-v1.5-int8.

```bash
for tw in 0.15 0.20 0.25 0.30; do
    echo "=== temporal-weight $tw ===" && \
    python3 benchmarks/run_benchmark.py \
        --binary ./build-debug/tests/bench_retrieval \
        --benchmark locomo \
        --dataset ~/datasets/locomo/data/locomo10.json \
        --temporal-weight $tw --proper-noun-boost 1.0
done
```

### Raw cosine baseline (no hybrid scoring)

For apples-to-apples comparison with published bi-encoder-only systems, use
`--raw` to disable keyword boosting (and omit the temporal/proper-noun flags).

```bash
python3 benchmarks/run_benchmark.py \
    --binary ./build-debug/tests/bench_retrieval \
    --benchmark longmemeval \
    --dataset ~/datasets/longmemeval/longmemeval_s_cleaned.json \
    --raw
```

### Alternate provider (Ollama)

The default provider is ONNX (`bge-small-en-v1.5-int8`, 384 dims). For
experimenting with other models on a separate Ollama instance:

```bash
ollama pull bge-small
python3 benchmarks/run_benchmark.py \
    --binary ./build-debug/tests/bench_retrieval \
    --benchmark longmemeval \
    --dataset ~/datasets/longmemeval/longmemeval_s_cleaned.json \
    --provider ollama --model bge-small --endpoint http://localhost:11434
```

### Save results to JSON

```bash
python3 benchmarks/run_benchmark.py \
    --binary ./build-debug/tests/bench_retrieval \
    --benchmark longmemeval \
    --dataset ~/datasets/longmemeval/longmemeval_s_cleaned.json \
    --granularity turn --turn-scoring official \
    --temporal-weight 0.20 --proper-noun-boost 1.0 \
    --output results.json
```

## After Running: What to Update

When results change meaningfully (>0.5 pp on any benchmark):

1. **Atlas design doc** — update the results tables in
   `~/code/The-OASIS-Project/atlas/dawn/archive/MEMORY_SYSTEM_DESIGN.md` §14.3:
   - Session-level results table
   - Turn-level results table
   - Comparison-to-published-baselines table (update the DAWN row and SOTA margin)
   - LoCoMo category breakdown table
   - Analysis bullet points
2. **`docs/TODO.md`** — update the shipped-section entry that tracks the
   latest retrieval baseline numbers.
3. **This file** — update the "Current Baselines" table above.

Atlas is the primary record; the table here is a fast-reference copy that
should match.

## Granularity (LongMemEval-only)

Session-level (~48 docs per question, top-K=10) is faster but easier.
Turn-level (~273 docs per question, top-K=5) is what academic papers like RMM
(Tan et al., ACL 2025) use. The standard run uses turn-level.

Two scoring modes for turn-level:

- `official` (default): any user turn from the answer session counts as
  correct.  Matches the official LongMemEval evaluation code.
- `strict`: only turns annotated with `has_answer=true` count.  Harder
  criterion (~1.7 targets per question vs ~11 for official).

```bash
python3 benchmarks/run_benchmark.py \
    --binary ./build-debug/tests/bench_retrieval \
    --benchmark longmemeval \
    --dataset ~/datasets/longmemeval/longmemeval_s_cleaned.json \
    --granularity turn --turn-scoring strict \
    --temporal-weight 0.20 --proper-noun-boost 1.0
```

## Flags Quick Reference

| Flag | Values | Notes |
|---|---|---|
| `--temporal-weight` | 0.0–1.0 | **Always 0.20 for standard runs**; 0.0 omits temporal boost (ablation) |
| `--proper-noun-boost` | 0.0–2.0 | **Always 1.0 for standard runs**; 0.0 for ablation |
| `--granularity` | `session`, `turn`, `dialog` | Argparse default `session`. Standard LoCoMo runs use the default (session); LME standard runs pass `turn`. Pass `dialog` for production-aligned LoCoMo retrieval. |
| `--turn-scoring` | `official`, `strict` | LME only; `official` for published comparisons |
| `--top-k` | int | Default 10; use 15–20 for depth experiments |
| `--sentence-chunks` | flag | LoCoMo only; tested May 2026 — zero effect, don't use |
| `--session-neighbor-window` | int | Top-N items by cosine become session anchors (split doc id on `':'`). 0 = off (default). LoCoMo dialog only. |
| `--session-neighbor-boost` | 0.0–0.20 | Additive score for chunks matching an anchor's session prefix. Validated 0.03 with `window=3`. |
| `--raw` | flag | Disables keyword boost; pure cosine baseline only |
| `--limit` | int | Cap dataset size for quick smoke tests |
| `--provider` | `onnx`, `ollama`, `openai` | Default `onnx` |
| `--model` | str | Override provider's model name |
| `--endpoint` | url | Required for `ollama`/`openai` providers |
| `--output` | path | Save full results JSON |

Environment variables:

| Var | Notes |
|---|---|
| `DAWN_ONNX_MODEL` | Override the ONNX model file path without recompiling |

## Metrics

| Benchmark | Metric | Description |
|---|---|---|
| LongMemEval | Recall@K | Did the correct session/turn appear in the top K results? |
| LongMemEval | NDCG@K | Position-weighted relevance (higher = correct answer ranked earlier) |
| LoCoMo | Avg Recall | Fraction of evidence dialog IDs found in top 10, averaged across QA pairs |
| ConvoMem | Avg Recall | Fraction of evidence messages found via substring match in top 10 |

LoCoMo is also broken down by question category (1–5: profile facts,
temporal, inference, single-hop, adversarial) — see the per-category numbers
in the baseline table above.
