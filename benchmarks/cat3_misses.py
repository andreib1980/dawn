#!/usr/bin/env python3
"""
LoCoMo cat-3 (multi-hop inference) failure-mode profiler.

Runs only the cat-3 questions from LoCoMo through bench_retrieval at deep
top-K so we can see where evidence actually lands.  For each missed
evidence dialog ID the harness records its rank in the candidate list,
the score, the retrieved top-10 with text, and the gold-evidence text —
enough to manually classify each miss as:

  - retrieval failure   (evidence rank > 50: cosine signal too weak)
  - ranking failure     (evidence rank 11..50: hybrid had it, ranked
                         below noise)
  - reasoning gap       (evidence found in top-10 but the question
                         needs multi-hop inference the LLM has to do)

Run:
   python3 benchmarks/cat3_misses.py \\
       --binary ./build-debug/tests/bench_retrieval \\
       --dataset ~/datasets/locomo/data/locomo10.json \\
       --output cat3_misses.json
"""

import argparse
import json
import sys
import time
from pathlib import Path

# Reuse the subprocess wrapper + ingest helpers from the main runner.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_benchmark import BenchRetrieval, parse_locomo_session_date


# Match run_locomo()'s default settings so misses reflect the standard config.
TEMPORAL_WEIGHT = 0.20
PROPER_NOUN_BOOST = 1.0
DEEP_TOP_K = 1000  # Larger than any conv's dialog pool.
EVAL_TOP_K = 10    # Where the benchmark draws the line.
RANK_BUCKETS = [(1, 10, "top-10 (recall hit)"),
                (11, 50, "ranking failure (hybrid pool)"),
                (51, DEEP_TOP_K, "retrieval failure (cosine signal weak)")]


def bucket_for_rank(rank):
   if rank is None:
      return "absent (outside deep pool)"
   for lo, hi, label in RANK_BUCKETS:
      if lo <= rank <= hi:
         return label
   return "absent (outside deep pool)"


def collect_dialog_index(conv):
   """Map dia_id -> {session_num, session_date, speaker, text}."""
   out = {}
   n = 1
   while True:
      key = f"session_{n}"
      if key not in conv:
         break
      date = conv.get(f"session_{n}_date_time", "")
      for d in conv[key]:
         dia_id = d.get("dia_id")
         if dia_id is None:
            continue
         out[dia_id] = {
            "session_num": n,
            "session_date": date,
            "speaker": d.get("speaker", "?"),
            "text": d.get("text", ""),
         }
      n += 1
   return out


def ingest_conv(engine, conv):
   """Ingest matching run_benchmark.run_locomo (dialog granularity, default
   formatting, with session timestamps)."""
   n = 1
   while True:
      key = f"session_{n}"
      if key not in conv:
         break
      ts = parse_locomo_session_date(conv.get(f"session_{n}_date_time", ""))
      for d in conv[key]:
         dia_id = d.get("dia_id", f"D{n}:?")
         speaker = d.get("speaker", "?")
         text = d.get("text", "")
         engine.add(dia_id, f'{speaker} said, "{text}"', created_at=ts)
      n += 1


def main():
   ap = argparse.ArgumentParser()
   ap.add_argument("--binary", required=True)
   ap.add_argument("--dataset", required=True)
   ap.add_argument("--output", default="cat3_misses.json")
   args = ap.parse_args()

   data = json.load(open(args.dataset))

   print(f"  Starting bench_retrieval (temporal={TEMPORAL_WEIGHT}, "
         f"pn-boost={PROPER_NOUN_BOOST}, deep top-K={DEEP_TOP_K})", file=sys.stderr)
   engine = BenchRetrieval(
      args.binary,
      provider="onnx",
      temporal_weight=TEMPORAL_WEIGHT,
      proper_noun_boost=PROPER_NOUN_BOOST,
   )
   print(f"  Ready: {engine.dims} dims, mode={engine.mode}", file=sys.stderr)

   misses = []          # one entry per cat-3 QA with recall < 1.0
   bucket_counts = {}   # per-evidence-piece tally
   total_evidence = 0
   missed_evidence = 0
   total_qa_cat3 = 0
   missed_qa_cat3 = 0

   t0 = time.time()
   for conv_idx, entry in enumerate(data):
      conv = entry.get("conversation", entry)
      cat3_qas = [q for q in entry.get("qa", [])
                  if str(q.get("category")) == "3" and q.get("evidence")]
      if not cat3_qas:
         continue

      dialog_index = collect_dialog_index(conv)
      engine.reset()
      ingest_conv(engine, conv)

      for qa in cat3_qas:
         total_qa_cat3 += 1
         question = qa.get("question", "")
         gold_answer = qa.get("answer", "")
         evidence_ids = qa.get("evidence", [])

         # Deep retrieval — get rank of every evidence ID
         result = engine.query(question, top_k=DEEP_TOP_K)
         retrieved = result.get("results", [])
         id_to_rank = {r["id"]: r["rank"] for r in retrieved}
         id_to_score = {r["id"]: r["score"] for r in retrieved}

         per_evidence = []
         any_missed = False
         for ev_id in evidence_ids:
            total_evidence += 1
            rank = id_to_rank.get(ev_id)
            score = id_to_score.get(ev_id)
            bucket = bucket_for_rank(rank)
            bucket_counts[bucket] = bucket_counts.get(bucket, 0) + 1
            ev_info = dialog_index.get(ev_id, {})
            in_top_eval = (rank is not None and rank <= EVAL_TOP_K)
            if not in_top_eval:
               missed_evidence += 1
               any_missed = True
            per_evidence.append({
               "evidence_id": ev_id,
               "rank": rank,
               "score": score,
               "bucket": bucket,
               "session": ev_info.get("session_num"),
               "session_date": ev_info.get("session_date"),
               "speaker": ev_info.get("speaker"),
               "text": ev_info.get("text"),
            })

         if not any_missed:
            continue

         missed_qa_cat3 += 1

         # Top-10 retrieved (with text) — what beat the evidence
         top10 = []
         for r in retrieved[:EVAL_TOP_K]:
            info = dialog_index.get(r["id"], {})
            top10.append({
               "rank": r["rank"],
               "id": r["id"],
               "score": r["score"],
               "speaker": info.get("speaker"),
               "session": info.get("session_num"),
               "text": info.get("text"),
            })

         misses.append({
            "conv_idx": conv_idx,
            "sample_id": entry.get("sample_id"),
            "question": question,
            "gold_answer": gold_answer,
            "evidence": per_evidence,
            "top10_retrieved": top10,
         })

      print(f"  [conv {conv_idx + 1}/{len(data)}] cat-3 QAs={len(cat3_qas)}  "
            f"misses so far={missed_qa_cat3}", file=sys.stderr)

   elapsed = time.time() - t0
   engine.quit()

   summary = {
      "config": {
         "temporal_weight": TEMPORAL_WEIGHT,
         "proper_noun_boost": PROPER_NOUN_BOOST,
         "deep_top_k": DEEP_TOP_K,
         "eval_top_k": EVAL_TOP_K,
      },
      "totals": {
         "total_cat3_qa": total_qa_cat3,
         "qa_with_any_miss": missed_qa_cat3,
         "total_evidence_pieces": total_evidence,
         "missed_evidence_pieces": missed_evidence,
         "fraction_recall_avg": (total_evidence - missed_evidence) / total_evidence
                                if total_evidence else 0.0,
      },
      "evidence_rank_buckets": bucket_counts,
      "elapsed_seconds": elapsed,
      "misses": misses,
   }

   with open(args.output, "w") as f:
      json.dump(summary, f, indent=2)

   print(f"\n  Wrote {args.output} ({len(misses)} cat-3 QAs with at least one missed evidence)",
         file=sys.stderr)
   print(f"  cat-3 QAs total: {total_qa_cat3}, with miss: {missed_qa_cat3}", file=sys.stderr)
   print(f"  evidence pieces: {total_evidence}, missed: {missed_evidence}", file=sys.stderr)
   print(f"  bucket counts: {bucket_counts}", file=sys.stderr)
   print(f"  elapsed: {elapsed:.1f}s", file=sys.stderr)


if __name__ == "__main__":
   main()
