#!/usr/bin/env python3
"""Diff two per-QA JSONL dumps from --dump-failures.

Stratifies cases by transition (before → after):
  WIN          wrong → CORRECT
  REGRESS      CORRECT → wrong
  STILL_OK     CORRECT → CORRECT
  STILL_FAIL   wrong → wrong (possibly different failure mode)

Within STILL_FAIL, secondary stratification shows whether the failure mode
shifted (e.g., MIS_COMPOSED → NOT_ENTAILABLE).

Usage:
  python3 diff_runs.py before.jsonl after.jsonl
  python3 diff_runs.py before.jsonl after.jsonl --samples 5
  python3 diff_runs.py before.jsonl after.jsonl --transition REGRESS --samples 10
"""

import argparse
import json
import sys
from collections import Counter, defaultdict


def classify(rec, reach_threshold=1.0):
    reach = rec.get("recall_reach", 0.0) or 0.0
    ent = rec.get("entailment")
    corr = rec.get("correctness")
    if corr is None or ent is None:
        return "EXCLUDED"
    if corr == 1.0:
        return "CORRECT"
    if ent == 1.0:
        return "MIS_COMPOSED"
    if reach >= reach_threshold:
        return "NOT_ENTAILABLE"
    return "RETRIEVAL_MISS"


def load(path):
    """JSONL → dict keyed by (conv_idx, question)."""
    out = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            r = json.loads(line)
            key = (r["conv_idx"], r["question"])
            out[key] = r
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("before", help="JSONL from baseline run")
    ap.add_argument("after", help="JSONL from candidate run")
    ap.add_argument("--samples", type=int, default=3,
                    help="Sample N cases per transition (default 3)")
    ap.add_argument("--transition", default=None,
                    choices=["WIN", "REGRESS", "STILL_OK", "STILL_FAIL"],
                    help="Restrict samples to one transition class")
    ap.add_argument("--category", default=None,
                    help="Restrict samples to a LoCoMo category")
    args = ap.parse_args()

    a = load(args.before)
    b = load(args.after)

    common = set(a.keys()) & set(b.keys())
    only_a = set(a.keys()) - set(b.keys())
    only_b = set(b.keys()) - set(a.keys())

    print(f"before: {len(a)} QAs   after: {len(b)} QAs   common: {len(common)}")
    if only_a or only_b:
        print(f"  only-in-before: {len(only_a)}   only-in-after: {len(only_b)}")
    print()

    # Per-key transition
    transitions = []  # list of (transition, before_bucket, after_bucket, before_rec, after_rec)
    for key in common:
        ra = a[key]; rb = b[key]
        ba = classify(ra); bb = classify(rb)
        if ba == "EXCLUDED" or bb == "EXCLUDED":
            t = "EXCLUDED"
        elif ba == "CORRECT" and bb == "CORRECT":
            t = "STILL_OK"
        elif ba == "CORRECT" and bb != "CORRECT":
            t = "REGRESS"
        elif ba != "CORRECT" and bb == "CORRECT":
            t = "WIN"
        else:
            t = "STILL_FAIL"
        transitions.append((t, ba, bb, ra, rb))

    # Overall counts
    t_counter = Counter(t for t, _, _, _, _ in transitions)
    total = sum(t_counter.values())
    print("Transition counts (over common QAs):")
    for t in ["STILL_OK", "WIN", "REGRESS", "STILL_FAIL", "EXCLUDED"]:
        n = t_counter[t]
        print(f"  {t:<12} {n:>5}  ({100*n/total:.1f}%)" if total else f"  {t:<12}    n/a")

    # Headline number delta
    before_correct = sum(1 for t, _, _, _, _ in transitions if t == "STILL_OK") + \
                     sum(1 for t, _, _, _, _ in transitions if t == "REGRESS")
    after_correct = sum(1 for t, _, _, _, _ in transitions if t == "STILL_OK") + \
                    sum(1 for t, _, _, _, _ in transitions if t == "WIN")
    print()
    print(f"Headline recall_generation (over common):")
    print(f"  before: {before_correct}/{total} = {100*before_correct/total:.2f}%")
    print(f"  after:  {after_correct}/{total} = {100*after_correct/total:.2f}%")
    print(f"  delta:  {after_correct - before_correct:+d}  ({100*(after_correct-before_correct)/total:+.2f}pp)")
    print()

    # STILL_FAIL bucket transitions
    print("STILL_FAIL — bucket movement (before → after):")
    sf_counter = Counter((ba, bb) for t, ba, bb, _, _ in transitions if t == "STILL_FAIL")
    for (ba, bb), n in sorted(sf_counter.items(), key=lambda x: -x[1]):
        print(f"  {ba:<16} → {bb:<16} {n:>4}")
    print()

    # Per-category transition matrix
    print("Per-category transitions:")
    cat_t = defaultdict(Counter)
    for t, _, _, ra, _ in transitions:
        cat_t[ra.get("category", "?")][t] += 1
    print(f"  {'cat':<5} {'N':>5} {'STILL_OK':>10} {'WIN':>6} {'REGRESS':>8} {'STILL_FAIL':>11}")
    for cat in sorted(cat_t.keys()):
        c = cat_t[cat]
        n = sum(c.values())
        # delta = WIN - REGRESS
        delta = c["WIN"] - c["REGRESS"]
        print(f"  {cat:<5} {n:>5} {c['STILL_OK']:>10} {c['WIN']:>6} {c['REGRESS']:>8} {c['STILL_FAIL']:>11}  Δ={delta:+d}")
    print()

    # Bucket migration table (all transitions, not just STILL_FAIL)
    print("Bucket migration (all common QAs, before → after):")
    bmig = Counter((ba, bb) for _, ba, bb, _, _ in transitions if ba != "EXCLUDED" and bb != "EXCLUDED")
    buckets = ["CORRECT", "MIS_COMPOSED", "NOT_ENTAILABLE", "RETRIEVAL_MISS"]
    header_label = "before -> after"
    print(f"  {header_label:<18} " + " ".join(f"{b:>16}" for b in buckets))
    for ba in buckets:
        row = [bmig.get((ba, bb), 0) for bb in buckets]
        cells = " ".join(f"{n:>16}" for n in row)
        print(f"  {ba:<18} {cells}")
    print()

    # Samples
    if args.samples > 0:
        focus = [args.transition] if args.transition else ["WIN", "REGRESS"]
        for t in focus:
            picks = [(ba, bb, ra, rb) for tr, ba, bb, ra, rb in transitions
                     if tr == t and (args.category is None or str(ra.get("category")) == str(args.category))]
            if not picks:
                continue
            print("=" * 78)
            print(f"Samples — transition={t}"
                  + (f" category={args.category}" if args.category else "")
                  + f"  ({len(picks)} total, showing {min(args.samples, len(picks))})")
            print("=" * 78)
            for ba, bb, ra, rb in picks[:args.samples]:
                print()
                print(f"  conv={ra['conv_idx']}  cat={ra['category']}  ({ba} → {bb})")
                print(f"  Q:    {ra['question']}")
                print(f"  GOLD: {ra['gold_answer']}")
                print(f"  BEFORE gen: {ra.get('generated')}")
                print(f"  AFTER  gen: {rb.get('generated')}")
                # Show top-2 fact texts before/after
                bf = (ra.get("retrieved") or [])[:2]
                af = (rb.get("retrieved") or [])[:2]
                print(f"  BEFORE top facts:")
                for f in bf: print(f"    - {f.get('text','')[:140]}")
                print(f"  AFTER  top facts:")
                for f in af: print(f"    - {f.get('text','')[:140]}")


if __name__ == "__main__":
    main()
