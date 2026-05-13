#!/usr/bin/env python3
"""
LoCoMo cat-3 failure-mode profiler — memory-pipeline retrieval path.

Sibling to cat3_misses.py.  Where that tool runs document_chunks
retrieval (raw-dialog cosine), this one runs the production
memory_action_search path: it loads pre-extracted snapshots from the
shared cache, fires deep top-K queries against memory_facts, and maps
fact provenance ranges back to dia_ids so we can see where the gold
evidence lands in the production retrieval pool.

Failure-mode buckets (each gold dia_id classifies independently):

  - top-10 (recall hit)        — at least one fact covering the dia_id
                                 is in the top-10 by hybrid score
  - ranking failure (11-50)    — covered by some fact in the deep pool,
                                 but not top-10
  - ranking failure (51+)      — covered by some fact below rank 50
  - extraction failure         — no fact in memory covers this dia_id
                                 (extraction didn't capture the turn,
                                 OR the LLM extracted around it without
                                 anchoring provenance to the right msg
                                 range)

The extraction-failure bucket is unique to memory-pipeline mode —
document_chunks always has every turn in the candidate pool.  Mapping
it out here is the primary reason to re-profile on this path.

Run:
   python3 benchmarks/cat3_misses_memory.py \\
       --binary ./build-debug/tests/bench_retrieval \\
       --dataset ~/datasets/locomo/data/locomo10.json \\
       --cache-dir ./benchmarks/snapshots \\
       --output cat3_misses_memory.json
"""

import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_benchmark import (BenchRetrieval, _snapshot_cache_paths,
                           parse_locomo_session_date)


# Mirror run_locomo_memory()'s production defaults so misses reflect what
# real LoCoMo runs see.  search_score_floor=0.30 is the new default shipped
# 2026-05-13; profile against that to capture the gate's behavior.
TEMPORAL_WEIGHT = 0.20
PROPER_NOUN_BOOST = 1.0
SEARCH_SCORE_FLOOR = 0.30

# top_k for the deep retrieval probe.  BENCH_MP_FACT_LIST_CAP is 500 in
# bench_memory_pipeline.c — well above any LoCoMo conv's fact count
# (max ~317), so this effectively returns the full pool ranked by hybrid
# score.  A gold dia_id with rank=None after that probe is an extraction
# miss, not a ranking miss.
DEEP_TOP_K = 500
EVAL_TOP_K = 10
RANK_BUCKETS = [
    (1, 10, "top-10 (recall hit)"),
    (11, 50, "ranking failure (11-50)"),
    (51, DEEP_TOP_K, "ranking failure (51+)"),
]


def bucket_for_rank(rank):
    if rank is None:
        return "extraction failure (no fact covers dia_id)"
    for lo, hi, label in RANK_BUCKETS:
        if lo <= rank <= hi:
            return label
    return "extraction failure (no fact covers dia_id)"


def collect_dialog_index(conv):
    """Map dia_id -> {session_num, session_date, speaker, text}.  Mirror of
    the helper in cat3_misses.py."""
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


def min_rank_per_dia_id(retrieved):
    """For each dia_id covered by any retrieved fact, return the minimum
    rank (= best position) of a fact that covers it.  retrieved is a list
    of dicts with 'rank' (1-indexed) and 'covered_dia_ids' (list of dia_id
    strings).

    A fact's provenance range can span many dia_ids; the same dia_id can
    be covered by multiple facts at different ranks.  For evaluating
    'did retrieval find this dia_id', the relevant signal is the best
    rank that surfaced it.
    """
    rank_for = {}
    for r in retrieved:
        rank = r.get("rank")
        if rank is None:
            continue
        for did in r.get("covered_dia_ids", []) or []:
            cur = rank_for.get(did)
            if cur is None or rank < cur:
                rank_for[did] = rank
    return rank_for


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", required=True,
                    help="Path to bench_retrieval binary")
    ap.add_argument("--dataset", required=True,
                    help="Path to locomo10.json")
    ap.add_argument("--cache-dir", default="./benchmarks/snapshots",
                    help="Directory holding pre-extracted snapshots (default: "
                    "./benchmarks/snapshots).  Must be populated by a prior "
                    "memory-pipeline run; this profiler is read-only against "
                    "cached state.")
    ap.add_argument("--output", default="cat3_misses_memory.json")
    ap.add_argument("--search-score-floor", type=float, default=SEARCH_SCORE_FLOOR,
                    dest="search_score_floor",
                    help="Pass-through to bench_retrieval --search-score-floor. "
                    "Default 0.30 (matches the shipped runtime default).")
    args = ap.parse_args()

    data = json.load(open(args.dataset))
    cache_dir = Path(args.cache_dir)
    if not cache_dir.is_dir():
        print(f"error: --cache-dir does not exist: {cache_dir}", file=sys.stderr)
        return 2

    print(f"  Starting bench_retrieval --memory-pipeline "
          f"(temporal={TEMPORAL_WEIGHT}, pn-boost={PROPER_NOUN_BOOST}, "
          f"floor={args.search_score_floor}, deep top-K={DEEP_TOP_K})",
          file=sys.stderr)
    engine = BenchRetrieval(
        args.binary,
        provider="onnx",
        temporal_weight=TEMPORAL_WEIGHT,
        proper_noun_boost=PROPER_NOUN_BOOST,
        memory_pipeline=True,
        search_score_floor=args.search_score_floor,
    )
    print(f"  Ready: {engine.dims} dims, mode={engine.mode}, "
          f"extraction={engine.ready_info.get('extraction_provider', '?')}:"
          f"{engine.ready_info.get('extraction_model', '?')}",
          file=sys.stderr)

    misses = []           # one entry per cat-3 QA with any missed evidence
    bucket_counts = {}    # per-evidence-piece tally
    total_evidence = 0
    missed_evidence = 0
    total_qa_cat3 = 0
    missed_qa_cat3 = 0
    snapshot_misses = 0   # convs whose snapshot wasn't cached (skipped)

    t0 = time.time()
    for conv_idx, entry in enumerate(data):
        conv = entry.get("conversation", entry)
        cat3_qas = [q for q in entry.get("qa", [])
                    if str(q.get("category")) == "3" and q.get("evidence")]
        if not cat3_qas:
            continue

        # Find the cached snapshot for this conv.  The profiler is read-only;
        # we skip convs that haven't been extracted yet (run the full
        # memory-pipeline bench once first to populate the cache).
        db_path, map_path, key = _snapshot_cache_paths(
            cache_dir, engine, conv_idx, conv)
        if not db_path.exists() or not map_path.exists():
            print(f"  [conv {conv_idx + 1}/{len(data)}] cache MISS key={key} "
                  f"— skip ({len(cat3_qas)} cat-3 QAs)", file=sys.stderr)
            snapshot_misses += 1
            continue

        lresp = engine.snapshot_load(db_path, map_path)
        if lresp.get("status") != "ok":
            print(f"  [conv {conv_idx + 1}/{len(data)}] snapshot_load failed: "
                  f"{lresp}", file=sys.stderr)
            snapshot_misses += 1
            continue

        dialog_index = collect_dialog_index(conv)

        for qa in cat3_qas:
            total_qa_cat3 += 1
            question = qa.get("question", "")
            gold_answer = qa.get("answer", "")
            evidence_raw = qa.get("evidence", [])
            # Normalize: LoCoMo conv 8 has 'D9:1 D4:4 D4:6' as one string in
            # three cat-3 questions.  Split on whitespace then flatten.
            evidence_ids = []
            for e in evidence_raw:
                evidence_ids.extend(str(e).split())

            qresp = engine.query_memory(question, top_k=DEEP_TOP_K)
            retrieved = qresp.get("results", [])
            # bench emits results in score-descending order; add 1-indexed rank
            for i, r in enumerate(retrieved):
                r["rank"] = i + 1

            rank_for_dia = min_rank_per_dia_id(retrieved)

            per_evidence = []
            any_missed = False
            for ev_id in evidence_ids:
                total_evidence += 1
                rank = rank_for_dia.get(ev_id)
                bucket = bucket_for_rank(rank)
                bucket_counts[bucket] = bucket_counts.get(bucket, 0) + 1
                ev_info = dialog_index.get(ev_id, {})
                in_top_eval = (rank is not None and rank <= EVAL_TOP_K)
                if not in_top_eval:
                    missed_evidence += 1
                    any_missed = True
                per_evidence.append({
                    "evidence_id": ev_id,
                    "best_fact_rank": rank,
                    "bucket": bucket,
                    "session": ev_info.get("session_num"),
                    "session_date": ev_info.get("session_date"),
                    "speaker": ev_info.get("speaker"),
                    "text": ev_info.get("text"),
                })

            if not any_missed:
                continue
            missed_qa_cat3 += 1

            # Top-10 retrieved facts — what beat the evidence
            top10 = []
            for r in retrieved[:EVAL_TOP_K]:
                top10.append({
                    "rank": r["rank"],
                    "fact_id": r.get("fact_id"),
                    "score": r.get("score"),
                    "text": r.get("text"),
                    "covered_dia_ids": r.get("covered_dia_ids", []),
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
            "search_score_floor": args.search_score_floor,
            "deep_top_k": DEEP_TOP_K,
            "eval_top_k": EVAL_TOP_K,
            "retrieval_path": "memory-pipeline (memory_action_search via "
                              "memory_embeddings_hybrid_search)",
        },
        "totals": {
            "total_cat3_qa": total_qa_cat3,
            "qa_with_any_miss": missed_qa_cat3,
            "total_evidence_pieces": total_evidence,
            "missed_evidence_pieces": missed_evidence,
            "fraction_recall_avg": (
                (total_evidence - missed_evidence) / total_evidence
                if total_evidence else 0.0),
            "snapshot_cache_misses": snapshot_misses,
        },
        "evidence_rank_buckets": bucket_counts,
        "elapsed_seconds": elapsed,
        "misses": misses,
    }

    with open(args.output, "w") as f:
        json.dump(summary, f, indent=2)

    print(f"\n  Wrote {args.output} "
          f"({len(misses)} cat-3 QAs with at least one missed evidence)",
          file=sys.stderr)
    print(f"  cat-3 QAs total: {total_qa_cat3}, with miss: {missed_qa_cat3}",
          file=sys.stderr)
    print(f"  evidence pieces: {total_evidence}, missed: {missed_evidence}",
          file=sys.stderr)
    print(f"  bucket counts: {bucket_counts}", file=sys.stderr)
    if snapshot_misses:
        print(f"  WARNING: {snapshot_misses} convs skipped (no cached "
              f"snapshot — run the full memory-pipeline bench once first).",
              file=sys.stderr)
    print(f"  elapsed: {elapsed:.1f}s", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
