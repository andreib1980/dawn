#!/usr/bin/env python3
"""Diagnose where the leader-comparable LoCoMo number is bleeding.

Reads the per-QA JSONL produced by run_benchmark.py --dump-failures and
classifies each QA into one of four buckets:

  CORRECT          correctness == 1.0
  MIS_COMPOSED     entailment == 1.0 AND correctness == 0.0
                   (facts contain the answer; LLM mis-composed it)
  NOT_ENTAILABLE   entailment == 0.0 AND recall_reach >= threshold
                   (retrieval covered gold dia_ids but facts can't
                   support the answer — extraction quality gap)
  RETRIEVAL_MISS   recall_reach < threshold
                   (retrieval didn't even cover the gold dia_ids)
  EXCLUDED         entailment / correctness exhausted retries

These map directly onto the diagnostic ladder.  recall_reach is the
geometric provenance overlap; recall_entailment is "could a careful
reader derive the answer from the retrieved facts"; recall_generation
is the leader-comparable LLM-judge metric.  A QA being CORRECT means
the whole pipeline worked.  A QA being MIS_COMPOSED means we found the
facts AND they entail the answer AND we still got it wrong — that's
purely a generator-side issue.  NOT_ENTAILABLE means retrieval brought
back fact texts that don't carry the answer (extraction skipped key
detail, or memory_callback shape doesn't surface it).  RETRIEVAL_MISS
means we never even got to the right dialog.

Usage:
  python3 diagnose_failures.py /tmp/locomo_failures.jsonl
  python3 diagnose_failures.py --reach-threshold 0.5 file.jsonl
  python3 diagnose_failures.py --samples 5 file.jsonl
  python3 diagnose_failures.py --category 3 --samples 10 file.jsonl
"""

import argparse
import json
import sys
from collections import Counter, defaultdict


def classify(rec, reach_threshold):
    reach = rec.get("recall_reach", 0.0) or 0.0
    ent = rec.get("entailment")
    corr = rec.get("correctness")
    if corr is None or ent is None:
        return "EXCLUDED"
    if corr == 1.0:
        return "CORRECT"
    # corr == 0.0 from here
    if ent == 1.0:
        return "MIS_COMPOSED"
    if reach >= reach_threshold:
        return "NOT_ENTAILABLE"
    return "RETRIEVAL_MISS"


def fmt_pct(num, denom):
    if denom <= 0:
        return "n/a"
    return f"{num}/{denom} ({100.0 * num / denom:.1f}%)"


def summary_table(records, reach_threshold):
    total = Counter()
    per_cat = defaultdict(Counter)
    for r in records:
        b = classify(r, reach_threshold)
        cat = r.get("category", "?")
        total[b] += 1
        per_cat[cat][b] += 1

    buckets = ["CORRECT", "MIS_COMPOSED", "NOT_ENTAILABLE",
               "RETRIEVAL_MISS", "EXCLUDED"]
    overall = sum(total.values())

    print("=" * 78)
    print(f"Failure stratification (reach threshold = {reach_threshold}, "
          f"N={overall})")
    print("=" * 78)
    print()
    print(f"  {'bucket':<18} {'count':>10}  {'share':>10}")
    print(f"  {'-'*18:<18} {'-'*10:>10}  {'-'*10:>10}")
    for b in buckets:
        n = total[b]
        share = f"{100.0 * n / overall:.1f}%" if overall else "n/a"
        print(f"  {b:<18} {n:>10}  {share:>10}")
    print()

    # Cumulative "leakage" from each pipeline stage
    correct = total["CORRECT"]
    correct_or_mis = correct + total["MIS_COMPOSED"]
    correct_or_mis_or_unent = correct_or_mis + total["NOT_ENTAILABLE"]
    print("Pipeline accumulation:")
    print(f"  recall_generation  (correct only):           "
          f"{fmt_pct(correct, overall)}")
    print(f"  ceiling if perfect generator (corr|mis):    "
          f"{fmt_pct(correct_or_mis, overall)}")
    print(f"  ceiling if perfect extraction/context:      "
          f"{fmt_pct(correct_or_mis_or_unent, overall)}")
    print()

    print("Per-category breakdown:")
    print(f"  {'cat':<5} {'N':>6} " + " ".join(f"{b:>14}" for b in buckets))
    for cat in sorted(per_cat.keys()):
        cnts = per_cat[cat]
        n = sum(cnts.values())
        cells = " ".join(
            f"{cnts[b]:>3} ({100.0*cnts[b]/n:>5.1f}%)" if n else f"{'':>14}"
            for b in buckets
        )
        print(f"  {str(cat):<5} {n:>6} {cells}")
    print()


def print_samples(records, bucket, reach_threshold, n=3, category=None):
    matches = [r for r in records
               if classify(r, reach_threshold) == bucket
               and (category is None or str(r.get("category")) == str(category))]
    if not matches:
        return
    print("=" * 78)
    print(f"Samples — bucket={bucket}"
          + (f" category={category}" if category else "")
          + f"  ({len(matches)} total, showing {min(n, len(matches))})")
    print("=" * 78)
    for r in matches[:n]:
        print()
        print(f"  conv={r['conv_idx']}  cat={r['category']}  "
              f"reach={r['recall_reach']:.2f}  ent={r.get('entailment')}  "
              f"corr={r.get('correctness')}")
        print(f"  Q: {r['question']}")
        print(f"  GOLD: {r['gold_answer']}")
        if r.get("generated") is not None:
            print(f"  GEN:  {r['generated']}")
        print(f"  evidence dia_ids: {r['evidence']}")
        print(f"  retrieved dia_ids: {r['retrieved_dia_ids']}")
        mt = r.get("memory_text")
        if mt:
            # Trim memory text for readability — show first ~800 chars
            mt_preview = mt[:800] + (" ... [truncated]" if len(mt) > 800 else "")
            print(f"  MEMORY_TEXT (with_source, len={len(mt)}):")
            for line in mt_preview.splitlines():
                print(f"    {line}")
        else:
            print("  retrieved facts (top 5):")
            for f in (r.get("retrieved") or [])[:5]:
                txt = f.get("text", "")[:160]
                print(f"    - {txt}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("jsonl", help="Per-QA dump from --dump-failures")
    ap.add_argument("--reach-threshold", type=float, default=1.0,
                    help="recall_reach >= threshold counts as 'retrieval ok'. "
                    "Default 1.0 (strict: every gold dia_id must be covered). "
                    "Set to 0.5 for partial-coverage semantics.")
    ap.add_argument("--samples", type=int, default=3,
                    help="Sample N failing cases per bucket (default 3)")
    ap.add_argument("--category", default=None,
                    help="Restrict samples to a single LoCoMo category")
    ap.add_argument("--buckets", default="MIS_COMPOSED,NOT_ENTAILABLE,RETRIEVAL_MISS",
                    help="Comma-separated bucket names to sample (default: all 3 failure buckets)")
    args = ap.parse_args()

    records = []
    with open(args.jsonl) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            records.append(json.loads(line))

    if not records:
        print("No records in JSONL.", file=sys.stderr)
        sys.exit(1)

    summary_table(records, args.reach_threshold)

    if args.samples > 0:
        for bucket in args.buckets.split(","):
            bucket = bucket.strip()
            if not bucket:
                continue
            print_samples(records, bucket, args.reach_threshold,
                          n=args.samples, category=args.category)


if __name__ == "__main__":
    main()
