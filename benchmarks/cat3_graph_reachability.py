#!/usr/bin/env python3
"""
LoCoMo cat-3 graph reachability diagnostic.

For each missed evidence piece from cat3_misses_memory.py, ask: is this
dia_id reachable via the entity-graph from the query's proper nouns?

The answer is the ceiling of what any graph-based retrieval policy
could achieve on this dataset.  If ~all misses are reachable, there's
real lift available with the right policy.  If few are reachable,
graph retrieval isn't the right tool for this gap.

Method:
  1. Per conv: snapshot_load
  2. Per cat-3 question:
     - Run query_graph_only(question) → graph-reachable fact set
     - Union their covered_dia_ids → graph-reachable dia_id set
  3. For each gold evidence piece previously classified as a miss
     (rank > 10 in cat3_misses_memory.json), record:
     - whether the gold dia_id is in the graph-reachable set
     - whether the query produced any seed entities at all

Run:
   python3 benchmarks/cat3_graph_reachability.py \\
       --binary ./build-debug/tests/bench_retrieval \\
       --dataset ~/datasets/locomo/data/locomo10.json \\
       --misses-input /tmp/cat3_misses_memory.json \\
       --cache-dir ./benchmarks/snapshots \\
       --output cat3_graph_reachability.json
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
                    help="Path to cat3_misses_memory.json from the prior profile run")
    ap.add_argument("--cache-dir", default="./benchmarks/snapshots")
    ap.add_argument("--output", default="cat3_graph_reachability.json")
    args = ap.parse_args()

    data = json.load(open(args.dataset))
    cache_dir = Path(args.cache_dir)
    misses_data = json.load(open(args.misses_input))
    # misses_data["misses"] is a list of QA entries (the ones with any miss).
    # Re-key by (conv_idx, question) so we can match per-question.
    miss_lookup = {}
    for m in misses_data["misses"]:
        key = (m["conv_idx"], m["question"])
        miss_lookup[key] = m

    print(f"  Starting bench_retrieval --memory-pipeline", file=sys.stderr)
    engine = BenchRetrieval(
        args.binary,
        provider="onnx",
        memory_pipeline=True,
    )
    print(f"  Ready: {engine.dims} dims", file=sys.stderr)

    # Stats
    total_missed_pieces = 0
    seeded_questions = 0
    unseeded_questions = 0
    graph_reachable_misses = 0
    graph_unreachable_misses = 0
    per_question = []

    t0 = time.time()
    for conv_idx, entry in enumerate(data):
        conv = entry.get("conversation", entry)
        cat3_qas = [q for q in entry.get("qa", [])
                    if str(q.get("category")) == "3" and q.get("evidence")]
        if not cat3_qas:
            continue
        # Only profile convs that had misses in the prior run (others are
        # complete-recall and don't affect this diagnostic).
        relevant = [q for q in cat3_qas
                    if (conv_idx, q.get("question", "")) in miss_lookup]
        if not relevant:
            continue

        db_path, map_path, key = _snapshot_cache_paths(cache_dir, engine, conv_idx, conv)
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
            # Run the graph-only retrieval
            gresp = engine._send({"cmd": "query_graph_only", "text": question})
            seed_count = gresp.get("seed_count", 0)
            graph_results = gresp.get("results", [])
            graph_dia_ids = set()
            for r in graph_results:
                graph_dia_ids.update(r.get("covered_dia_ids", []) or [])

            if seed_count == 0:
                unseeded_questions += 1
            else:
                seeded_questions += 1

            # For each miss piece for this question, check reachability
            per_piece = []
            for ev in miss_entry["evidence"]:
                if "top-10" in ev["bucket"]:
                    continue  # not a miss
                total_missed_pieces += 1
                gold_dia = ev["evidence_id"]
                reachable = gold_dia in graph_dia_ids
                if reachable:
                    graph_reachable_misses += 1
                else:
                    graph_unreachable_misses += 1
                per_piece.append({
                    "evidence_id": gold_dia,
                    "best_fact_rank": ev.get("best_fact_rank"),
                    "bucket": ev["bucket"],
                    "graph_reachable": reachable,
                    "speaker": ev.get("speaker"),
                    "text": ev.get("text", "")[:120],
                })

            per_question.append({
                "conv_idx": conv_idx,
                "question": question,
                "gold_answer": miss_entry.get("gold_answer", ""),
                "seed_count": seed_count,
                "graph_fact_count": len(graph_results),
                "graph_reachable_dia_count": len(graph_dia_ids),
                "missed_evidence": per_piece,
            })

        print(f"  [conv {conv_idx + 1}/{len(data)}] processed {len(relevant)} miss-QAs",
              file=sys.stderr)

    elapsed = time.time() - t0
    engine.quit()

    # Bucket-level breakdown
    bucket_breakdown = {}  # bucket -> {reachable, unreachable}
    for q in per_question:
        for piece in q["missed_evidence"]:
            b = piece["bucket"]
            bucket_breakdown.setdefault(b, {"reachable": 0, "unreachable": 0})
            if piece["graph_reachable"]:
                bucket_breakdown[b]["reachable"] += 1
            else:
                bucket_breakdown[b]["unreachable"] += 1

    summary = {
        "total_missed_pieces": total_missed_pieces,
        "graph_reachable_misses": graph_reachable_misses,
        "graph_unreachable_misses": graph_unreachable_misses,
        "reachable_pct": (graph_reachable_misses / total_missed_pieces * 100
                          if total_missed_pieces else 0.0),
        "seeded_questions": seeded_questions,
        "unseeded_questions": unseeded_questions,
        "per_bucket": bucket_breakdown,
        "elapsed_seconds": elapsed,
        "per_question": per_question,
    }
    with open(args.output, "w") as f:
        json.dump(summary, f, indent=2)

    print(f"\n=== Reachability summary ===", file=sys.stderr)
    print(f"  Total missed evidence pieces: {total_missed_pieces}", file=sys.stderr)
    print(f"  Graph-reachable misses:      {graph_reachable_misses} "
          f"({summary['reachable_pct']:.1f}%)", file=sys.stderr)
    print(f"  Graph-unreachable misses:    {graph_unreachable_misses}", file=sys.stderr)
    print(f"  Questions with seeds:        {seeded_questions}", file=sys.stderr)
    print(f"  Questions without seeds:     {unseeded_questions}", file=sys.stderr)
    print(f"  Per-bucket reachability:", file=sys.stderr)
    for b, d in bucket_breakdown.items():
        total = d["reachable"] + d["unreachable"]
        pct = d["reachable"] / total * 100 if total else 0
        print(f"    {b}: {d['reachable']}/{total} reachable ({pct:.0f}%)", file=sys.stderr)
    print(f"  elapsed: {elapsed:.1f}s", file=sys.stderr)


if __name__ == "__main__":
    main()
