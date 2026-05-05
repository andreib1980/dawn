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
import hashlib
import json
import math
import os
import re
import subprocess
import sys
import time
from pathlib import Path

# Bumped when the snapshot file format changes in an incompatible way (e.g.,
# bench DDL adds a non-default column or the dia_map JSON shape changes).
# Embedded into cache keys so old snapshots auto-invalidate on upgrade.
SNAPSHOT_FORMAT_VERSION = 1


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
      extraction_provider="",
      extraction_model="",
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
         if extraction_provider:
            cmd += ["--extraction-provider", extraction_provider]
         if extraction_model:
            cmd += ["--extraction-model", extraction_model]
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

   def conv_create(self, title="bench", anchor_date=None):
      """Optional anchor_date (Unix seconds) seeds conversations.anchor_date so
      memory_extraction.c can resolve relative phrases against the LoCoMo
      session date rather than bench wall-clock."""
      cmd = {"cmd": "conv_create", "title": title}
      if anchor_date is not None:
         cmd["anchor_date"] = int(anchor_date)
      return self._send(cmd)

   def add_message(self, conv_id, dia_id, role, content):
      return self._send({"cmd": "add_message", "conv_id": conv_id, "dia_id": dia_id,
                         "role": role, "content": content})

   def extract(self, conv_id, session_id, timeout_ms=600000, anchor_date=None):
      """Trigger extraction; can take seconds-to-minutes depending on model.
      Optional anchor_date overrides the conversation's anchor for this
      extraction — LoCoMo sessions span months, so each session's anchor
      should be its own session_X_date_time, not the conversation's first."""
      cmd = {"cmd": "extract", "conv_id": conv_id, "session_id": session_id,
             "timeout_ms": timeout_ms}
      if anchor_date is not None:
         cmd["anchor_date"] = int(anchor_date)
      return self._send_with_timeout(cmd, timeout=(timeout_ms / 1000.0) + 30.0)

   def query_memory(self, text, top_k=10):
      return self._send({"cmd": "query_memory", "text": text, "top_k": top_k})

   def reset_memory(self):
      return self._send({"cmd": "reset_memory", "user_id": 1})

   def snapshot_save(self, db_path, map_path):
      """Save current bench DB + dia_map to the given paths."""
      return self._send_with_timeout(
         {"cmd": "snapshot_save", "db_path": str(db_path), "map_path": str(map_path)},
         timeout=120.0)

   def snapshot_load(self, db_path, map_path):
      """Restore bench DB + dia_map from the given paths.  Replaces all state
      under user_id=1 (facts, entities, conversations, messages, dia_map)."""
      return self._send_with_timeout(
         {"cmd": "snapshot_load", "db_path": str(db_path), "map_path": str(map_path)},
         timeout=120.0)

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
# LLM Entailment Judge (Phase 2.2)
# =============================================================================
#
# A diagnostic gate over recall_reach.  recall_reach answers "do retrieved facts
# cover the gold dia_ids?" — geometric overlap.  recall_entailment answers
# "could a careful reader derive the gold answer from the retrieved facts?" —
# the actually-useful signal for whether retrieval is doing its job.
#
# Implementation: send (question, gold_answer, retrieved fact texts) to a
# strong LLM judge, ask for a strict YES/NO.  Cache results to disk keyed on
# everything that affects the answer, so prompt-tuning iterations are free.


def _read_anthropic_key_from_secrets(secrets_path):
   """Cheap regex parse of secrets.toml for the claude_api_key field.  We avoid
   tomli/tomllib because the host Python is 3.10 and we don't want a new
   bench-only dependency."""
   try:
      with open(secrets_path) as f:
         body = f.read()
   except (FileNotFoundError, PermissionError):
      return None
   m = re.search(r'^\s*claude_api_key\s*=\s*"([^"]+)"', body, re.MULTILINE)
   return m.group(1) if m else None


class JudgeCache:
   """Disk-persistent cache for entailment judgments.  Single JSON file keyed
   by SHA-256 of (judge_model, prompt_version, question, gold_answer,
   sorted retrieved fact texts).  Prompt-version is part of the key so a
   prompt edit auto-invalidates old judgments instead of returning stale
   scores under a new rubric."""

   def __init__(self, cache_path):
      self.path = Path(cache_path) if cache_path else None
      self.entries = {}
      self.dirty = False
      if self.path and self.path.exists():
         try:
            with open(self.path) as f:
               self.entries = json.load(f)
         except (json.JSONDecodeError, OSError) as exc:
            print(f"  judge_cache: failed to load {self.path} ({exc}); starting empty",
                  file=sys.stderr)
            self.entries = {}

   @staticmethod
   def make_key(judge_model, prompt_version, question, gold_answer, fact_texts):
      h = hashlib.sha256()
      h.update(f"v{prompt_version}".encode("utf-8"))
      h.update(b"\x1f")
      h.update((judge_model or "").encode("utf-8"))
      h.update(b"\x1f")
      h.update((question or "").encode("utf-8"))
      h.update(b"\x1f")
      h.update((gold_answer or "").encode("utf-8"))
      h.update(b"\x1f")
      # Sort fact texts so retrieval-order changes don't fragment the cache.
      for ft in sorted(fact_texts):
         h.update((ft or "").encode("utf-8"))
         h.update(b"\x1e")
      return h.hexdigest()[:24]

   def get(self, key):
      return self.entries.get(key)

   def put(self, key, score, raw_response):
      self.entries[key] = {"score": score, "raw": raw_response}
      self.dirty = True

   def flush(self):
      if not self.path or not self.dirty:
         return
      self.path.parent.mkdir(parents=True, exist_ok=True)
      tmp = self.path.with_suffix(self.path.suffix + ".tmp")
      with open(tmp, "w") as f:
         json.dump(self.entries, f, indent=2, sort_keys=True)
      os.replace(tmp, self.path)
      self.dirty = False


# Bumped when the entailment prompt or response format changes in a way that
# would make old cached judgments invalid under the new rubric.
ENTAILMENT_PROMPT_VERSION = 1

ENTAILMENT_SYSTEM = (
   "You evaluate a memory retrieval system. Given a question, the correct "
   "answer, and a set of retrieved memory facts, decide whether the facts "
   "contain enough information for a careful reader to derive the correct "
   "answer. Respond with exactly one word: YES or NO."
)

ENTAILMENT_USER_TEMPLATE = """\
Question: {question}

Correct answer: {gold_answer}

Retrieved facts:
{numbered_facts}

Do the retrieved facts contain enough information for a careful reader to \
derive the correct answer? Be strict: if facts are only tangentially related \
or are missing a key detail required by the answer, respond NO. Respond with \
exactly one word: YES or NO."""


def _anthropic_call(model, system, user_prompt, api_key, temperature=0.0,
                    max_tokens=64, timeout=60.0):
   """Direct HTTPS POST to the Anthropic Messages API.  Returns the first text
   block from the response.  No SDK dependency — we only need this one shape
   of call.  Caller is responsible for catching exceptions (timeouts, 4xx,
   5xx) and deciding how to score."""
   import urllib.request
   payload = json.dumps({
      "model": model,
      "max_tokens": max_tokens,
      "temperature": temperature,
      "system": system,
      "messages": [{"role": "user", "content": user_prompt}],
   }).encode("utf-8")
   req = urllib.request.Request(
      "https://api.anthropic.com/v1/messages",
      data=payload,
      headers={
         "x-api-key": api_key,
         "anthropic-version": "2023-06-01",
         "content-type": "application/json",
      },
      method="POST")
   with urllib.request.urlopen(req, timeout=timeout) as resp:
      body = resp.read().decode("utf-8")
   data = json.loads(body)
   blocks = data.get("content", [])
   for b in blocks:
      if b.get("type") == "text":
         return b.get("text", "")
   return ""


def _parse_yes_no(raw):
   """Strict YES/NO parser shared by entailment and correctness judges."""
   if not raw:
      return 0.0
   tok = raw.strip().upper()
   tok = re.sub(r"[^A-Z]", "", tok)
   if tok.startswith("YES"):
      return 1.0
   if tok.startswith("NO"):
      return 0.0
   return 0.0


class EntailmentJudge:
   """Anthropic-API-backed entailment scorer.  Caches every call; flush() at
   the end persists the cache so the next run is free.  Failures are logged
   and treated as score=0.0 (does not entail) — judge errors should not look
   like passes."""

   def __init__(self, model, api_key, cache, temperature=0.0, max_facts=20):
      self.model = model
      self.api_key = api_key
      self.cache = cache
      self.temperature = temperature
      self.max_facts = max_facts  # cap to keep prompt bounded; LoCoMo top-K=10 typically
      self.calls_made = 0
      self.cache_hits = 0
      self.errors = 0

   def _build_prompt(self, question, gold_answer, fact_texts):
      texts = list(fact_texts)[:self.max_facts]
      if not texts:
         numbered = "(none)"
      else:
         numbered = "\n".join(f"{i + 1}. {t}" for i, t in enumerate(texts))
      return ENTAILMENT_USER_TEMPLATE.format(
         question=question, gold_answer=gold_answer, numbered_facts=numbered)

   @staticmethod
   def _parse_score(raw):
      """Strict YES/NO parser.  We intentionally do not accept fuzzy/partial
      verdicts — the gate is binary."""
      return _parse_yes_no(raw)

   def judge(self, question, gold_answer, fact_texts):
      """Returns (score, was_cached).  score in {0.0, 1.0}."""
      key = JudgeCache.make_key(
         self.model, ENTAILMENT_PROMPT_VERSION, question, gold_answer, fact_texts)
      cached = self.cache.get(key)
      if cached is not None:
         self.cache_hits += 1
         return cached.get("score", 0.0), True

      user_prompt = self._build_prompt(question, gold_answer, fact_texts)
      try:
         raw = _anthropic_call(
            self.model, ENTAILMENT_SYSTEM, user_prompt, self.api_key,
            temperature=self.temperature, max_tokens=8)
      except Exception as exc:
         self.errors += 1
         print(f"  judge: API error ({exc}); scoring 0.0", file=sys.stderr)
         self.cache.put(key, 0.0, f"ERROR: {exc}")
         return 0.0, False

      score = self._parse_score(raw)
      self.calls_made += 1
      self.cache.put(key, score, raw)
      return score, False


def _resolve_anthropic_key(args):
   """Shared API-key resolution chain for judge + generator."""
   return (args.judge_api_key
           or os.environ.get("ANTHROPIC_API_KEY")
           or _read_anthropic_key_from_secrets("./secrets.toml"))


def make_entailment_judge(args):
   """Build an EntailmentJudge from CLI args + secrets.toml fallback.  Returns
   None if --judge-model is not set (entailment scoring opts in)."""
   if not args.judge_model:
      return None
   if args.judge_provider != "anthropic":
      print(f"  judge: only --judge-provider=anthropic is supported (got "
            f"'{args.judge_provider}'); disabling entailment", file=sys.stderr)
      return None
   api_key = _resolve_anthropic_key(args)
   if not api_key:
      print("  judge: no API key — pass --judge-api-key, set ANTHROPIC_API_KEY, "
            "or fill claude_api_key in ./secrets.toml. Entailment disabled.",
            file=sys.stderr)
      return None
   cache_path = None
   if args.cache_dir and not args.no_judge_cache:
      cache_path = Path(args.cache_dir) / "judgements.json"
   cache = JudgeCache(cache_path)
   return EntailmentJudge(
      model=args.judge_model,
      api_key=api_key,
      cache=cache,
      temperature=args.judge_temperature,
      max_facts=args.judge_max_facts,
   )


# =============================================================================
# Generate-and-Judge (Phase 2.3)
# =============================================================================
#
# The leader-comparable metric.  Two LLM calls per QA:
#   1. Generator: question + retrieved facts → candidate answer
#   2. CorrectnessJudge: question + gold + candidate → strict YES/NO
#
# This is what published memory systems (ByteRover, MemMachine, etc.) report.
# `recall_entailment` (Phase 2.2) tells us "does retrieval surface enough
# info?", which gates everything downstream.  `recall_generation` tells us
# whether DAWN's actual answer pipeline can produce the right answer from
# what retrieval surfaced — what users actually experience.

GENERATOR_PROMPT_VERSION = 1
CORRECTNESS_PROMPT_VERSION = 1

GENERATOR_SYSTEM = (
   "You answer the user's question using only the provided memory facts. "
   "Be concise — one short sentence when possible. If the facts do not "
   "contain a clear answer, respond exactly: I don't know."
)

GENERATOR_USER_TEMPLATE = """\
Memory facts:
{numbered_facts}

Question: {question}"""

CORRECTNESS_SYSTEM = (
   "You judge whether a generated answer is factually equivalent to a "
   "correct answer. Focus on the substantive content — date, name, number, "
   "or fact — not on phrasing or formatting. Respond with exactly one word: "
   "YES or NO."
)

CORRECTNESS_USER_TEMPLATE = """\
Question: {question}

Correct answer: {gold_answer}

Generated answer: {generated_answer}

Is the generated answer factually equivalent to the correct answer? \
YES if both convey the same essential answer (paraphrase, formatting, or \
extra context is fine). NO if the generated answer contradicts, omits a \
key required detail, or is non-committal when the correct answer is concrete.

Edge case: if the correct answer is "Not mentioned" / "No information" / \
similar, then a generated "I don't know" or refusal counts as YES; a \
generated specific claim counts as NO.

Respond with exactly one word: YES or NO."""


class AnswerGenerator:
   """Synthesizes a candidate answer from (question, retrieved facts) using a
   single Anthropic Messages call.  Caches every generation; an identical
   (model, prompt version, question, retrieved facts) tuple is free on rerun."""

   def __init__(self, model, api_key, cache, temperature=0.0, max_tokens=200,
                max_facts=20):
      self.model = model
      self.api_key = api_key
      self.cache = cache
      self.temperature = temperature
      self.max_tokens = max_tokens
      self.max_facts = max_facts
      self.calls_made = 0
      self.cache_hits = 0
      self.errors = 0

   def _build_prompt(self, question, fact_texts):
      texts = list(fact_texts)[:self.max_facts]
      if not texts:
         numbered = "(none)"
      else:
         numbered = "\n".join(f"{i + 1}. {t}" for i, t in enumerate(texts))
      return GENERATOR_USER_TEMPLATE.format(
         question=question, numbered_facts=numbered)

   def generate(self, question, fact_texts):
      """Returns (answer_text, was_cached).  Empty string on hard failure
      (caller can decide whether to count that as a wrong answer or skip)."""
      # Reuse JudgeCache.make_key shape — gold_answer slot is unused, pass "".
      key = JudgeCache.make_key(
         self.model, GENERATOR_PROMPT_VERSION, question, "", fact_texts)
      cached = self.cache.get(key)
      if cached is not None:
         self.cache_hits += 1
         return cached.get("raw", ""), True

      user_prompt = self._build_prompt(question, fact_texts)
      try:
         raw = _anthropic_call(
            self.model, GENERATOR_SYSTEM, user_prompt, self.api_key,
            temperature=self.temperature, max_tokens=self.max_tokens)
      except Exception as exc:
         self.errors += 1
         print(f"  generator: API error ({exc}); empty answer",
               file=sys.stderr)
         # Cache empty raw so reruns short-circuit cleanly through the
         # correctness judge's empty-generation fast-path; the diagnostic
         # error message is logged above.
         self.cache.put(key, 0.0, "")
         return "", False

      self.calls_made += 1
      self.cache.put(key, 0.0, raw)
      return raw, False


class CorrectnessJudge:
   """Binary correctness scorer for generated answers.  Same JudgeCache shape
   as EntailmentJudge but a separate prompt version + cache file so the two
   metrics never share a verdict."""

   def __init__(self, model, api_key, cache, temperature=0.0):
      self.model = model
      self.api_key = api_key
      self.cache = cache
      self.temperature = temperature
      self.calls_made = 0
      self.cache_hits = 0
      self.errors = 0

   def _build_prompt(self, question, gold_answer, generated_answer):
      return CORRECTNESS_USER_TEMPLATE.format(
         question=question, gold_answer=gold_answer,
         generated_answer=generated_answer)

   def judge(self, question, gold_answer, generated_answer):
      """Returns (score, was_cached).  score in {0.0, 1.0}."""
      # Cache key: hash a fact-list of length 1 containing the generated answer
      # so JudgeCache.make_key can be reused without a new shape.  Order is
      # stable since it's a single element.
      key = JudgeCache.make_key(
         self.model, CORRECTNESS_PROMPT_VERSION, question, gold_answer,
         [generated_answer or ""])
      cached = self.cache.get(key)
      if cached is not None:
         self.cache_hits += 1
         return cached.get("score", 0.0), True

      # Empty generated answer is automatically wrong unless gold is also
      # empty (which doesn't happen in LoCoMo).  Skip the API round-trip.
      if not generated_answer or not generated_answer.strip():
         self.cache.put(key, 0.0, "(empty generation)")
         return 0.0, False

      user_prompt = self._build_prompt(question, gold_answer, generated_answer)
      try:
         raw = _anthropic_call(
            self.model, CORRECTNESS_SYSTEM, user_prompt, self.api_key,
            temperature=self.temperature, max_tokens=8)
      except Exception as exc:
         self.errors += 1
         print(f"  correctness: API error ({exc}); scoring 0.0",
               file=sys.stderr)
         self.cache.put(key, 0.0, f"ERROR: {exc}")
         return 0.0, False

      score = _parse_yes_no(raw)
      self.calls_made += 1
      self.cache.put(key, score, raw)
      return score, False


def make_generator(args):
   """Build an AnswerGenerator if --generator-model is set, else None.
   Reuses the judge API key chain so a single ANTHROPIC_API_KEY covers both."""
   if not args.generator_model:
      return None
   api_key = _resolve_anthropic_key(args)
   if not api_key:
      print("  generator: no API key — disabling generate-and-judge",
            file=sys.stderr)
      return None
   cache_path = None
   if args.cache_dir and not args.no_judge_cache:
      cache_path = Path(args.cache_dir) / "generations.json"
   cache = JudgeCache(cache_path)
   return AnswerGenerator(
      model=args.generator_model,
      api_key=api_key,
      cache=cache,
      temperature=args.generator_temperature,
      max_tokens=args.generator_max_tokens,
      max_facts=args.judge_max_facts,
   )


def make_correctness_judge(args):
   """Build a CorrectnessJudge from --judge-model.  Only used when a generator
   is also configured (generate-and-judge requires both)."""
   if not args.judge_model:
      return None
   api_key = _resolve_anthropic_key(args)
   if not api_key:
      return None
   cache_path = None
   if args.cache_dir and not args.no_judge_cache:
      cache_path = Path(args.cache_dir) / "correctness.json"
   cache = JudgeCache(cache_path)
   return CorrectnessJudge(
      model=args.judge_model,
      api_key=api_key,
      cache=cache,
      temperature=args.judge_temperature,
   )


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
#     See atlas/dawn/memory/LOCOMO_CAT3_PROFILING.md "Outcome" section.


def _locomo_conv_content_hash(conv):
   """Stable hash over the conversation's session content.  Used as part of the
   snapshot cache key so dataset edits invalidate old caches automatically.
   Iterates session_1, session_2, ... in numeric order; per-dialog tuple is
   (dia_id, speaker, text)."""
   h = hashlib.sha256()
   session_n = 0
   while True:
      session_n += 1
      key = f"session_{session_n}"
      if key not in conv:
         break
      dialogs = conv[key]
      if not isinstance(dialogs, list):
         continue
      h.update(key.encode("utf-8"))
      h.update(b"\x1f")
      for d in dialogs:
         dia_id = d.get("dia_id", "") or ""
         speaker = d.get("speaker", "") or ""
         text = d.get("text", "") or ""
         h.update(dia_id.encode("utf-8"))
         h.update(b"\x1f")
         h.update(speaker.encode("utf-8"))
         h.update(b"\x1f")
         h.update(text.encode("utf-8"))
         h.update(b"\x1e")
   return h.hexdigest()


def _snapshot_cache_paths(cache_dir, engine, conv_idx, conv):
   """Build (db_path, map_path, key) for a single LoCoMo conversation.
   Cache key includes everything that would change the extracted state:
     - snapshot format version
     - extraction provider/model (LLM-driven)
     - embedding provider + dims (embeddings stored alongside facts)
     - conv index + content hash
   Stored under: cache_dir/{key}.db and cache_dir/{key}.json"""
   key_input = "\x1f".join([
      f"v{SNAPSHOT_FORMAT_VERSION}",
      engine.ready_info.get("extraction_provider", ""),
      engine.ready_info.get("extraction_model", ""),
      engine.provider,
      str(engine.dims),
      str(conv_idx),
      _locomo_conv_content_hash(conv),
   ])
   key = hashlib.sha256(key_input.encode("utf-8")).hexdigest()[:16]
   db_path = Path(cache_dir) / f"{key}.db"
   map_path = Path(cache_dir) / f"{key}.json"
   return db_path, map_path, key


def run_locomo_memory(engine, dataset_path, limit=0, top_k=10, cache_dir=None,
                      no_cache=False, judge=None, generator=None,
                      correctness_judge=None):
   """Run LoCoMo through extraction + memory-fact retrieval. Returns metrics.

   If cache_dir is set and no_cache is False, per-conversation extraction state
   (facts/entities/conversations/messages + dia_map) is cached on first run via
   snapshot_save, and reused on subsequent runs via snapshot_load.  Cache keys
   incorporate extraction model + embedding provider + dataset content hash, so
   any change to those auto-invalidates old entries.

   When judge is set (an EntailmentJudge), each QA pair gets a strict YES/NO
   entailment verdict and recall_entailment is reported alongside recall_reach.

   When both generator (AnswerGenerator) and correctness_judge (CorrectnessJudge)
   are set, each QA pair also runs generate-and-judge: the generator synthesizes
   an answer from retrieved facts, the correctness judge compares it to gold,
   and recall_generation is reported.  This is the leader-comparable metric."""
   with open(dataset_path) as f:
      data = json.load(f)
   if isinstance(data, dict):
      entries = list(data.values())
   else:
      entries = data
   if limit > 0:
      entries = entries[:limit]

   if cache_dir and not no_cache:
      Path(cache_dir).mkdir(parents=True, exist_ok=True)

   all_recall = []
   all_entailment = []
   all_generation = []
   per_category = {}
   per_category_entailment = {}
   per_category_generation = {}
   total_qa = 0
   gen_and_judge = generator is not None and correctness_judge is not None
   total_facts_added = 0  # cumulative across all convs (facts_total resets per conv via reset_memory)
   last_conv_facts = 0    # facts in the most-recently-extracted conv
   total_extractions = 0
   total_extract_seconds = 0.0
   total_cache_hits = 0
   total_cache_misses = 0
   t0 = time.time()

   for conv_idx, entry in enumerate(entries):
      conv = entry.get("conversation", entry)

      # Cache lookup: skip extraction entirely if a snapshot for this exact
      # (extraction model, embedding provider, conv content) tuple exists.
      cache_hit = False
      db_path = map_path = None
      cache_key = None
      if cache_dir and not no_cache:
         db_path, map_path, cache_key = _snapshot_cache_paths(
            cache_dir, engine, conv_idx, conv)
         if db_path.exists() and map_path.exists():
            try:
               lresp = engine.snapshot_load(db_path, map_path)
               if lresp.get("status") == "ok":
                  cache_hit = True
                  last_conv_facts = lresp.get("facts", last_conv_facts)
                  total_cache_hits += 1
                  print(f"  [conv {conv_idx}] cache HIT key={cache_key} "
                        f"facts={lresp.get('facts', 0)} "
                        f"entities={lresp.get('entities', 0)} "
                        f"messages={lresp.get('messages', 0)}",
                        file=sys.stderr)
            except Exception as exc:
               print(f"  [conv {conv_idx}] snapshot_load failed ({exc}); "
                     f"falling back to extraction", file=sys.stderr)

      if not cache_hit:
         total_cache_misses += 1
         # Reset per-conv: fresh facts & dia_id map
         engine.reset_memory()

         # Pre-parse all session dates once (cheap polish: parse_locomo_session_date
         # would otherwise fire on session_1 twice — at conv_create and again on
         # session_1's extract pass).  Build a {n: ts_or_None} map.
         session_dates = {}
         _n = 0
         while True:
            _n += 1
            if f"session_{_n}" not in conv:
               break
            session_dates[_n] = parse_locomo_session_date(
               conv.get(f"session_{_n}_date_time", ""))

         # Seed the conversation's anchor with session_1's date if available;
         # each per-session extract below also overrides with its own date so
         # the actual relative-phrase anchor matches the session at extract time.
         first_session_ts = session_dates.get(1)

         # Create DAWN conversation
         cresp = engine.conv_create(title=f"locomo_{conv_idx}",
                                    anchor_date=first_session_ts)
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

            # Per-session anchor: resolve "yesterday"/"last week"/etc against
            # this session's actual date, not session_1's.
            session_ts = session_dates.get(session_n) or first_session_ts
            eresp = engine.extract(conv_id, session_id=f"locomo_{conv_idx}_s{session_n}",
                                   timeout_ms=600000, anchor_date=session_ts)
            total_extractions += 1
            total_extract_seconds += eresp.get("duration_ms", 0) / 1000.0
            total_facts_added += eresp.get("facts_added", 0)
            last_conv_facts = eresp.get("facts_total", last_conv_facts)

            print(f"  [conv {conv_idx} sess {session_n}] facts={last_conv_facts} "
                  f"+{eresp.get('facts_added', 0)} dur={eresp.get('duration_ms', 0)}ms",
                  file=sys.stderr)

         # Save snapshot at end of this conversation's extraction
         if cache_dir and not no_cache and db_path and map_path:
            try:
               sresp = engine.snapshot_save(db_path, map_path)
               if sresp.get("status") == "ok":
                  print(f"  [conv {conv_idx}] cache SAVE key={cache_key} "
                        f"facts={sresp.get('facts', 0)} "
                        f"entities={sresp.get('entities', 0)}",
                        file=sys.stderr)
               else:
                  print(f"  [conv {conv_idx}] snapshot_save failed: "
                        f"{sresp.get('message', 'unknown')}", file=sys.stderr)
            except Exception as exc:
               print(f"  [conv {conv_idx}] snapshot_save error: {exc}",
                     file=sys.stderr)

      # Score each QA pair via memory query + provenance->dia_id mapping
      qa_pairs = entry.get("qa", entry.get("QA", entry.get("qa_pairs", [])))
      for qa in qa_pairs:
         question = qa.get("question", "")
         evidence = qa.get("evidence", [])
         category = str(qa.get("category", "unknown"))
         # LoCoMo gold answer: prefer 'answer'; cat-5 (adversarial) often uses
         # 'adversarial_answer' instead.  Coerce to str — answers are sometimes
         # ints (dates, counts) and we hash them as text.
         gold_answer_raw = (qa.get("answer")
                            if qa.get("answer") is not None
                            else qa.get("adversarial_answer")
                            if qa.get("adversarial_answer") is not None
                            else qa.get("gold_answer", ""))
         gold_answer = str(gold_answer_raw) if gold_answer_raw is not None else ""
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

         # Build fact texts once — both entailment and generator consume them.
         fact_texts = [r.get("text", "") for r in retrieved if r.get("text")]

         # Entailment judge — strict YES/NO over (question, gold, fact texts)
         if judge is not None and gold_answer:
            ent_score, _ = judge.judge(question, gold_answer, fact_texts)
            all_entailment.append(ent_score)
            per_category_entailment.setdefault(category, []).append(ent_score)

         # Generate-and-judge — the leader-comparable metric.  Generator
         # synthesizes an answer from facts; correctness judge compares to gold.
         if gen_and_judge and gold_answer:
            generated, _ = generator.generate(question, fact_texts)
            corr_score, _ = correctness_judge.judge(
               question, gold_answer, generated)
            all_generation.append(corr_score)
            per_category_generation.setdefault(category, []).append(corr_score)

      # Persist all caches periodically so a crash mid-run doesn't lose work
      if judge is not None and judge.cache is not None:
         judge.cache.flush()
      if generator is not None and generator.cache is not None:
         generator.cache.flush()
      if correctness_judge is not None and correctness_judge.cache is not None:
         correctness_judge.cache.flush()

      avg = sum(all_recall) / len(all_recall) if all_recall else 0
      progress = f"  [conv {conv_idx + 1}/{len(entries)}] QA={total_qa} avg_recall_reach={avg:.3f}"
      if judge is not None and all_entailment:
         avg_ent = sum(all_entailment) / len(all_entailment)
         progress += f" recall_entailment={avg_ent:.3f}"
      if gen_and_judge and all_generation:
         avg_gen = sum(all_generation) / len(all_generation)
         progress += f" recall_generation={avg_gen:.3f}"
      print(progress, file=sys.stderr)

   # Final flush of all caches before reporting
   if judge is not None and judge.cache is not None:
      judge.cache.flush()
   if generator is not None and generator.cache is not None:
      generator.cache.flush()
   if correctness_judge is not None and correctness_judge.cache is not None:
      correctness_judge.cache.flush()

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
      "cache_hits": total_cache_hits,
      "cache_misses": total_cache_misses,
      "elapsed_seconds": elapsed,
      "per_category": {},
   }
   for cat, vals in sorted(per_category.items()):
      results["per_category"][cat] = sum(vals) / len(vals) if vals else 0
   if judge is not None:
      results["judge_model"] = judge.model
      results["judge_provider"] = "anthropic"
      results["judge_calls_made"] = judge.calls_made
      results["judge_cache_hits"] = judge.cache_hits
      results["judge_errors"] = judge.errors
      results["recall_entailment"] = (
         sum(all_entailment) / len(all_entailment) if all_entailment else 0)
      results["entailment_evaluated"] = len(all_entailment)
      results["per_category_entailment"] = {}
      for cat, vals in sorted(per_category_entailment.items()):
         results["per_category_entailment"][cat] = (
            sum(vals) / len(vals) if vals else 0)
   if gen_and_judge:
      results["generator_model"] = generator.model
      results["generator_calls_made"] = generator.calls_made
      results["generator_cache_hits"] = generator.cache_hits
      results["generator_errors"] = generator.errors
      results["correctness_calls_made"] = correctness_judge.calls_made
      results["correctness_cache_hits"] = correctness_judge.cache_hits
      results["correctness_errors"] = correctness_judge.errors
      results["recall_generation"] = (
         sum(all_generation) / len(all_generation) if all_generation else 0)
      results["generation_evaluated"] = len(all_generation)
      results["per_category_generation"] = {}
      for cat, vals in sorted(per_category_generation.items()):
         results["per_category_generation"][cat] = (
            sum(vals) / len(vals) if vals else 0)
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

   if "avg_recall_reach" in results:
      print(f"  {'avg_recall_reach':20s} {results['avg_recall_reach']:.4f}")

   # Entailment block (Phase 2.2 — strict YES/NO judge)
   if "recall_entailment" in results:
      print(f"  {'recall_entailment':20s} {results['recall_entailment']:.4f}  "
            f"(judge={results.get('judge_model', '?')}, "
            f"n={results.get('entailment_evaluated', 0)}, "
            f"calls={results.get('judge_calls_made', 0)}, "
            f"hits={results.get('judge_cache_hits', 0)}, "
            f"errs={results.get('judge_errors', 0)})")

   # Generation block (Phase 2.3 — generate-and-judge, leader-comparable)
   if "recall_generation" in results:
      print(f"  {'recall_generation':20s} {results['recall_generation']:.4f}  "
            f"(gen={results.get('generator_model', '?')} "
            f"calls={results.get('generator_calls_made', 0)} "
            f"hits={results.get('generator_cache_hits', 0)}; "
            f"corr={results.get('judge_model', '?')} "
            f"calls={results.get('correctness_calls_made', 0)} "
            f"hits={results.get('correctness_cache_hits', 0)}; "
            f"n={results.get('generation_evaluated', 0)})")

   # Per-category breakdown
   if "per_category" in results:
      print(f"\n  Per-category recall_reach:")
      for cat, val in results["per_category"].items():
         row = f"    {cat:25s} {val:.3f}"
         if "per_category_entailment" in results:
            ent = results["per_category_entailment"].get(cat)
            if ent is not None:
               row += f"   ent={ent:.3f}"
         if "per_category_generation" in results:
            gen = results["per_category_generation"].get(cat)
            if gen is not None:
               row += f"   gen={gen:.3f}"
         print(row)

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
      "answer support — see atlas/dawn/memory/LOCOMO_CAT3_PROFILING.md).",
   )
   parser.add_argument(
      "--config",
      default="./dawn.toml",
      dest="config_path",
      help="Path to dawn.toml (default ./dawn.toml). Used in --memory-pipeline mode "
      "to load extraction_provider/extraction_model.",
   )
   parser.add_argument(
      "--cache-dir",
      default="./benchmarks/snapshots",
      dest="cache_dir",
      help="Directory for memory-pipeline extraction snapshots (default "
      "./benchmarks/snapshots). Cached state is keyed by extraction model + "
      "embedding provider + dataset content hash; mismatched runs auto-skip "
      "the cache. Has no effect outside --memory-pipeline.",
   )
   parser.add_argument(
      "--no-cache",
      action="store_true",
      dest="no_cache",
      help="Disable extraction snapshot caching for this run (always re-extract). "
      "Combine with --memory-pipeline.",
   )
   parser.add_argument(
      "--judge-model",
      default="",
      dest="judge_model",
      help="Anthropic model to use as the entailment judge (e.g., "
      "'claude-haiku-4-5' for cheap, 'claude-sonnet-4-6' or 'claude-opus-4-7' "
      "for higher confidence). Empty disables entailment scoring. Memory-"
      "pipeline LoCoMo only.",
   )
   parser.add_argument(
      "--judge-provider",
      default="anthropic",
      dest="judge_provider",
      help="Judge provider (only 'anthropic' supported in v1; default).",
   )
   parser.add_argument(
      "--judge-api-key",
      default="",
      dest="judge_api_key",
      help="Anthropic API key for the judge. Falls back to ANTHROPIC_API_KEY "
      "env var, then ./secrets.toml claude_api_key.",
   )
   parser.add_argument(
      "--judge-temperature",
      type=float,
      default=0.0,
      dest="judge_temperature",
      help="Judge sampling temperature (default 0.0; deterministic).",
   )
   parser.add_argument(
      "--judge-max-facts",
      type=int,
      default=20,
      dest="judge_max_facts",
      help="Cap on retrieved facts shown to the judge (default 20). LoCoMo "
      "top-K is typically 10 so 20 is comfortably above.",
   )
   parser.add_argument(
      "--no-judge-cache",
      action="store_true",
      dest="no_judge_cache",
      help="Disable disk caching of judge verdicts. Forces a fresh API call "
      "per QA pair on every run. Useful when iterating on the prompt. Applies "
      "to entailment, generations, and correctness caches.",
   )
   parser.add_argument(
      "--generator-model",
      default="",
      dest="generator_model",
      help="Anthropic model for the answer generator (e.g., 'claude-haiku-4-5' "
      "or 'claude-sonnet-4-6'). Empty disables generate-and-judge — "
      "recall_generation will not be reported. Requires --judge-model for "
      "the correctness verdict.",
   )
   parser.add_argument(
      "--generator-temperature",
      type=float,
      default=0.0,
      dest="generator_temperature",
      help="Generator sampling temperature (default 0.0; deterministic).",
   )
   parser.add_argument(
      "--generator-max-tokens",
      type=int,
      default=200,
      dest="generator_max_tokens",
      help="Max output tokens for the generator (default 200; LoCoMo answers "
      "are typically a phrase or short sentence).",
   )
   parser.add_argument(
      "--sweep-extraction-models",
      default="",
      dest="sweep_extraction_models",
      help="Comma-separated list of extraction models to sweep "
      "(e.g., 'claude-haiku-4-5,claude-sonnet-4-6,claude-opus-4-7'). Each "
      "entry can be 'model' or 'provider:model' (default provider 'claude'). "
      "The benchmark runs once per model with a fresh bench process. "
      "Snapshot/judge/generation caches are shared across the sweep — keys "
      "include the extraction model so models never overwrite each other. "
      "Memory-pipeline LoCoMo only.",
   )
   args = parser.parse_args()

   sweep_models = _parse_sweep_models(args.sweep_extraction_models)
   if sweep_models and not (args.benchmark == "locomo" and args.memory_pipeline):
      print("  --sweep-extraction-models requires --benchmark locomo + "
            "--memory-pipeline; ignoring sweep list", file=sys.stderr)
      sweep_models = []

   if sweep_models:
      _run_sweep(args, sweep_models)
   else:
      results = _run_one(args, extraction_provider="", extraction_model="")
      print_results(args.benchmark.upper(), results)
      if args.output:
         with open(args.output, "w") as f:
            json.dump(results, f, indent=2)
         print(f"  Results saved to: {args.output}")


def _parse_sweep_models(spec):
   """Parse a comma-separated --sweep-extraction-models spec.  Each entry is
   either 'model' (default provider claude) or 'provider:model'.  Returns a
   list of (provider, model, label) tuples."""
   if not spec:
      return []
   out = []
   for raw in spec.split(","):
      entry = raw.strip()
      if not entry:
         continue
      if ":" in entry:
         provider, model = entry.split(":", 1)
         provider = provider.strip()
         model = model.strip()
      else:
         provider = "claude"
         model = entry
      label = f"{provider}:{model}"
      out.append((provider, model, label))
   return out


def _run_one(args, extraction_provider, extraction_model):
   """Run the benchmark once with the given extraction-model override and
   return the results dict.  Spawns a fresh bench process — one per call so
   per-model state never leaks across iterations of a sweep."""
   mode_label = "raw" if args.raw else "hybrid"
   tag = f", extraction={extraction_provider}:{extraction_model}" if extraction_model else ""
   print(f"  Starting bench_retrieval ({args.provider}, {mode_label}{tag})...",
         file=sys.stderr)
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
      extraction_provider=extraction_provider,
      extraction_model=extraction_model,
   )
   print(f"  Ready: {engine.dims} dims, provider={engine.provider}, "
         f"mode={engine.mode}, extraction="
         f"{engine.ready_info.get('extraction_provider', '?')}:"
         f"{engine.ready_info.get('extraction_model', '?')}",
         file=sys.stderr)

   try:
      if args.benchmark == "longmemeval":
         gran = "turn" if args.granularity == "turn" else "session"
         results = run_longmemeval(
            engine, args.dataset, limit=args.limit,
            granularity=gran, turn_scoring=args.turn_scoring)
      elif args.benchmark == "locomo":
         if args.memory_pipeline:
            judge = make_entailment_judge(args)
            generator = make_generator(args)
            correctness_judge = make_correctness_judge(args) if generator else None
            if args.generator_model and not args.judge_model:
               print("  generator: --generator-model requires --judge-model "
                     "(needed for correctness verdict). Generation disabled.",
                     file=sys.stderr)
               generator = None
               correctness_judge = None
            results = run_locomo_memory(
               engine, args.dataset, limit=args.limit, top_k=args.top_k,
               cache_dir=args.cache_dir, no_cache=args.no_cache,
               judge=judge, generator=generator,
               correctness_judge=correctness_judge)
         else:
            results = run_locomo(
               engine, args.dataset, limit=args.limit,
               granularity=args.granularity,
               sentence_chunks=args.sentence_chunks, top_k=args.top_k)
      elif args.benchmark == "convomem":
         results = run_convomem(engine, args.dataset, limit=args.limit or 100)
      else:
         raise ValueError(f"unknown benchmark: {args.benchmark}")
   finally:
      engine.quit()

   return results


def _run_sweep(args, sweep_models):
   """Run the benchmark once per (provider, model) tuple, then emit a
   consolidated comparison table.  Per-model results JSON is saved to
   <cache-dir>/sweep_<label>.json; aggregate JSON to --output if provided."""
   per_model_results = []
   sweep_t0 = time.time()
   for provider, model, label in sweep_models:
      print(f"\n{'#' * 60}\n# Sweep entry: {label}\n{'#' * 60}", file=sys.stderr)
      results = _run_one(args, extraction_provider=provider, extraction_model=model)
      results["_sweep_label"] = label
      per_model_results.append(results)
      print_results(f"{args.benchmark.upper()} [{label}]", results)
      # Per-model JSON next to caches for downstream analysis
      if args.cache_dir:
         per_path = Path(args.cache_dir) / f"sweep_{label.replace(':', '_').replace('/', '_')}.json"
         per_path.parent.mkdir(parents=True, exist_ok=True)
         with open(per_path, "w") as f:
            json.dump(results, f, indent=2)

   sweep_elapsed = time.time() - sweep_t0
   _print_sweep_table(per_model_results, sweep_elapsed)

   if args.output:
      aggregate = {
         "mode": "sweep",
         "elapsed_seconds": sweep_elapsed,
         "models": per_model_results,
      }
      with open(args.output, "w") as f:
         json.dump(aggregate, f, indent=2)
      print(f"  Sweep results saved to: {args.output}")


def _fmt_metric(val):
   """Render a metric for the sweep table — 4-decimal float or '-' when the
   metric isn't applicable (e.g., entailment off, generation off)."""
   if val is None:
      return "-"
   try:
      return f"{float(val):.4f}"
   except (TypeError, ValueError):
      return "-"


def _print_sweep_table(per_model_results, sweep_elapsed):
   """One-line-per-model comparison table plus per-category rows."""
   if not per_model_results:
      return
   labels = [r["_sweep_label"] for r in per_model_results]
   maxw = max(len("Model"), max(len(lbl) for lbl in labels))

   print(f"\n{'=' * 70}")
   print(f"  Four-Model Sweep Summary  (total {sweep_elapsed:.0f}s)")
   print(f"{'=' * 70}")
   header = f"  {'Model':<{maxw}}  {'reach':>7}  {'ent':>7}  {'gen':>7}  {'qa':>5}"
   print(header)
   print("  " + "-" * (len(header) - 2))
   for r in per_model_results:
      reach = _fmt_metric(r.get("avg_recall_reach"))
      ent = _fmt_metric(r.get("recall_entailment"))
      gen = _fmt_metric(r.get("recall_generation"))
      qa = r.get("total_qa", 0)
      print(f"  {r['_sweep_label']:<{maxw}}  "
            f"{reach:>7}  {ent:>7}  {gen:>7}  {qa:>5}")

   # Per-category rows for the metric we trust most: recall_generation
   # if present, else recall_entailment, else recall_reach
   if any("per_category_generation" in r for r in per_model_results):
      _print_sweep_category_table(per_model_results, "per_category_generation",
                                  "recall_generation")
   elif any("per_category_entailment" in r for r in per_model_results):
      _print_sweep_category_table(per_model_results, "per_category_entailment",
                                  "recall_entailment")
   else:
      _print_sweep_category_table(per_model_results, "per_category",
                                  "recall_reach")
   print(f"{'=' * 70}\n")


def _print_sweep_category_table(per_model_results, key, metric_label):
   """Per-category breakdown across models (rows = categories, cols = models)."""
   cats = set()
   for r in per_model_results:
      cats.update((r.get(key) or {}).keys())
   if not cats:
      return
   cats = sorted(cats)
   labels = [r["_sweep_label"] for r in per_model_results]
   col_w = max(7, max(len(lbl) for lbl in labels))

   print(f"\n  Per-category {metric_label}:")
   header = f"    {'cat':<5}  " + "  ".join(f"{lbl:>{col_w}}" for lbl in labels)
   print(header)
   print("    " + "-" * (len(header) - 4))
   for cat in cats:
      cells = []
      for r in per_model_results:
         v = (r.get(key) or {}).get(cat)
         cells.append(f"{v:>{col_w}.3f}" if isinstance(v, (int, float)) else f"{'-':>{col_w}}")
      print(f"    {cat:<5}  " + "  ".join(cells))


if __name__ == "__main__":
   main()
