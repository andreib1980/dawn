#!/usr/bin/env python3
"""Rerank shootout: test multiple cross-encoder models on LoCoMo conv 1.

Standalone Python script — does NOT touch the C bench.  Steps:
  1. Load LoCoMo conv 1 (chunks + queries + ground truth)
  2. Embed chunks with bge-small (Python ONNX CPU) — same model as production
  3. Per query: cosine sweep over chunks → top-50 candidates
  4. For each candidate cross-encoder model: rerank top-50, take top-10
  5. Report recall@10 vs ground truth

Cheap test: validates whether ANY cross-encoder model lifts LoCoMo before
investing in C-side tokenizer ports for non-WordPiece tokenizers.
"""
import json
import sys
import time
from pathlib import Path

import numpy as np
import onnxruntime as ort
from tokenizers import Tokenizer

ROOT = Path(__file__).resolve().parent.parent
MODELS = ROOT / "models" / "embeddings"

BI_ENCODER_MODEL = MODELS / "bge-small-en-v1.5-int8.onnx"
BI_ENCODER_VOCAB = MODELS / "vocab.txt"
DATASET = Path.home() / "datasets" / "locomo" / "data" / "locomo10.json"

# All cross-encoders to test.  (display_name, model_path, tokenizer_path)
# tokenizer_path can be a vocab.txt (BERT WordPiece) or tokenizer.json (HF).
RERANKERS = [
    # Already tested in prior run: ms-marco-L-6 (+8.9pp), ms-marco-L-12 (+10.4pp).
    # Remaining: jina-tiny (BPE, no token_type_ids), mxbai-base (SentencePiece).
    ("jina-reranker-v1-tiny-en", MODELS / "jina-reranker-v1-tiny-en-int8.onnx",
     MODELS / "jina-tokenizer.json"),
    ("mxbai-rerank-base-v1", MODELS / "mxbai-rerank-base-v1-quantized.onnx",
     MODELS / "mxbai-tokenizer.json"),
]

TOP_K_RERANK = 10
POOL_SIZE = 50


# ───────────────────────── BERT WordPiece tokenizer (matches our C impl) ──────

class WordPieceTokenizer:
    """Minimal BERT WordPiece — mirrors src/memory/memory_embed_tokenizer.c."""

    CLS_ID = 101
    SEP_ID = 102
    UNK_ID = 100

    def __init__(self, vocab_path: Path):
        self.vocab = {}
        with open(vocab_path) as f:
            for i, line in enumerate(f):
                self.vocab[line.strip()] = i

    def encode(self, text: str, max_len: int) -> dict:
        ids = [self.CLS_ID]
        for word in text.lower().split():
            offset = 0
            first = True
            while offset < len(word):
                best_len, best_id = 0, self.UNK_ID
                for try_len in range(len(word) - offset, 0, -1):
                    piece = word[offset:offset + try_len]
                    if not first:
                        piece = "##" + piece
                    if piece in self.vocab:
                        best_len, best_id = try_len, self.vocab[piece]
                        break
                if best_len == 0:
                    ids.append(self.UNK_ID)
                    break
                ids.append(best_id)
                offset += best_len
                first = False
                if len(ids) >= max_len - 1:
                    break
            if len(ids) >= max_len - 1:
                break
        ids.append(self.SEP_ID)
        return {"input_ids": ids[:max_len], "type_ids": [0] * len(ids[:max_len])}

    def encode_pair(self, query: str, passage: str, max_len: int) -> dict:
        q = self.encode(query, max_len // 2 + 2)
        # passage tokens, type_id=1
        p_text = passage.lower()
        ids = list(q["input_ids"][:-1])  # drop trailing SEP from query
        ids.append(self.SEP_ID)
        types = [0] * len(ids)
        for word in p_text.split():
            offset = 0
            first = True
            while offset < len(word) and len(ids) < max_len - 1:
                best_len, best_id = 0, self.UNK_ID
                for try_len in range(len(word) - offset, 0, -1):
                    piece = word[offset:offset + try_len]
                    if not first:
                        piece = "##" + piece
                    if piece in self.vocab:
                        best_len, best_id = try_len, self.vocab[piece]
                        break
                if best_len == 0:
                    ids.append(self.UNK_ID)
                    types.append(1)
                    break
                ids.append(best_id)
                types.append(1)
                offset += best_len
                first = False
        ids.append(self.SEP_ID)
        types.append(1)
        return {"input_ids": ids[:max_len], "type_ids": types[:max_len]}


def hf_tokenize_pair(tok: Tokenizer, query: str, passage: str, max_len: int) -> dict:
    """Use HuggingFace tokenizers for BPE/SentencePiece-based models."""
    tok.enable_truncation(max_length=max_len)
    enc = tok.encode(query, passage)
    return {
        "input_ids": enc.ids,
        "type_ids": enc.type_ids,
        "attention_mask": enc.attention_mask,
    }


# ───────────────────────── bi-encoder (bge-small) ──────────────────────────────

def mean_pool_l2(last_hidden: np.ndarray, mask: np.ndarray) -> np.ndarray:
    """Mean-pool over the seq_len axis with attention mask, then L2-normalize."""
    masked = last_hidden * mask[..., None]
    summed = masked.sum(axis=1)
    counts = mask.sum(axis=1, keepdims=True).clip(min=1)
    pooled = summed / counts
    norms = np.linalg.norm(pooled, axis=1, keepdims=True).clip(min=1e-9)
    return pooled / norms


def biencoder_embed(sess, wp_tok, texts: list[str], max_len: int = 256) -> np.ndarray:
    """Embed a list of texts; returns (n, dims) L2-normalized matrix."""
    out = []
    for text in texts:
        enc = wp_tok.encode(text, max_len)
        ids = np.array([enc["input_ids"]], dtype=np.int64)
        types = np.array([enc["type_ids"]], dtype=np.int64)
        mask = np.ones_like(ids)
        result = sess.run(None, {
            "input_ids": ids,
            "attention_mask": mask,
            "token_type_ids": types,
        })
        last_hidden = result[0]
        pooled = mean_pool_l2(last_hidden, mask.astype(np.float32))[0]
        out.append(pooled)
    return np.stack(out)


# ───────────────────────── cross-encoder rerank ────────────────────────────────

def rerank_score(sess, tokenizer, kind: str, query: str, passages: list[str],
                 max_len: int = 256) -> np.ndarray:
    """Score (query, passage) pairs with the cross-encoder; returns logits."""
    input_names = {i.name for i in sess.get_inputs()}
    scores = []
    for p in passages:
        if kind == "wordpiece":
            enc = tokenizer.encode_pair(query, p, max_len)
            attn = [1] * len(enc["input_ids"])
        else:
            enc = hf_tokenize_pair(tokenizer, query, p, max_len)
            attn = enc.get("attention_mask", [1] * len(enc["input_ids"]))
        ids = np.array([enc["input_ids"]], dtype=np.int64)
        types = np.array([enc["type_ids"]], dtype=np.int64)
        mask = np.array([attn], dtype=np.int64)
        feed = {"input_ids": ids}
        if "attention_mask" in input_names:
            feed["attention_mask"] = mask
        if "token_type_ids" in input_names:
            feed["token_type_ids"] = types
        out = sess.run(None, feed)[0]
        scores.append(float(out.flatten()[0]))
    return np.array(scores, dtype=np.float32)


# ───────────────────────── LoCoMo loading ──────────────────────────────────────

def load_locomo_conv0(path: Path):
    """Return (chunks, queries) for LoCoMo conversation 1.

    Schema (per inspecting locomo10.json):
      data[i] = {qa: [...], conversation: {session_1: [{dia_id, speaker, text}, ...],
                                           session_1_date_time: "...", ...}, ...}

    chunks: list of (dia_id, text)
    queries: list of (question, evidence_ids, category)
    """
    with open(path) as f:
        data = json.load(f)
    entry = data[0]
    conv = entry.get("conversation", entry)

    chunks = []
    n = 1
    while True:
        key = f"session_{n}"
        if key not in conv:
            break
        for d in conv[key]:
            dia_id = d.get("dia_id", "?")
            speaker = d.get("speaker", "?")
            text = d.get("text", "")
            chunks.append((dia_id, f'{speaker} said, "{text}"'))
        n += 1

    queries = []
    for q in entry.get("qa", entry.get("QA", [])):
        question = q.get("question", "")
        evidence = q.get("evidence", [])
        category = str(q.get("category", "?"))
        if not question or not evidence:
            continue
        ev_ids = []
        for e in evidence:
            if isinstance(e, str):
                ev_ids.append(e)
            elif isinstance(e, dict):
                ev_ids.append(e.get("dia_id") or e.get("id") or str(e))
        queries.append((question, ev_ids, category))
    return chunks, queries


# ───────────────────────── main evaluation ─────────────────────────────────────

def main():
    print(f"Loading LoCoMo conv 1 from {DATASET}", file=sys.stderr)
    chunks, queries = load_locomo_conv0(DATASET)
    print(f"  {len(chunks)} chunks, {len(queries)} queries", file=sys.stderr)

    # Bi-encoder pass: embed chunks + queries
    print(f"\nBi-encoder: bge-small int8 (CPU)", file=sys.stderr)
    bi_sess = ort.InferenceSession(str(BI_ENCODER_MODEL),
                                   providers=["CPUExecutionProvider"])
    wp_tok = WordPieceTokenizer(BI_ENCODER_VOCAB)
    t0 = time.time()
    chunk_embs = biencoder_embed(bi_sess, wp_tok, [t for _, t in chunks])
    print(f"  embedded {len(chunks)} chunks in {time.time()-t0:.1f}s", file=sys.stderr)
    t0 = time.time()
    query_embs = biencoder_embed(bi_sess, wp_tok, [q for q, _, _ in queries])
    print(f"  embedded {len(queries)} queries in {time.time()-t0:.1f}s", file=sys.stderr)

    # Cosine + top-50 candidates per query
    print(f"\nFirst-stage retrieval: top-{POOL_SIZE} candidates per query", file=sys.stderr)
    sims = query_embs @ chunk_embs.T  # already L2-normalized
    pools = np.argsort(-sims, axis=1)[:, :POOL_SIZE]

    # Baseline (bi-encoder only) recall@10
    def recall_at_k(retrieved_ids: list[str], evidence_ids: list[str], k: int) -> float:
        hits = sum(1 for e in evidence_ids if e in retrieved_ids[:k])
        return hits / max(len(evidence_ids), 1)

    chunk_ids = [c[0] for c in chunks]
    baseline_recalls = []
    baseline_per_cat = {}
    for i, (q, evidence, cat) in enumerate(queries):
        retrieved = [chunk_ids[idx] for idx in pools[i, :TOP_K_RERANK]]
        r = recall_at_k(retrieved, evidence, TOP_K_RERANK)
        baseline_recalls.append(r)
        baseline_per_cat.setdefault(cat, []).append(r)

    print(f"\n{'='*70}")
    print(f"BASELINE (bi-encoder only, top-{TOP_K_RERANK}): "
          f"recall@{TOP_K_RERANK} = {np.mean(baseline_recalls):.3f}")
    for cat in sorted(baseline_per_cat):
        print(f"  cat {cat}: {np.mean(baseline_per_cat[cat]):.3f}  "
              f"(n={len(baseline_per_cat[cat])})")
    print(f"{'='*70}\n")

    # Run each candidate reranker
    for name, model_path, tok_path in RERANKERS:
        if not model_path.exists():
            print(f"SKIP {name}: model not found at {model_path}", file=sys.stderr)
            continue
        print(f"--- {name} ---", file=sys.stderr)
        sess = ort.InferenceSession(str(model_path), providers=["CPUExecutionProvider"])
        if tok_path.suffix == ".txt":
            tok = WordPieceTokenizer(tok_path)
            kind = "wordpiece"
        else:
            tok = Tokenizer.from_file(str(tok_path))
            kind = "hf"

        recalls = []
        per_cat = {}
        t0 = time.time()
        for i, (q, evidence, cat) in enumerate(queries):
            cand_ids = [chunk_ids[idx] for idx in pools[i]]
            cand_texts = [chunks[idx][1] for idx in pools[i]]
            scores = rerank_score(sess, tok, kind, q, cand_texts)
            order = np.argsort(-scores)
            retrieved = [cand_ids[j] for j in order[:TOP_K_RERANK]]
            r = recall_at_k(retrieved, evidence, TOP_K_RERANK)
            recalls.append(r)
            per_cat.setdefault(cat, []).append(r)
            if (i + 1) % 25 == 0:
                print(f"  {i+1}/{len(queries)}  recall@{TOP_K_RERANK}={np.mean(recalls):.3f}  "
                      f"({(time.time()-t0)/(i+1)*1000:.0f}ms/q)", file=sys.stderr)
        elapsed = time.time() - t0

        print(f"\n=== {name} ===")
        print(f"  recall@{TOP_K_RERANK}:  {np.mean(recalls):.3f}  "
              f"(baseline {np.mean(baseline_recalls):.3f}, "
              f"Δ {np.mean(recalls)-np.mean(baseline_recalls):+.3f})")
        for cat in sorted(per_cat):
            base = np.mean(baseline_per_cat[cat])
            cur = np.mean(per_cat[cat])
            print(f"  cat {cat}: {cur:.3f}  (baseline {base:.3f}, Δ {cur-base:+.3f})")
        print(f"  time: {elapsed:.1f}s ({elapsed/len(queries)*1000:.0f}ms/query)")
        print()


if __name__ == "__main__":
    main()
