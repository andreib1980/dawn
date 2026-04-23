#!/usr/bin/env python3
"""
DAWN Retrieval Benchmark Runner

Drives the bench_retrieval C binary with LongMemEval, LoCoMo, and ConvoMem
datasets, computing Recall@K and NDCG@K metrics to benchmark DAWN's
retrieval quality against published results.

Usage:
   python3 benchmarks/run_benchmark.py \\
       --binary ./build-debug/tests/bench_retrieval \\
       --benchmark longmemeval \\
       --dataset ~/datasets/longmemeval.json

   python3 benchmarks/run_benchmark.py \\
       --binary ./build-debug/tests/bench_retrieval \\
       --benchmark locomo \\
       --dataset ~/datasets/locomo10.json

   python3 benchmarks/run_benchmark.py \\
       --binary ./build-debug/tests/bench_retrieval \\
       --benchmark convomem \\
       --dataset ~/datasets/convomem/ \\
       --limit 100
"""

import argparse
import json
import math
import os
import subprocess
import sys
import time
from pathlib import Path


# =============================================================================
# Subprocess Wrapper
# =============================================================================


class BenchRetrieval:
   """Manages the bench_retrieval C binary subprocess."""

   def __init__(
      self, binary_path, provider="onnx", model="", endpoint="", api_key="", raw_mode=False
   ):
      cmd = [binary_path, "--provider", provider]
      if model:
         cmd += ["--model", model]
      if endpoint:
         cmd += ["--endpoint", endpoint]
      if api_key:
         cmd += ["--api-key", api_key]
      if raw_mode:
         cmd += ["--no-keyword-boost"]

      self.proc = subprocess.Popen(
         cmd,
         stdin=subprocess.PIPE,
         stdout=subprocess.PIPE,
         stderr=subprocess.PIPE,
         text=True,
         bufsize=1,
      )

      # Read ready message — skip OLOG preamble lines (logging is on stdout)
      self.ready_info = None
      for _ in range(50):
         line = self.proc.stdout.readline().strip()
         if not line:
            continue
         if line.startswith("{"):
            self.ready_info = json.loads(line)
            break
      if not self.ready_info or self.ready_info.get("status") != "ready":
         raise RuntimeError("bench_retrieval failed to start (no ready JSON)")

   def _send(self, obj):
      """Send a JSON command and read the response, skipping OLOG preamble lines."""
      self.proc.stdin.write(json.dumps(obj) + "\n")
      self.proc.stdin.flush()
      for _ in range(200):
         line = self.proc.stdout.readline().strip()
         if not line:
            continue
         if line.startswith("{"):
            return json.loads(line)
      raise RuntimeError(f"bench_retrieval: no JSON response for {obj.get('cmd')}")

   def add(self, doc_id, text):
      return self._send({"cmd": "add", "id": doc_id, "text": text})

   def query(self, text, top_k=10):
      return self._send({"cmd": "query", "text": text, "top_k": top_k})

   def reset(self):
      return self._send({"cmd": "reset"})

   def quit(self):
      self.proc.stdin.write(json.dumps({"cmd": "quit"}) + "\n")
      self.proc.stdin.flush()
      self.proc.wait(timeout=5)

   @property
   def dims(self):
      return self.ready_info.get("dims", 0)

   @property
   def provider(self):
      return self.ready_info.get("provider", "unknown")

   @property
   def mode(self):
      return self.ready_info.get("mode", "hybrid")


# =============================================================================
# Metrics
# =============================================================================


def recall_any_at_k(retrieved_ids, relevant_ids, k):
   """1.0 if any relevant ID appears in top-K, else 0.0."""
   top_k = set(retrieved_ids[:k])
   return float(any(rid in top_k for rid in relevant_ids))


def recall_all_at_k(retrieved_ids, relevant_ids, k):
   """1.0 if all relevant IDs appear in top-K, else 0.0."""
   top_k = set(retrieved_ids[:k])
   return float(all(rid in top_k for rid in relevant_ids))


def dcg(relevances, k):
   """Discounted Cumulative Gain."""
   score = 0.0
   for i, rel in enumerate(relevances[:k]):
      score += rel / math.log2(i + 2)
   return score


def ndcg_at_k(retrieved_ids, relevant_ids, k):
   """Normalized Discounted Cumulative Gain."""
   relevant_set = set(relevant_ids)
   relevances = [1.0 if rid in relevant_set else 0.0 for rid in retrieved_ids[:k]]
   ideal = sorted(relevances, reverse=True)
   idcg = dcg(ideal, k)
   if idcg == 0:
      return 0.0
   return dcg(relevances, k) / idcg


def partial_recall(retrieved_texts, evidence_texts, k):
   """Fraction of evidence texts found via substring match in top-K."""
   if not evidence_texts:
      return 1.0
   ret_texts = [t.strip().lower() for t in retrieved_texts[:k]]
   found = 0
   for ev in evidence_texts:
      ev_lower = ev.strip().lower()
      for rt in ret_texts:
         if ev_lower in rt or rt in ev_lower:
            found += 1
            break
   return found / len(evidence_texts)


# =============================================================================
# LongMemEval Benchmark
# =============================================================================


def turn_id_to_session_id(turn_id):
   """Extract session ID from a turn ID (e.g., 'sess_123_turn_4' -> 'sess_123')."""
   if "_turn_" in turn_id:
      return turn_id.rsplit("_turn_", 1)[0]
   return turn_id


def run_longmemeval(engine, dataset_path, limit=0, granularity="session", turn_scoring="official"):
   """Run LongMemEval benchmark. Returns metrics dict.

   granularity:
      'session' — one doc per session (all user turns joined). ~48 docs per question.
                  Scores against answer_session_ids.
      'turn'    — one doc per user turn. ~273 docs per question.
                  Uses top_k=5 to match RMM paper methodology (ACL 2025).

   turn_scoring (only applies when granularity='turn'):
      'official' — any user turn from an answer session counts as correct
                   (~11 correct targets per question). Matches the official
                   LongMemEval eval code (xiaowu0162/LongMemEval). Directly
                   comparable to RMM paper Table 1 numbers.
      'strict'   — only turns with has_answer=true count as correct
                   (~1.7 correct targets per question). Harder criterion.
   """
   with open(dataset_path) as f:
      data = json.load(f)

   if limit > 0:
      data = data[:limit]

   total = len(data)
   ks = [1, 3, 5, 10] if granularity == "session" else [1, 3, 5]
   metrics = {f"recall_any@{k}": [] for k in ks}
   metrics.update({f"ndcg@{k}": [] for k in ks})
   skipped = 0

   t0 = time.time()
   for i, entry in enumerate(data):
      sessions = entry["haystack_sessions"]
      session_ids = entry["haystack_session_ids"]
      question = entry["question"]

      engine.reset()

      if granularity == "turn":
         # One doc per user turn — turn-level evaluation
         # Build correct set based on scoring mode
         answer_turn_ids = set()
         answer_session_ids = set(entry["answer_session_ids"])

         for session, sess_id in zip(sessions, session_ids):
            turn_idx = 0
            for turn in session:
               if turn["role"] == "user" and turn["content"].strip():
                  turn_id = f"{sess_id}_turn_{turn_idx}"
                  engine.add(turn_id, turn["content"])

                  if turn_scoring == "official":
                     # Official: any turn from answer session is correct
                     if sess_id in answer_session_ids:
                        answer_turn_ids.add(turn_id)
                  else:
                     # Strict: only turns with has_answer=true
                     if turn.get("has_answer"):
                        answer_turn_ids.add(turn_id)

                  turn_idx += 1

         # Skip questions with no correct turns
         if not answer_turn_ids:
            skipped += 1
            continue

         relevant_ids = answer_turn_ids
         top_k = 5  # Match RMM paper: Top-K=5 without reranker
      else:
         # One doc per session — user turns joined
         for session, sess_id in zip(sessions, session_ids):
            user_turns = [t["content"] for t in session if t["role"] == "user"]
            text = "\n".join(user_turns)
            if text.strip():
               engine.add(sess_id, text)

         relevant_ids = set(entry["answer_session_ids"])
         top_k = max(ks)

      # Query
      result = engine.query(question, top_k=top_k)
      retrieved_ids = [r["id"] for r in result.get("results", [])]

      # Score
      for k in ks:
         metrics[f"recall_any@{k}"].append(recall_any_at_k(retrieved_ids, relevant_ids, k))
         metrics[f"ndcg@{k}"].append(ndcg_at_k(retrieved_ids, relevant_ids, k))

      # Progress
      evaluated = len(metrics["recall_any@5"]) if "recall_any@5" in metrics else len(metrics["recall_any@3"])
      if (i + 1) % 10 == 0 or i == total - 1:
         elapsed = time.time() - t0
         r5_key = "recall_any@5" if "recall_any@5" in metrics else "recall_any@3"
         r5 = sum(metrics[r5_key]) / len(metrics[r5_key]) if metrics[r5_key] else 0
         skip_label = f"  skipped={skipped}" if skipped else ""
         print(
            f"  [{i + 1:4}/{total}] R@5={r5:.3f}  "
            f"elapsed={elapsed:.0f}s  "
            f"avg={elapsed / (i + 1):.1f}s/q{skip_label}",
            file=sys.stderr,
         )

   # Aggregate
   results = {}
   for key, values in metrics.items():
      results[key] = sum(values) / len(values) if values else 0.0
   evaluated = len(metrics[f"recall_any@{ks[0]}"])
   results["total_questions"] = total
   results["evaluated"] = evaluated
   results["skipped"] = skipped
   results["granularity"] = granularity
   results["turn_scoring"] = turn_scoring if granularity == "turn" else "n/a"
   results["top_k"] = 5 if granularity == "turn" else max(ks)
   results["elapsed_seconds"] = time.time() - t0
   return results


# =============================================================================
# LoCoMo Benchmark
# =============================================================================


def extract_locomo_evidence_ids(evidence, granularity):
   """Convert evidence dialog IDs to the expected format."""
   import re

   if granularity == "dialog":
      return set(evidence)
   else:
      sessions = set()
      for eid in evidence:
         match = re.match(r"D(\d+):", eid)
         if match:
            sessions.add(f"session_{match.group(1)}")
      return sessions


def run_locomo(engine, dataset_path, limit=0, granularity="dialog"):
   """Run LoCoMo benchmark. Returns metrics dict."""
   with open(dataset_path) as f:
      data = json.load(f)

   # LoCoMo format: list of entries, each with 'conversation' and 'qa' keys
   if isinstance(data, dict):
      entries = list(data.values())
   else:
      entries = data

   if limit > 0:
      entries = entries[:limit]

   all_recall = []
   per_category = {}
   total_qa = 0
   t0 = time.time()

   for conv_idx, entry in enumerate(entries):
      # LoCoMo nests sessions under 'conversation' key
      conv = entry.get("conversation", entry)

      # Extract sessions
      sessions = []
      session_num = 1
      while True:
         key = f"session_{session_num}"
         if key not in conv:
            break
         sessions.append(
            {
               "session_num": session_num,
               "date": conv.get(f"session_{session_num}_date_time", ""),
               "dialogs": conv[key],
            }
         )
         session_num += 1

      if not sessions:
         continue

      engine.reset()

      # Ingest
      for sess in sessions:
         if granularity == "dialog":
            for d in sess["dialogs"]:
               dia_id = d.get("dia_id", f"D{sess['session_num']}:?")
               speaker = d.get("speaker", "?")
               text = d.get("text", "")
               doc = f'{speaker} said, "{text}"'
               engine.add(dia_id, doc)
         else:
            texts = []
            for d in sess["dialogs"]:
               speaker = d.get("speaker", "?")
               text = d.get("text", "")
               texts.append(f'{speaker} said, "{text}"')
            doc = "\n".join(texts)
            engine.add(f"session_{sess['session_num']}", doc)

      # Evaluate QA pairs — LoCoMo uses 'qa' key
      qa_pairs = entry.get("qa", entry.get("QA", entry.get("qa_pairs", [])))
      for qa in qa_pairs:
         question = qa.get("question", "")
         evidence = qa.get("evidence", [])
         category = str(qa.get("category", "unknown"))

         if not question or not evidence:
            continue

         result = engine.query(question, top_k=10)
         retrieved_ids = [r["id"] for r in result.get("results", [])]

         evidence_set = extract_locomo_evidence_ids(evidence, granularity)
         recall = compute_fraction_recall(retrieved_ids, evidence_set)

         all_recall.append(recall)
         per_category.setdefault(category, []).append(recall)
         total_qa += 1

      if (conv_idx + 1) % 2 == 0 or conv_idx == len(entries) - 1:
         avg = sum(all_recall) / len(all_recall) if all_recall else 0
         print(
            f"  [conv {conv_idx + 1}/{len(entries)}] "
            f"QA={total_qa}  avg_recall={avg:.3f}",
            file=sys.stderr,
         )

   elapsed = time.time() - t0
   avg_recall = sum(all_recall) / len(all_recall) if all_recall else 0

   results = {
      "avg_recall": avg_recall,
      "total_qa": total_qa,
      "conversations": len(entries),
      "elapsed_seconds": elapsed,
      "per_category": {},
   }
   for cat, vals in sorted(per_category.items()):
      results["per_category"][cat] = sum(vals) / len(vals) if vals else 0

   return results


def compute_fraction_recall(retrieved_ids, evidence_ids):
   """Fraction of evidence IDs found in retrieved."""
   if not evidence_ids:
      return 1.0
   found = sum(1 for eid in evidence_ids if eid in set(retrieved_ids))
   return found / len(evidence_ids)


# =============================================================================
# ConvoMem Benchmark
# =============================================================================


def run_convomem(engine, dataset_path, limit=100):
   """Run ConvoMem benchmark. Returns metrics dict."""
   dataset_dir = Path(dataset_path)

   # Load evidence items from JSON files in the directory
   items = []
   if dataset_dir.is_file():
      with open(dataset_dir) as f:
         data = json.load(f)
      if "evidence_items" in data:
         items = data["evidence_items"]
      elif isinstance(data, list):
         items = data
   else:
      # Directory of JSON files
      for json_file in sorted(dataset_dir.glob("**/*.json")):
         with open(json_file) as f:
            data = json.load(f)
         if "evidence_items" in data:
            items.extend(data["evidence_items"])
         elif isinstance(data, list):
            items.extend(data)
         if len(items) >= limit:
            break

   items = items[:limit]
   if not items:
      print("  No ConvoMem items found.", file=sys.stderr)
      return {"avg_recall": 0, "total_items": 0}

   all_recall = []
   t0 = time.time()

   for i, item in enumerate(items):
      question = item.get("question", "")
      conversations = item.get("conversations", [])
      evidence_messages = item.get("message_evidences", [])
      evidence_texts = [e["text"] for e in evidence_messages]

      if not question or not conversations:
         continue

      engine.reset()

      # Ingest: one doc per message
      msg_idx = 0
      msg_texts = []
      for conv in conversations:
         for msg in conv.get("messages", []):
            text = msg.get("text", "")
            engine.add(f"msg_{msg_idx}", text)
            msg_texts.append(text)
            msg_idx += 1

      if msg_idx == 0:
         continue

      # Query
      result = engine.query(question, top_k=10)
      retrieved_ids = [r["id"] for r in result.get("results", [])]

      # Map retrieved IDs back to texts
      id_to_text = {f"msg_{j}": msg_texts[j] for j in range(len(msg_texts))}
      retrieved_texts = [id_to_text.get(rid, "") for rid in retrieved_ids]

      # Partial recall via substring matching
      recall = partial_recall(retrieved_texts, evidence_texts, k=10)
      all_recall.append(recall)

      if (i + 1) % 20 == 0 or i == len(items) - 1:
         avg = sum(all_recall) / len(all_recall) if all_recall else 0
         print(
            f"  [{i + 1:4}/{len(items)}] avg_recall={avg:.3f}",
            file=sys.stderr,
         )

   elapsed = time.time() - t0
   avg_recall = sum(all_recall) / len(all_recall) if all_recall else 0

   return {
      "avg_recall": avg_recall,
      "total_items": len(all_recall),
      "elapsed_seconds": elapsed,
   }


# =============================================================================
# Main
# =============================================================================


def print_results(benchmark_name, results):
   """Pretty-print benchmark results."""
   print(f"\n{'=' * 60}")
   print(f"  DAWN Retrieval Benchmark: {benchmark_name}")
   print(f"{'=' * 60}")

   if "granularity" in results:
      print(f"  Granularity: {results['granularity']}")
   if "top_k" in results:
      print(f"  Top-K:      {results['top_k']}")
   if results.get("turn_scoring", "n/a") != "n/a":
      print(f"  Scoring:    {results['turn_scoring']}")
   if "total_questions" in results:
      print(f"  Questions:  {results['total_questions']}")
   if "evaluated" in results and results.get("skipped", 0) > 0:
      print(f"  Evaluated:  {results['evaluated']} (skipped {results['skipped']} without answer turns)")
   if "total_qa" in results:
      print(f"  QA pairs:   {results['total_qa']}")
   if "total_items" in results:
      print(f"  Items:      {results['total_items']}")
   if "conversations" in results:
      print(f"  Convos:     {results['conversations']}")

   elapsed = results.get("elapsed_seconds", 0)
   print(f"  Time:       {elapsed:.1f}s")
   print(f"{'─' * 60}")

   # Print recall/NDCG metrics
   for key in sorted(results.keys()):
      if key.startswith("recall_") or key.startswith("ndcg"):
         print(f"  {key:20s} {results[key]:.4f}")

   if "avg_recall" in results:
      print(f"  {'avg_recall':20s} {results['avg_recall']:.4f}")

   # Per-category breakdown
   if "per_category" in results:
      print(f"\n  Per-category recall:")
      for cat, val in results["per_category"].items():
         print(f"    {cat:25s} {val:.3f}")

   print(f"{'=' * 60}\n")


def main():
   parser = argparse.ArgumentParser(description="DAWN Retrieval Benchmark Runner")
   parser.add_argument(
      "--binary",
      required=True,
      help="Path to bench_retrieval binary",
   )
   parser.add_argument(
      "--benchmark",
      required=True,
      choices=["longmemeval", "locomo", "convomem"],
      help="Benchmark to run",
   )
   parser.add_argument("--dataset", required=True, help="Path to benchmark dataset")
   parser.add_argument("--provider", default="onnx", help="Embedding provider (default: onnx)")
   parser.add_argument("--model", default="", help="Model name for HTTP providers")
   parser.add_argument("--endpoint", default="", help="Endpoint URL for HTTP providers")
   parser.add_argument("--api-key", default="", help="API key for OpenAI provider")
   parser.add_argument("--limit", type=int, default=0, help="Limit entries (0 = all)")
   parser.add_argument(
      "--granularity",
      default="session",
      choices=["session", "turn", "dialog"],
      help="Retrieval granularity: session (default), turn (academic standard for "
      "LongMemEval), dialog (LoCoMo per-dialog)",
   )
   parser.add_argument(
      "--raw",
      action="store_true",
      help="Disable keyword boosting (raw cosine only, for baseline comparison)",
   )
   parser.add_argument(
      "--turn-scoring",
      default="official",
      choices=["official", "strict"],
      help="Turn-level scoring: official (any turn from answer session, matches "
      "LongMemEval eval code) or strict (only has_answer turns)",
   )
   parser.add_argument("--output", help="Save results JSON to file")

   args = parser.parse_args()

   # Start the C binary
   mode_label = "raw" if args.raw else "hybrid"
   print(f"  Starting bench_retrieval ({args.provider}, {mode_label})...", file=sys.stderr)
   engine = BenchRetrieval(
      args.binary,
      provider=args.provider,
      model=args.model,
      endpoint=args.endpoint,
      api_key=args.api_key,
      raw_mode=args.raw,
   )
   print(
      f"  Ready: {engine.dims} dims, provider={engine.provider}, mode={engine.mode}",
      file=sys.stderr,
   )

   # Run benchmark
   if args.benchmark == "longmemeval":
      gran = "turn" if args.granularity == "turn" else "session"
      results = run_longmemeval(
         engine,
         args.dataset,
         limit=args.limit,
         granularity=gran,
         turn_scoring=args.turn_scoring,
      )
   elif args.benchmark == "locomo":
      results = run_locomo(
         engine,
         args.dataset,
         limit=args.limit,
         granularity=args.granularity,
      )
   elif args.benchmark == "convomem":
      results = run_convomem(engine, args.dataset, limit=args.limit or 100)

   # Print results
   print_results(args.benchmark.upper(), results)

   # Save JSON
   if args.output:
      with open(args.output, "w") as f:
         json.dump(results, f, indent=2)
      print(f"  Results saved to: {args.output}")

   # Cleanup
   engine.quit()


if __name__ == "__main__":
   main()
