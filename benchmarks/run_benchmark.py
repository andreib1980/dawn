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
import re
import subprocess
import sys
import time
from pathlib import Path


# =============================================================================
# Helpers
# =============================================================================


def split_sentences(text, min_len=15):
   """Split text into sentences on .!? boundaries.
   Keeps only sentences >= min_len chars to skip fragments like 'Yes.' or 'Oh!'."""
   parts = re.split(r"(?<=[.!?])\s+", text.strip())
   return [p.strip() for p in parts if len(p.strip()) >= min_len]


# =============================================================================
# Subprocess Wrapper
# =============================================================================


class BenchRetrieval:
   """Manages the bench_retrieval C binary subprocess."""

   def __init__(
      self,
      binary_path,
      provider="onnx",
      model="",
      endpoint="",
      api_key="",
      raw_mode=False,
      temporal_weight=0.0,
      proper_noun_boost=0.0,
      now_override=None,
      session_neighbor_window=0,
      session_neighbor_boost=0.0,
      memory_pipeline=False,
      config_path=None,
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
      if temporal_weight > 0.0:
         cmd += ["--temporal-weight", str(temporal_weight)]
      if proper_noun_boost > 0.0:
         cmd += ["--proper-noun-boost", str(proper_noun_boost)]
      if now_override is not None:
         cmd += ["--now", str(int(now_override))]
      if session_neighbor_window > 0 and session_neighbor_boost > 0.0:
         cmd += ["--session-neighbor-window", str(session_neighbor_window),
                 "--session-neighbor-boost", str(session_neighbor_boost)]
      if memory_pipeline:
         cmd += ["--memory-pipeline"]
         if config_path:
            cmd += ["--config", config_path]

      # text=False / no bufsize: _read_json_line reads raw bytes via os.read
      # off the stdout fd directly, doing line splitting in Python. Mixing
      # select(fd) with TextIOWrapper.readline() deadlocks when the C side
      # flushes multiple lines at once. stdin stays binary too — _send
      # encodes JSON to bytes before writing.
      # stderr=DEVNULL so a never-drained pipe can't fill up and block the
      # child on a stderr write. Anything bench_retrieval logs that we care
      # about goes to stdout (OLOG INFO/WARN target stdout).
      self.proc = subprocess.Popen(
         cmd,
         stdin=subprocess.PIPE,
         stdout=subprocess.PIPE,
         stderr=subprocess.DEVNULL,
         bufsize=0,
      )
      self._stdout_buf = b""

      # Read ready message — skip OLOG preamble lines (logging is on stdout).
      # Time-based so verbose startup logging never exhausts a fixed line cap.
      self.ready_info = self._read_json_line(timeout=30.0, what="ready message")
      if not self.ready_info or self.ready_info.get("status") != "ready":
         raise RuntimeError("bench_retrieval failed to start (no ready JSON)")

   def _read_json_line(self, timeout, what):
      """Read lines from bench stdout until one parses as JSON or we time out.
      Handles arbitrary amounts of interleaved OLOG output without a line cap.

      Uses raw os.read() rather than self.proc.stdout.readline(), because
      Python's BufferedReader/TextIOWrapper has its own user-space buffer:
      mixing select() on the raw fd with readline() on the wrapper causes
      a deadlock when the C side flushes multiple lines at once (the first
      readline pulls all bytes into the wrapper, then select reports the
      kernel pipe as empty until the deadline)."""
      import os, select, time
      deadline = time.monotonic() + timeout
      fd = self.proc.stdout.fileno()
      buf = self._stdout_buf  # bytes buffer carried across calls
      while True:
         # Serve any complete line already in our buffer first.
         nl = buf.find(b"\n")
         while nl >= 0:
            raw, buf = buf[:nl], buf[nl + 1:]
            line = raw.decode("utf-8", errors="replace").strip()
            if line.startswith("{"):
               self._stdout_buf = buf
               return json.loads(line)
            # else: OLOG preamble line — discard and look for another.
            nl = buf.find(b"\n")

         remaining = deadline - time.monotonic()
         if remaining <= 0:
            self._stdout_buf = buf
            raise RuntimeError(f"bench_retrieval: timed out waiting for {what}")
         ready, _, _ = select.select([fd], [], [], remaining)
         if not ready:
            self._stdout_buf = buf
            raise RuntimeError(f"bench_retrieval: timed out waiting for {what}")
         chunk = os.read(fd, 65536)
         if not chunk:
            self._stdout_buf = buf
            raise RuntimeError(f"bench_retrieval exited before sending {what}")
         buf += chunk

   def _send(self, obj):
      """Send a JSON command and read the response, skipping OLOG preamble lines."""
      self.proc.stdin.write((json.dumps(obj) + "\n").encode("utf-8"))
      self.proc.stdin.flush()
      return self._read_json_line(timeout=120.0, what=f"response to {obj.get('cmd')}")

   def add(self, doc_id, text, created_at=None):
      """Optional created_at (Unix seconds) feeds the temporal-scoring boost."""
      cmd = {"cmd": "add", "id": doc_id, "text": text}
      if created_at is not None:
         cmd["created_at"] = int(created_at)
      return self._send(cmd)

   def query(self, text, top_k=10):
      return self._send({"cmd": "query", "text": text, "top_k": top_k})

   def reset(self):
      return self._send({"cmd": "reset"})

   # =========================================================================
   # Memory-pipeline mode commands
   # =========================================================================

   def _send_with_timeout(self, obj, timeout):
      """Like _send but with a configurable timeout for slow operations
      (extraction can take 5-30s per call on Claude Haiku/Sonnet/Opus)."""
      self.proc.stdin.write((json.dumps(obj) + "\n").encode("utf-8"))
      self.proc.stdin.flush()
      return self._read_json_line(timeout=timeout, what=f"response to {obj.get('cmd')}")

   def conv_create(self, title="bench"):
      return self._send({"cmd": "conv_create", "title": title})

   def add_message(self, conv_id, dia_id, role, content):
      return self._send({"cmd": "add_message", "conv_id": conv_id, "dia_id": dia_id,
                         "role": role, "content": content})

   def extract(self, conv_id, session_id, timeout_ms=600000):
      """Trigger extraction; can take seconds-to-minutes depending on model."""
      return self._send_with_timeout(
         {"cmd": "extract", "conv_id": conv_id, "session_id": session_id,
          "timeout_ms": timeout_ms},
         timeout=(timeout_ms / 1000.0) + 30.0)

   def query_memory(self, text, top_k=10):
      return self._send({"cmd": "query_memory", "text": text, "top_k": top_k})

   def reset_memory(self):
      return self._send({"cmd": "reset_memory", "user_id": 1})

   def quit(self):
      self.proc.stdin.write((json.dumps({"cmd": "quit"}) + "\n").encode("utf-8"))
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


def parse_locomo_session_date(s):
   """Parse LoCoMo's 'H:MM am/pm on D Month, YYYY' format → Unix seconds (UTC).
   Returns 0 on failure (chunk gets no temporal boost)."""
   if not s:
      return 0
   import datetime
   try:
      dt = datetime.datetime.strptime(s, "%I:%M %p on %d %B, %Y")
      return int(dt.replace(tzinfo=datetime.timezone.utc).timestamp())
   except (ValueError, TypeError):
      return 0


def run_locomo(engine, dataset_path, limit=0, granularity="dialog", sentence_chunks=False, top_k=10):
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

      # Ingest — pass session timestamp so temporal-query scoring (#3) can
      # boost dialogs from the right month/year ("in summer 2023").
      for sess in sessions:
         session_ts = parse_locomo_session_date(sess["date"])
         if granularity == "dialog":
            for d in sess["dialogs"]:
               dia_id = d.get("dia_id", f"D{sess['session_num']}:?")
               speaker = d.get("speaker", "?")
               text = d.get("text", "")
               if sentence_chunks:
                  sentences = split_sentences(text) or [text]
                  for sent in sentences:
                     engine.add(dia_id, f"[{speaker}] {sent}", created_at=session_ts)
               else:
                  engine.add(dia_id, f'{speaker} said, "{text}"', created_at=session_ts)
         else:
            texts = []
            for d in sess["dialogs"]:
               speaker = d.get("speaker", "?")
               text = d.get("text", "")
               texts.append(f'{speaker} said, "{text}"')
            doc = "\n".join(texts)
            engine.add(f"session_{sess['session_num']}", doc, created_at=session_ts)

      # Evaluate QA pairs — LoCoMo uses 'qa' key
      qa_pairs = entry.get("qa", entry.get("QA", entry.get("qa_pairs", [])))
      for qa in qa_pairs:
         question = qa.get("question", "")
         evidence = qa.get("evidence", [])
         category = str(qa.get("category", "unknown"))

         if not question or not evidence:
            continue

         result = engine.query(question, top_k=top_k)
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
# LoCoMo Memory-Pipeline Benchmark (Phase 1)
# =============================================================================
#
# Drives bench_retrieval through extraction + memory_facts retrieval instead of
# raw-dialog cosine. Methodology:
#
#   - One DAWN conversation per LoCoMo conv (Option A — production-faithful).
#   - Per LoCoMo session: ingest dialogs as messages, then trigger extraction.
#     The high-water-mark cursor in memory_extraction.c filters new messages.
#   - Per QA pair: query_memory and aggregate covered_dia_ids[] across the
#     retrieved facts (each fact's provenance range maps to dia_ids via the
#     bench's in-process dia_id<->msg_id table).
#   - Recall metric: recall_reach = |gold_dia_ids ∩ retrieved_covered_dia_ids|
#     / |gold_dia_ids|. Documented as "retrieval reach", NOT answer support.
#     See atlas/dawn/archive/LOCOMO_CAT3_PROFILING.md "Outcome" section.


def run_locomo_memory(engine, dataset_path, limit=0, top_k=10):
   """Run LoCoMo through extraction + memory-fact retrieval. Returns metrics."""
   with open(dataset_path) as f:
      data = json.load(f)
   if isinstance(data, dict):
      entries = list(data.values())
   else:
      entries = data
   if limit > 0:
      entries = entries[:limit]

   all_recall = []
   per_category = {}
   total_qa = 0
   total_facts_added = 0  # cumulative across all convs (facts_total resets per conv via reset_memory)
   last_conv_facts = 0    # facts in the most-recently-extracted conv
   total_extractions = 0
   total_extract_seconds = 0.0
   t0 = time.time()

   for conv_idx, entry in enumerate(entries):
      conv = entry.get("conversation", entry)

      # Reset per-conv: fresh facts & dia_id map
      engine.reset_memory()

      # Create DAWN conversation
      cresp = engine.conv_create(title=f"locomo_{conv_idx}")
      conv_id = cresp.get("conv_id")
      if not conv_id:
         print(f"  conv {conv_idx}: conv_create failed; skipping", file=sys.stderr)
         continue

      # Determine speakers (LoCoMo has two; first introduced -> user)
      first_speaker = None

      # Iterate sessions: ingest then extract
      session_n = 0
      while True:
         session_n += 1
         key = f"session_{session_n}"
         if key not in conv:
            break
         dialogs = conv[key]
         if not isinstance(dialogs, list):
            continue

         for d in dialogs:
            dia_id = d.get("dia_id", "")
            speaker = d.get("speaker", "?")
            text = d.get("text", "")
            if first_speaker is None:
               first_speaker = speaker
            role = "user" if speaker == first_speaker else "assistant"
            content = f'{speaker} said, "{text}"'
            engine.add_message(conv_id, dia_id, role, content)

         eresp = engine.extract(conv_id, session_id=f"locomo_{conv_idx}_s{session_n}",
                                timeout_ms=600000)
         total_extractions += 1
         total_extract_seconds += eresp.get("duration_ms", 0) / 1000.0
         total_facts_added += eresp.get("facts_added", 0)
         last_conv_facts = eresp.get("facts_total", last_conv_facts)

         print(f"  [conv {conv_idx} sess {session_n}] facts={last_conv_facts} "
               f"+{eresp.get('facts_added', 0)} dur={eresp.get('duration_ms', 0)}ms",
               file=sys.stderr)

      # Score each QA pair via memory query + provenance->dia_id mapping
      qa_pairs = entry.get("qa", entry.get("QA", entry.get("qa_pairs", [])))
      for qa in qa_pairs:
         question = qa.get("question", "")
         evidence = qa.get("evidence", [])
         category = str(qa.get("category", "unknown"))
         if not question or not evidence:
            continue

         # Normalize evidence: split any space-separated multi-IDs (LoCoMo
         # conv 8 has 'D9:1 D4:4 D4:6' as a single string in 3 cat-3 questions)
         flat_evidence = []
         for e in evidence:
            flat_evidence.extend(e.split())
         evidence_set = set(flat_evidence)

         qresp = engine.query_memory(question, top_k=top_k)
         retrieved = qresp.get("results", [])

         # Aggregate covered dia_ids across all retrieved facts
         covered = set()
         for r in retrieved:
            covered.update(r.get("covered_dia_ids", []))

         recall = compute_fraction_recall(list(covered), evidence_set)
         all_recall.append(recall)
         per_category.setdefault(category, []).append(recall)
         total_qa += 1

      avg = sum(all_recall) / len(all_recall) if all_recall else 0
      print(f"  [conv {conv_idx + 1}/{len(entries)}] QA={total_qa} avg_recall_reach={avg:.3f}",
            file=sys.stderr)

   elapsed = time.time() - t0
   results = {
      "mode": "memory-pipeline",
      "granularity": "per_locomo_session",
      "scoring": "recall_reach (retrieval reach via provenance overlap; NOT answer support)",
      "extraction_provider": engine.ready_info.get("extraction_provider", ""),
      "extraction_model": engine.ready_info.get("extraction_model", ""),
      "avg_recall_reach": sum(all_recall) / len(all_recall) if all_recall else 0,
      "total_qa": total_qa,
      "conversations": len(entries),
      "total_facts_extracted": total_facts_added,
      "total_extractions": total_extractions,
      "extraction_total_seconds": total_extract_seconds,
      "elapsed_seconds": elapsed,
      "per_category": {},
   }
   for cat, vals in sorted(per_category.items()):
      results["per_category"][cat] = sum(vals) / len(vals) if vals else 0
   return results


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
   parser.add_argument(
      "--temporal-weight",
      type=float,
      default=0.0,
      help="Temporal-boost weight (0 = disabled, default). Try 0.10–0.30 for "
      "LoCoMo cat-3 lift. Requires datasets that pass timestamps (LoCoMo).",
   )
   parser.add_argument(
      "--now",
      type=int,
      default=None,
      help="Pin 'now' (Unix seconds) for relative expressions in queries. For LoCoMo, "
      "use ~1707000000 (early 2024) to anchor 'last week'/'recently' to the dataset era.",
   )
   parser.add_argument(
      "--proper-noun-boost",
      type=float,
      default=0.0,
      dest="proper_noun_boost",
      help="Extra keyword-match weight for capitalized query words (proper nouns/names). "
      "Try 0.5–2.0. Stacks with --temporal-weight.",
   )
   parser.add_argument(
      "--sentence-chunks",
      action="store_true",
      dest="sentence_chunks",
      help="Split dialog turns into individual sentences before indexing (LoCoMo only). "
      "Reduces embedding dilution for single-fact turns.",
   )
   parser.add_argument(
      "--top-k",
      type=int,
      default=10,
      dest="top_k",
      help="Number of results to retrieve per query (default: 10). Higher values help "
      "diagnose whether evidence is present but poorly ranked (LoCoMo cat-3).",
   )
   parser.add_argument(
      "--session-neighbor-window",
      type=int,
      default=0,
      dest="session_neighbor_window",
      help="Session-neighbor anchor window: top-N items by cosine become anchors. "
      "Doc IDs split on ':'; chunks sharing an anchor's prefix get a boost. 0 = off. "
      "Try 3–10. LoCoMo dialog only — datasets without ':' in IDs are no-ops.",
   )
   parser.add_argument(
      "--session-neighbor-boost",
      type=float,
      default=0.0,
      dest="session_neighbor_boost",
      help="Additive score for chunks matching an anchor session prefix. Try 0.05–0.20.",
   )
   parser.add_argument(
      "--memory-pipeline",
      action="store_true",
      dest="memory_pipeline",
      help="LoCoMo only: drive extraction + memory_facts retrieval instead of "
      "raw-dialog cosine. Loads dawn.toml for extraction provider/model. "
      "Reports recall_reach (retrieval reach via provenance overlap; NOT "
      "answer support — see atlas/dawn/archive/LOCOMO_CAT3_PROFILING.md).",
   )
   parser.add_argument(
      "--config",
      default="./dawn.toml",
      dest="config_path",
      help="Path to dawn.toml (default ./dawn.toml). Used in --memory-pipeline mode "
      "to load extraction_provider/extraction_model.",
   )
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
      temporal_weight=args.temporal_weight,
      proper_noun_boost=args.proper_noun_boost,
      now_override=args.now,
      session_neighbor_window=args.session_neighbor_window,
      session_neighbor_boost=args.session_neighbor_boost,
      memory_pipeline=args.memory_pipeline,
      config_path=args.config_path if args.memory_pipeline else None,
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
      if args.memory_pipeline:
         results = run_locomo_memory(
            engine,
            args.dataset,
            limit=args.limit,
            top_k=args.top_k,
         )
      else:
         results = run_locomo(
            engine,
            args.dataset,
            limit=args.limit,
            granularity=args.granularity,
            sentence_chunks=args.sentence_chunks,
            top_k=args.top_k,
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
