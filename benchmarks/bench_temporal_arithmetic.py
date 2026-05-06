#!/usr/bin/env python3
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# By contributing to this project, you agree to license your contributions
# under the GPLv3 (or any later version) or any future licenses chosen by
# the project author(s).
#
# Cat-2 temporal-arithmetic guardrail.
#
# Validates that the extraction LLM (default Haiku) reliably resolves relative
# temporal phrases ("yesterday", "last month", "five years ago") to ISO 8601
# dates given a conversation anchor. This is the load-bearing assumption of
# Cat-2 Temporal Phase 1 (anchor injection) — if the model's date arithmetic
# regresses, the extraction prompt or model choice has drifted and the
# anchor-injection mechanism stops paying.
#
# Promoted from /tmp/l1_microbench.py (May 2026 Phase 1.1 work) into a
# permanent bench harness. See atlas/dawn/memory/CAT2_TEMPORAL.md for the
# threshold rationale: 82.1% correct + 2.6% hallucination on the original
# 50-question sample. Default --threshold 0.70 is the architecture-review
# floor; below this, L1 is structurally broken, not just noisy.
#
# Cost: ~$0.02 at default sample size on Haiku. Output: per-case JSON +
# stdout summary. Non-zero exit when match-rate falls below --threshold.
import argparse
import json
import os
import random
import re
import sys
from datetime import datetime
from pathlib import Path

DEFAULT_LOCOMO_PATH = Path.home() / "datasets/locomo/data/locomo10.json"
DEFAULT_SECRETS_PATH = Path(__file__).resolve().parent.parent / "secrets.toml"
DEFAULT_MODEL = "claude-haiku-4-5"
DEFAULT_SAMPLE_SIZE = 50
DEFAULT_SEED = 42
DEFAULT_THRESHOLD = 0.70


def load_api_key(secrets_path):
    body = Path(secrets_path).read_text()
    m = re.search(r'^\s*claude_api_key\s*=\s*"([^"]+)"', body, re.MULTILINE)
    if not m:
        sys.exit(f"error: no claude_api_key in {secrets_path}")
    return m.group(1)


SYSTEM_PROMPT = (
    "You resolve temporal references in conversations to ISO 8601 dates. "
    "You will receive a conversation excerpt, the date the conversation took place "
    "(the anchor), and a question about when an event occurred. "
    "Resolve any relative phrases (yesterday, last week, two months ago, etc.) "
    "against the anchor date. "
    "Output exactly one line of the form: DATE: YYYY-MM-DD or DATE: YYYY-MM or DATE: YYYY "
    "(use the most precise form supported by the source). If no date can be determined, "
    "output exactly: DATE: UNKNOWN. No commentary, no preamble."
)

USER_TEMPLATE = """\
Anchor date (when the conversation took place): {anchor}

Conversation excerpt:
{dialog}

Question: {question}

What date does the event in the question refer to?"""

MONTHS = {
    "january": 1, "february": 2, "march": 3, "april": 4, "may": 5, "june": 6,
    "july": 7, "august": 8, "september": 9, "october": 10, "november": 11, "december": 12,
    "jan": 1, "feb": 2, "mar": 3, "apr": 4, "jun": 6, "jul": 7, "aug": 8,
    "sep": 9, "sept": 9, "oct": 10, "nov": 11, "dec": 12,
}

# Phrase-equivalence tolerance: if the gold contains a relative phrase like
# "the week before X" / "the Friday before X" / "weekend of X", the model is
# expected to resolve to a specific day NEAR the gold's anchor, not the
# anchor itself. We accept any day within PHRASE_DAY_TOLERANCE days of the
# parsed gold date when one of these phrases is present. The May 2026 L1
# baseline (82.1% correct) used manual phrase review; this is the automated
# approximation.
PHRASE_DAY_TOLERANCE = 10
PHRASE_MARKERS = (
    "before", "after", "weekend", "week of", "beginning", "early",
    "middle", "mid-", "end of", "late", "first ", "last ",
)


def build_dia_index(conv):
    out = {}
    n = 0
    while True:
        n += 1
        skey = f"session_{n}"
        if skey not in conv:
            break
        date_s = conv.get(f"session_{n}_date_time", "")
        for d in conv[skey]:
            dia = d.get("dia_id")
            if dia:
                out[dia] = {
                    "session_num": n,
                    "session_date": date_s,
                    "speaker": d.get("speaker", ""),
                    "text": d.get("text", ""),
                }
    return out


def parse_gold(gold):
    """Return (year, month, day) tuple with None for unspecified components."""
    g = gold.lower().strip()
    g = re.sub(r"[,.]", "", g)
    g = re.sub(r"(\d+)(st|nd|rd|th)", r"\1", g)
    m = re.search(r"\b(\d{4})-(\d{1,2})-(\d{1,2})\b", g)
    if m:
        return int(m.group(1)), int(m.group(2)), int(m.group(3))
    m = re.search(r"\b(\d{1,2})\s+(" + "|".join(MONTHS) + r")\s+(\d{4})\b", g)
    if m:
        return int(m.group(3)), MONTHS[m.group(2)], int(m.group(1))
    m = re.search(r"\b(" + "|".join(MONTHS) + r")\s+(\d{1,2})\s+(\d{4})\b", g)
    if m:
        return int(m.group(3)), MONTHS[m.group(1)], int(m.group(2))
    m = re.search(r"\b(" + "|".join(MONTHS) + r")\s+(\d{4})\b", g)
    if m:
        return int(m.group(2)), MONTHS[m.group(1)], None
    m = re.search(r"\b(\d{4})-(\d{1,2})\b", g)
    if m:
        return int(m.group(1)), int(m.group(2)), None
    m = re.search(r"\b(19|20)\d{2}\b", g)
    if m:
        return int(m.group(0)), None, None
    return None, None, None


def parse_emitted(raw):
    """Extract the DATE: line from the model's response."""
    if not raw:
        return None, None, None, "empty"
    m = re.search(r"DATE:\s*(\S.*?)(?:\n|$)", raw, re.IGNORECASE)
    if not m:
        return None, None, None, "no_date_line"
    val = m.group(1).strip()
    if val.upper() == "UNKNOWN":
        return None, None, None, "unknown"
    m2 = re.match(r"^(\d{4})-(\d{1,2})-(\d{1,2})$", val)
    if m2:
        return int(m2.group(1)), int(m2.group(2)), int(m2.group(3)), "day"
    m2 = re.match(r"^(\d{4})-(\d{1,2})$", val)
    if m2:
        return int(m2.group(1)), int(m2.group(2)), None, "month"
    m2 = re.match(r"^(\d{4})$", val)
    if m2:
        return int(m2.group(1)), None, None, "year"
    return None, None, None, f"unparseable:{val[:30]}"


def is_date_gold(gold_t):
    """A gold is date-bearing iff parse_gold extracted at least a year."""
    return gold_t[0] is not None


def has_phrase_marker(gold_text):
    """Detect relative-temporal phrases like 'week before X', 'Friday before X'."""
    g = gold_text.lower()
    return any(marker in g for marker in PHRASE_MARKERS)


def precision_aware_match(gold_t, emit_t, gold_text=""):
    """Match emit against gold with precision-aware comparison and phrase tolerance.

    Year-only gold ⇒ require year match. Adding month/day tightens.
    When the gold contains a relative phrase (e.g. "the Friday before X"),
    accept emit days within PHRASE_DAY_TOLERANCE of the gold day."""
    gy, gm, gd = gold_t
    ey, em, ed = emit_t
    if gy is None or ey is None:
        return False
    if gy != ey:
        return False
    if gm is not None and em is not None and gm != em:
        return False
    if gd is not None and ed is not None:
        if gd == ed:
            return True
        if has_phrase_marker(gold_text) and abs(gd - ed) <= PHRASE_DAY_TOLERANCE:
            return True
        return False
    return True


def collect_questions(locomo_path, parse_locomo_session_date):
    with open(locomo_path) as f:
        locomo = json.load(f)
    all_q = []
    for ci, entry in enumerate(locomo):
        conv = entry.get("conversation", entry)
        idx = build_dia_index(conv)
        for qa in entry.get("qa", []):
            if str(qa.get("category")) != "2":
                continue
            ev_raw = qa.get("evidence", [])
            flat = []
            for e in ev_raw:
                flat.extend(e.split())
            if not flat:
                continue
            primary = flat[0]
            d = idx.get(primary)
            if not d:
                continue
            anchor_ts = parse_locomo_session_date(d["session_date"])
            if not anchor_ts:
                continue
            anchor_iso = datetime.utcfromtimestamp(anchor_ts).strftime("%Y-%m-%d")
            dialog_lines = []
            for dia in flat:
                dd = idx.get(dia, {})
                if dd:
                    dialog_lines.append(f'{dd["speaker"]}: "{dd["text"]}"')
            all_q.append({
                "conv": ci,
                "question": qa.get("question", ""),
                "gold": str(qa.get("answer") or qa.get("adversarial_answer") or ""),
                "evidence": flat,
                "anchor_iso": anchor_iso,
                "dialog": "\n".join(dialog_lines),
            })
    return all_q


def run(args):
    bench_dir = Path(__file__).resolve().parent
    sys.path.insert(0, str(bench_dir))
    from run_benchmark import _anthropic_call, parse_locomo_session_date

    api_key = load_api_key(args.secrets_path)

    all_q = collect_questions(args.locomo_path, parse_locomo_session_date)
    print(f"Total cat-2 questions with valid anchor: {len(all_q)}")
    if len(all_q) < args.sample_size:
        sys.exit(f"error: only {len(all_q)} questions available, need {args.sample_size}")

    random.seed(args.seed)
    sample = random.sample(all_q, args.sample_size)

    print(f"\nRunning {len(sample)} questions through {args.model} "
          f"(seed={args.seed})...\n")
    results = []
    for i, q in enumerate(sample, 1):
        user = USER_TEMPLATE.format(
            anchor=q["anchor_iso"], dialog=q["dialog"][:1500], question=q["question"])
        try:
            raw = _anthropic_call(args.model, SYSTEM_PROMPT, user, api_key,
                                  temperature=0.0, max_tokens=64)
        except Exception as exc:
            raw = ""
            print(f"  [{i}] API error: {exc}", file=sys.stderr)
        gold_t = parse_gold(q["gold"])
        emit_t = parse_emitted(raw)
        date_gold = is_date_gold(gold_t)
        matched = (precision_aware_match(gold_t, emit_t[:3], q["gold"])
                   if date_gold else None)
        results.append({
            **q, "raw": raw, "gold_parsed": gold_t, "emit_parsed": emit_t[:3],
            "emit_status": emit_t[3], "matched": matched, "date_gold": date_gold,
        })
        if date_gold:
            sys.stdout.write(f"\r  [{i}/{len(sample)}] match={matched}  emit={emit_t[3]}  ")
        else:
            sys.stdout.write(f"\r  [{i}/{len(sample)}] non-date-gold  emit={emit_t[3]}  ")
        sys.stdout.flush()
    print()

    total = len(results)
    scorable = [r for r in results if r["date_gold"]]
    n_scorable = len(scorable)
    n_excluded = total - n_scorable
    parsed = sum(1 for r in scorable if r["emit_status"] in ("day", "month", "year"))
    matched = sum(1 for r in scorable if r["matched"])
    unknown = sum(1 for r in scorable if r["emit_status"] == "unknown")
    errors = sum(1 for r in scorable
                 if r["emit_status"] not in ("day", "month", "year", "unknown"))
    matched_rate = matched / n_scorable if n_scorable else 0.0

    print(f"\n=== Cat-2 temporal-arithmetic guardrail ({args.model}, n={total}) ===")
    print(f"  Non-date-gold (excluded): {n_excluded}/{total}")
    print(f"  Scorable (date gold):     {n_scorable}/{total}")
    if n_scorable:
        print(f"  Emitted parseable ISO:    {parsed}/{n_scorable}  "
              f"({100 * parsed / n_scorable:.1f}%)")
        print(f"  Emitted DATE: UNKNOWN:    {unknown}/{n_scorable}  "
              f"({100 * unknown / n_scorable:.1f}%)")
        print(f"  Output errors:            {errors}/{n_scorable}")
        print(f"  Matched gold:             {matched}/{n_scorable}  "
              f"({100 * matched / n_scorable:.1f}%)")
    if parsed:
        print(f"  Matched | parseable:      {matched}/{parsed} "
              f"({100 * matched / parsed:.1f}%)")
    print(f"  Threshold:                {100 * args.threshold:.1f}% "
          f"(matched/scorable; phrase tolerance ±{PHRASE_DAY_TOLERANCE} days)")

    fail = [r for r in scorable
            if r["matched"] is False and r["emit_status"] in ("day", "month", "year")]
    if fail:
        print(f"\nFailures (matched=False, emit=parseable) — first 10 of {len(fail)}:")
        for r in fail[:10]:
            print(f"  conv {r['conv']:>2}  Q: {r['question'][:60]}")
            print(f"           gold={r['gold']!r}  emit={r['emit_parsed']}  "
                  f"status={r['emit_status']}")

    if args.json_output:
        # O_EXCL refuses to overwrite an existing file (so a symlink the
        # operator did not create cannot be followed); O_NOFOLLOW refuses to
        # follow a symlink at the named path; mode 0o600 keeps the dump
        # readable only by the owner. The dump contains LoCoMo dialog
        # excerpts (public benchmark data today) but the same path will be
        # used by any future user-data adaptation, so we set the safer
        # baseline now.
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW
        try:
            fd = os.open(args.json_output, flags, 0o600)
        except FileExistsError:
            sys.exit(f"error: --json-output path {args.json_output} already exists; "
                     f"refusing to overwrite")
        with os.fdopen(fd, "w") as f:
            json.dump(results, f, indent=2, default=str)
        print(f"\nSaved {total} cases to {args.json_output}")

    if matched_rate < args.threshold:
        print(f"\nFAIL: matched-rate {100 * matched_rate:.1f}% < threshold "
              f"{100 * args.threshold:.1f}%")
        return 1
    print(f"\nPASS: matched-rate {100 * matched_rate:.1f}% >= threshold "
          f"{100 * args.threshold:.1f}%")
    return 0


def main():
    parser = argparse.ArgumentParser(
        description="Cat-2 temporal-arithmetic guardrail for the extraction LLM.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    parser.add_argument("--locomo-path", type=Path, default=DEFAULT_LOCOMO_PATH,
                        help="Path to locomo10.json")
    parser.add_argument("--secrets-path", type=Path, default=DEFAULT_SECRETS_PATH,
                        help="Path to secrets.toml (claude_api_key)")
    parser.add_argument("--model", default=DEFAULT_MODEL,
                        help="Anthropic model to test")
    parser.add_argument("--sample-size", type=int, default=DEFAULT_SAMPLE_SIZE,
                        help="Number of cat-2 questions to sample")
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED,
                        help="Random seed for sampling (stable run-to-run)")
    parser.add_argument("--threshold", type=float, default=DEFAULT_THRESHOLD,
                        help="Minimum matched-rate (0.0-1.0); below this exits non-zero")
    parser.add_argument("--json-output", type=Path, default=None,
                        help="Optional path to dump per-case results as JSON")
    args = parser.parse_args()
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
