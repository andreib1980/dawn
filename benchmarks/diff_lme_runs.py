#!/usr/bin/env python3
"""Compare two LongMemEval memory-pipeline JSONL dumps from --dump-failures.

Lighter than diff_runs.py because LongMemEval has no entailment judge in the
memory-pipeline path — just session-overlap recall + generation correctness.

Output:
- Headline correctness delta (WIN/REGRESS)
- Per-question-type breakdown
- Sample WIN cases (filter helped)
- Sample REGRESS cases (filter hurt)

Usage:
  python3 diff_lme_runs.py before.jsonl after.jsonl
  python3 diff_lme_runs.py before.jsonl after.jsonl --samples 5
"""

import argparse
import json
import sys
from collections import Counter, defaultdict


def load(path):
    out = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            r = json.loads(line)
            key = r["question_id"]
            out[key] = r
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("before", help="JSONL from filter-OFF / baseline run")
    ap.add_argument("after", help="JSONL from filter-ON / candidate run")
    ap.add_argument("--samples", type=int, default=3,
                    help="Sample N WIN + N REGRESS cases (default 3)")
    args = ap.parse_args()

    a = load(args.before)
    b = load(args.after)

    common = set(a.keys()) & set(b.keys())
    only_a = set(a.keys()) - set(b.keys())
    only_b = set(b.keys()) - set(a.keys())
    print(f"before: {len(a)} Qs  after: {len(b)} Qs  common: {len(common)}")
    if only_a or only_b:
        print(f"  only-in-before: {len(only_a)}  only-in-after: {len(only_b)}")
    print()

    # Transition: based on correctness (1.0 or 0.0, None = excluded)
    win = []
    regress = []
    same_ok = 0
    same_fail = 0
    excluded = 0
    for k in common:
        ca = a[k].get("correctness")
        cb = b[k].get("correctness")
        if ca is None or cb is None:
            excluded += 1
            continue
        if ca == cb:
            if ca == 1.0:
                same_ok += 1
            else:
                same_fail += 1
        elif cb == 1.0:
            win.append(k)
        else:
            regress.append(k)

    total = same_ok + same_fail + len(win) + len(regress)
    print(f"Transitions (correctness, n={total}):")
    print(f"  STILL_OK    {same_ok}")
    print(f"  WIN         {len(win)}  (wrong → CORRECT)")
    print(f"  REGRESS     {len(regress)}  (CORRECT → wrong)")
    print(f"  STILL_FAIL  {same_fail}")
    if excluded:
        print(f"  EXCLUDED    {excluded}  (no correctness on one side)")
    print()

    before_correct = same_ok + len(regress)
    after_correct = same_ok + len(win)
    print(f"Headline correctness (over n={total}):")
    if total:
        print(f"  before: {before_correct}/{total} = {100 * before_correct / total:.2f}%")
        print(f"  after:  {after_correct}/{total} = {100 * after_correct / total:.2f}%")
        print(f"  delta:  {after_correct - before_correct:+d}  "
              f"({100 * (after_correct - before_correct) / total:+.2f}pp)")
    print()

    # Per question_type
    qt_stats = defaultdict(lambda: {"win": 0, "regress": 0, "still_ok": 0, "still_fail": 0, "n": 0})
    for k in common:
        qt = a[k].get("question_type", "?")
        ca = a[k].get("correctness")
        cb = b[k].get("correctness")
        if ca is None or cb is None:
            continue
        qt_stats[qt]["n"] += 1
        if ca == cb == 1.0:
            qt_stats[qt]["still_ok"] += 1
        elif ca == cb == 0.0:
            qt_stats[qt]["still_fail"] += 1
        elif cb == 1.0:
            qt_stats[qt]["win"] += 1
        else:
            qt_stats[qt]["regress"] += 1
    print("Per question_type:")
    print(f"  {'type':<28} {'N':>4} {'OK':>4} {'WIN':>4} {'REGR':>5} {'FAIL':>5}  Δ")
    for qt in sorted(qt_stats.keys()):
        s = qt_stats[qt]
        delta = s["win"] - s["regress"]
        print(f"  {qt:<28} {s['n']:>4} {s['still_ok']:>4} {s['win']:>4} "
              f"{s['regress']:>5} {s['still_fail']:>5}  {delta:+d}")
    print()

    # Session recall delta
    sr_a = [a[k].get("recall", 0) for k in common if a[k].get("recall") is not None]
    sr_b = [b[k].get("recall", 0) for k in common if b[k].get("recall") is not None]
    if sr_a and sr_b:
        print(f"Session-overlap recall (mean): before={sum(sr_a)/len(sr_a):.4f}  "
              f"after={sum(sr_b)/len(sr_b):.4f}  delta={(sum(sr_b)/len(sr_b))-(sum(sr_a)/len(sr_a)):+.4f}")
        print()

    # Sample WIN / REGRESS
    if args.samples > 0:
        for label, keys in [("WIN", win), ("REGRESS", regress)]:
            if not keys:
                continue
            print("=" * 78)
            print(f"Samples — {label}  ({len(keys)} total, showing {min(args.samples, len(keys))})")
            print("=" * 78)
            for k in keys[:args.samples]:
                ra = a[k]
                rb = b[k]
                print()
                print(f"  qid={k}  type={ra.get('question_type')}")
                print(f"  Q:    {ra['question'][:200]}")
                print(f"  GOLD: {str(ra.get('gold_answer', ''))[:200]}")
                print(f"  BEFORE gen: {ra.get('generated')}")
                print(f"  AFTER  gen: {rb.get('generated')}")
                print(f"  BEFORE recall: {ra.get('recall')}")
                print(f"  AFTER  recall: {rb.get('recall')}")
                # answer_session_ids overlap
                a_sids_b = set(ra.get('answer_session_ids', []))
                ret_a = set(ra.get('retrieved_session_ids', []))
                ret_b = set(rb.get('retrieved_session_ids', []))
                print(f"  retrieved-set delta: +{len(ret_b - ret_a)} -{len(ret_a - ret_b)}")


if __name__ == "__main__":
    main()
