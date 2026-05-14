#!/usr/bin/env python3
"""
LoCoMo cat-3 2-hop graph reachability diagnostic.

Sibling to cat3_graph_reachability.py (1-hop only).  Answers the
Phase 2 design question: "of the misses unreachable via Phase 1A's
1-hop walk, how many become reachable when we expand to 2 hops?"

Method (matches cat3_graph_reachability.py shape):
  1. Per conv: snapshot_load the cached extraction state.
  2. Per cat-3 miss-question:
     - Run query_graph_only(question)   → 1-hop reachable dia_ids
     - Run query_graph_two_hop(question) → 2-hop reachable dia_ids
     - Classify each missed evidence piece:
        * reached_at_1hop       — in 1-hop set (Phase 1A already has it)
        * reached_only_at_2hop  — NEW reach from 2-hop (Phase 2 helps)
        * unreachable_at_2hop   — beyond 2-hop (Phase 2 won't help)
  3. Report per-bucket marginal lift.

The "reached_only_at_2hop" count is the upper bound on what Phase 2's
2-hop traversal can rescue.  Compare against total missed pieces to
get the reachability ceiling lift.

Run:
   python3 benchmarks/cat3_graph_2hop_reachability.py \\
       --binary ./build-debug/tests/bench_retrieval \\
       --dataset ~/datasets/locomo/data/locomo10.json \\
       --misses-input /tmp/cat3_misses_phase2_base.json \\
       --cache-dir ./benchmarks/snapshots_phase2_base \\
       --output /tmp/cat3_2hop_reachability.json
"""

import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_benchmark import BenchRetrieval, _snapshot_cache_paths


def main():
   ap = argparse.ArgumentParser(
      description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
   ap.add_argument("--binary", required=True)
   ap.add_argument("--dataset", required=True)
   ap.add_argument("--misses-input", required=True,
                   help="cat3_misses_memory.json from prior profile run")
   ap.add_argument("--cache-dir", default="./benchmarks/snapshots")
   ap.add_argument("--output", default="cat3_2hop_reachability.json")
   args = ap.parse_args()

   data = json.load(open(args.dataset))
   cache_dir = Path(args.cache_dir)
   misses_data = json.load(open(args.misses_input))
   miss_lookup = {(m["conv_idx"], m["question"]): m for m in misses_data["misses"]}

   print(f"  Starting bench_retrieval --memory-pipeline (1-hop + 2-hop)", file=sys.stderr)
   engine = BenchRetrieval(args.binary, provider="onnx", memory_pipeline=True)
   print(f"  Ready: {engine.dims} dims", file=sys.stderr)

   total_pieces = 0
   reached_1hop = 0
   reached_only_2hop = 0
   unreachable_2hop = 0
   per_question = []

   t0 = time.time()
   for conv_idx, entry in enumerate(data):
      conv = entry.get("conversation", entry)
      cat3_qas = [q for q in entry.get("qa", [])
                  if str(q.get("category")) == "3" and q.get("evidence")]
      relevant = [q for q in cat3_qas
                  if (conv_idx, q.get("question", "")) in miss_lookup]
      if not relevant:
         continue

      db_path, map_path, _ = _snapshot_cache_paths(cache_dir, engine, conv_idx, conv)
      if not db_path.exists() or not map_path.exists():
         print(f"  [conv {conv_idx + 1}] cache MISS — skip", file=sys.stderr)
         continue
      lresp = engine.snapshot_load(db_path, map_path)
      if lresp.get("status") != "ok":
         print(f"  [conv {conv_idx + 1}] snapshot_load failed", file=sys.stderr)
         continue

      for qa in relevant:
         question = qa.get("question", "")
         miss_entry = miss_lookup[(conv_idx, question)]

         # 1-hop reach (Phase 1A baseline)
         r1 = engine._send({"cmd": "query_graph_only", "text": question})
         dia_1hop = set()
         for r in (r1.get("results") or []):
            dia_1hop.update(r.get("covered_dia_ids", []) or [])
         seed_count = r1.get("seed_count", 0)

         # 2-hop reach
         r2 = engine._send({"cmd": "query_graph_two_hop", "text": question})
         dia_2hop = set()
         for r in (r2.get("results") or []):
            dia_2hop.update(r.get("covered_dia_ids", []) or [])
         expanded_seed_count = r2.get("expanded_seed_count", 0)

         per_piece = []
         for ev in miss_entry["evidence"]:
            if "top-10" in ev["bucket"]:
               continue
            total_pieces += 1
            gold_dia = ev["evidence_id"]
            in_1hop = gold_dia in dia_1hop
            in_2hop = gold_dia in dia_2hop

            if in_1hop:
               classification = "reached_at_1hop"
               reached_1hop += 1
            elif in_2hop:
               classification = "reached_only_at_2hop"
               reached_only_2hop += 1
            else:
               classification = "unreachable_at_2hop"
               unreachable_2hop += 1

            per_piece.append({
               "evidence_id": gold_dia,
               "best_fact_rank": ev.get("best_fact_rank"),
               "bucket": ev["bucket"],
               "classification": classification,
               "speaker": ev.get("speaker"),
               "text": ev.get("text", "")[:120],
            })

         per_question.append({
            "conv_idx": conv_idx,
            "question": question,
            "gold_answer": miss_entry.get("gold_answer", ""),
            "seed_count": seed_count,
            "expanded_seed_count": expanded_seed_count,
            "graph_facts_1hop": len(r1.get("results") or []),
            "graph_facts_2hop": len(r2.get("results") or []),
            "missed_evidence": per_piece,
         })

      print(f"  [conv {conv_idx + 1}/{len(data)}] processed {len(relevant)} miss-QAs",
            file=sys.stderr)

   elapsed = time.time() - t0
   engine.quit()

   # Per-bucket breakdown
   bucket_breakdown = {}
   for q in per_question:
      for p in q["missed_evidence"]:
         b = p["bucket"]
         bd = bucket_breakdown.setdefault(b, {"reached_at_1hop": 0,
                                              "reached_only_at_2hop": 0,
                                              "unreachable_at_2hop": 0})
         bd[p["classification"]] += 1

   summary = {
      "total_missed_pieces": total_pieces,
      "reached_at_1hop": reached_1hop,
      "reached_only_at_2hop": reached_only_2hop,
      "unreachable_at_2hop": unreachable_2hop,
      "reached_1hop_pct": (reached_1hop / total_pieces * 100 if total_pieces else 0.0),
      "marginal_2hop_pct": (reached_only_2hop / total_pieces * 100 if total_pieces else 0.0),
      "unreachable_pct": (unreachable_2hop / total_pieces * 100 if total_pieces else 0.0),
      "per_bucket": bucket_breakdown,
      "elapsed_seconds": elapsed,
      "per_question": per_question,
   }
   with open(args.output, "w") as f:
      json.dump(summary, f, indent=2)

   print(f"\n=== 2-hop Reachability summary ===", file=sys.stderr)
   print(f"  Total missed evidence pieces: {total_pieces}", file=sys.stderr)
   print(f"  Reached at 1-hop (Phase 1A): {reached_1hop} "
         f"({summary['reached_1hop_pct']:.1f}%)", file=sys.stderr)
   print(f"  Reached ONLY at 2-hop:       {reached_only_2hop} "
         f"({summary['marginal_2hop_pct']:.1f}%)  ← Phase 2 ceiling lift", file=sys.stderr)
   print(f"  Unreachable at 2-hop:        {unreachable_2hop} "
         f"({summary['unreachable_pct']:.1f}%)", file=sys.stderr)
   print(f"  Per-bucket:", file=sys.stderr)
   for b, d in bucket_breakdown.items():
      total = sum(d.values())
      print(f"    {b}:", file=sys.stderr)
      print(f"      1-hop reached:  {d['reached_at_1hop']}/{total}", file=sys.stderr)
      print(f"      +2-hop reached: {d['reached_only_at_2hop']}/{total}", file=sys.stderr)
      print(f"      unreachable:    {d['unreachable_at_2hop']}/{total}", file=sys.stderr)
   print(f"  elapsed: {elapsed:.1f}s", file=sys.stderr)


if __name__ == "__main__":
   main()
