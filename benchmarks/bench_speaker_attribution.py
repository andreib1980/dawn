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
# Speaker-attribution guardrail for the extraction prompt.
#
# A cheap pre-bench probe (~$0.03 per run, ~10 seconds) that catches the
# fact-widening + attribution-shift modes that the full LoCoMo bench
# catches at $10 + 2 hours.  Becomes a permanent guardrail: every
# extraction-prompt change must pass this probe before any full bench.
#
# Both components run from the SAME 25 hand-curated dialog snippets — no
# upstream bench / snapshot machinery, no separate calibration step.  The
# probe parses EXTRACTION_PROMPT_TEMPLATE live from
# src/memory/memory_extraction.c and sends each snippet through Haiku at
# temperature 0.0, then aggregates two views over the responses:
#
#   (1) FACT-COUNT WIDENING — total facts produced across all 25 cases.
#       Catches the top-K-crowding mechanism that sank attempt #1.  At
#       temperature 0.0 the metric is highly deterministic (HEAD
#       calibration: 60 facts run-to-run within ±1).  Threshold +10%.
#       Conceptual basis: when the prompt encourages finer-grained fact
#       splitting (e.g. one event → three sub-facts), the 20-fact
#       retrieval top-K dilutes canonical-subject attribution and the
#       generator's source-budget chars get spent on fragments instead of
#       cohesive facts.  Attempt #1 produced 77/60 = +28% on this metric
#       while only +5.95% on full conv 7 (within HEAD's own ±6% run-to-
#       run noise) — the test-set view is cleaner because the cases are
#       all single-statement extractions where granularity drift shows
#       directly.
#
#   (2) ATTRIBUTION CORRECTNESS — per-case subject check.
#       Each case has expected and forbidden subject keywords.  Pass
#       requires (a) at least one extracted fact mentions an expected
#       subject, AND (b) no extracted fact mentions a forbidden subject
#       without also mentioning an expected one.  Aggregate metrics:
#       known-good pass rate (must be 100%; positive control on the
#       single-user case) + attribution-failure pass rate (must be >=
#       BASELINE - 10pp).
#
# Validated 2026-05-07 against attempt #1's reverted prompt:
#   - Component 1 fact count: HEAD 60 → A1 77 (+28%) → REJECTED at +10%
#   - Component 2 attribution: HEAD 25/25 → A1 25/25 (test cases too
#     simple to differentiate at this granularity; component 1 is the
#     load-bearing reject)
#
# Component 2's attribution-pass-rate gate alone wasn't sensitive enough
# to flag attempt #1 — attempt #1 still got the simple cases right; the
# regression manifested in retrieval-and-generation, not in raw
# attribution at the prompt level.  Component 1 IS sensitive because the
# fact-granularity drift compounds across many extractions.  Both
# components ship: component 2 is the safety-net for known-good
# regression (any future prompt change that breaks the simple case fails
# component 2 immediately) while component 1 is the load-bearing
# fact-widening detector.
#
# Workflow for future prompt changes:
#
#   1. Edit src/memory/memory_extraction.c.
#   2. python3 benchmarks/bench_speaker_attribution.py
#      Probe exits non-zero on failure with per-component reasons.
#   3. If green, only then run the full LoCoMo bench (~$10).
#
# Calibration baseline (HEAD as of 2026-05-07, claude-haiku-4-5):
#
#   Component 1: BASELINE_FACT_COUNT = 60 facts across 25 cases, threshold +10%.
#                Run-to-run variance at temp=0: ±1 fact.
#   Component 2: HEAD pass rate 25/25 (5/5 known-good + 20/20 attribution).
#                Threshold: known-good must be 25/25; attribution must be
#                >= BASELINE_ATTR_PASS_RATE - 0.10.
#
# Recalibrate via the calibration ritual when the extraction model or
# embedding provider changes:
#   1. Run probe on HEAD with a fresh model.
#   2. Update BASELINE_FACT_COUNT and BASELINE_ATTR_PASS_RATE constants.
#   3. Re-run the bedrock validation against attempt #1 (instructions
#      below).

import argparse
import json
import os
import re
import sys
from pathlib import Path

DAWN_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_LOCOMO_PATH = Path.home() / "datasets/locomo/data/locomo10.json"
DEFAULT_SECRETS_PATH = DAWN_ROOT / "secrets.toml"
DEFAULT_MODEL = "claude-haiku-4-5"
EXTRACTION_C_PATH = DAWN_ROOT / "src/memory/memory_extraction.c"

# Calibrated 2026-05-07.  See module docstring for recalibration procedure.
BASELINE_FACT_COUNT = 60          # total facts across 25 cases under HEAD prompt
FACT_COUNT_THRESHOLD = 1.10       # fail if candidate > BASELINE * threshold
BASELINE_ATTR_PASS_RATE = 1.00    # 20/20 attribution-failure cases under HEAD
ATTR_PASS_RATE_TOLERANCE = 0.10   # candidate must be >= baseline - 10pp


# =============================================================================
# Prompt extraction from C source
# =============================================================================

def extract_prompt_template_from_c(c_path):
    """Parse EXTRACTION_PROMPT_TEMPLATE from memory_extraction.c.

    The template is a static const string built by C string-literal
    concatenation across many lines, containing C strings whose literal
    text includes semicolons (e.g. "preferences[];").  A naive regex
    that stops at the first ';' eats half the prompt.  Instead we locate
    the assignment, then walk character by character respecting C
    string-literal escape rules until we find a ';' outside any string.
    Fails loudly if the declaration shape changes.
    """
    src = Path(c_path).read_text()

    decl = re.search(
        r"static\s+const\s+char\s*\*\s*EXTRACTION_PROMPT_TEMPLATE\s*=\s*",
        src)
    if not decl:
        sys.exit(f"error: EXTRACTION_PROMPT_TEMPLATE declaration not found in {c_path}")

    i = decl.end()
    fragments = []
    in_string = False
    in_block_comment = False
    in_line_comment = False
    cur = []
    n = len(src)
    while i < n:
        ch = src[i]
        if in_block_comment:
            if ch == "*" and i + 1 < n and src[i + 1] == "/":
                in_block_comment = False
                i += 2
                continue
            i += 1
            continue
        if in_line_comment:
            if ch == "\n":
                in_line_comment = False
            i += 1
            continue
        if in_string:
            if ch == "\\" and i + 1 < n:
                cur.append(src[i:i + 2])
                i += 2
                continue
            if ch == '"':
                fragments.append("".join(cur))
                cur = []
                in_string = False
                i += 1
                continue
            cur.append(ch)
            i += 1
            continue
        # Outside string and comments — looking for opening literal or ';' terminator.
        if ch == ";":
            break
        if ch == '"':
            in_string = True
            i += 1
            continue
        if ch == "/" and i + 1 < n and src[i + 1] == "*":
            in_block_comment = True
            i += 2
            continue
        if ch == "/" and i + 1 < n and src[i + 1] == "/":
            in_line_comment = True
            i += 2
            continue
        i += 1

    if i >= n:
        sys.exit(f"error: walked past end-of-file looking for ';' terminator")
    if not fragments:
        sys.exit(f"error: no string literals found inside EXTRACTION_PROMPT_TEMPLATE")

    # Decode the small set of C escapes we actually use.
    def decode(s):
        return (s.replace(r"\n", "\n")
                 .replace(r"\t", "\t")
                 .replace(r'\"', '"')
                 .replace(r"\\", "\\"))

    return "".join(decode(f) for f in fragments)


# =============================================================================
# Component 1 — fact-count widening
#
# Computed from the same case_results component 2 produces.  No separate
# extraction or snapshot needed.  The metric is the total number of facts
# extracted across all 25 cases; threshold is BASELINE_FACT_COUNT * 1.10.
# =============================================================================


def component1_fact_count(case_results):
    """Aggregate total fact count across all test cases (case_results comes
    from component2_attribution).  Returns (pass, info)."""
    total = sum(len(r.get("fact_texts") or []) for r in case_results)
    threshold = int(BASELINE_FACT_COUNT * FACT_COUNT_THRESHOLD)
    delta_pct = 100.0 * (total - BASELINE_FACT_COUNT) / BASELINE_FACT_COUNT
    info = {
        "baseline_fact_count": BASELINE_FACT_COUNT,
        "candidate_fact_count": total,
        "threshold_fact_count": threshold,
        "delta_pct": round(delta_pct, 1),
        "n_cases": len(case_results),
    }
    return total <= threshold, info


# =============================================================================
# Targeted attribution test cases (component 2)
# =============================================================================
#
# Each case has: a 1-3 turn dialog snippet (with explicit "Speaker said,"
# prefixes mirroring the LoCoMo bench harness), an anchor date for the
# extraction prompt, expected/forbidden subject keywords (case-insensitive
# substring match against fact_text).  Pass requires:
#   - at least one extracted fact contains an expected subject, AND
#   - no extracted fact contains a forbidden subject without also
#     containing an expected subject.
#
# 20 attribution-failure cases (assistant-side personal statements) +
# 5 known-good cases (user-side personal statements as positive control).
# All sourced from LoCoMo10 cat-1/cat-2 evidence dialogs.

# fmt: off
TEST_CASES = [
    # ---- Conv 7 (Deborah/Jolene) — Case 10 family --------------------------
    {
        "id": "case10_jolene_pendant",
        "kind": "attribution_failure",
        "anchor": "2023-01-15",
        "turns": [
            ("user", 'Deborah said, "Hey Jolene, nice to meet you! Have you found a way to remember her?"'),
            ("assistant", 'Jolene said, "This pendant reminds me of my mother, she gave it to me in 2010 in Paris."'),
        ],
        "expected_subjects": ["jolene"],
        "forbidden_subjects": ["deborah"],
    },
    # ---- Conv 0 (Caroline=user / Melanie=assistant) ------------------------
    {
        "id": "melanie_charity_race",
        "kind": "attribution_failure",
        "anchor": "2023-05-25",
        "turns": [
            ("user", 'Caroline said, "Hey Mel! What\'s been going on?"'),
            ("assistant", 'Melanie said, "Hey Caroline, since we last chatted, I\'ve had a lot of things happening to me. I ran a charity race for mental health last Saturday – it was tough but rewarding."'),
        ],
        "expected_subjects": ["melanie"],
        "forbidden_subjects": ["caroline"],
    },
    {
        "id": "melanie_pottery_class",
        "kind": "attribution_failure",
        "anchor": "2023-07-03",
        "turns": [
            ("user", 'Caroline said, "Doing anything creative lately?"'),
            ("assistant", 'Melanie said, "Wow, Caroline! That\'s great! I just signed up for a pottery class yesterday. It\'s like therapy for me."'),
        ],
        "expected_subjects": ["melanie"],
        "forbidden_subjects": ["caroline"],
    },
    {
        "id": "melanie_clarinet",
        "kind": "attribution_failure",
        "anchor": "2023-08-30",
        "turns": [
            ("user", 'Caroline said, "Do you play any instruments?"'),
            ("assistant", 'Melanie said, "Yeah, I play clarinet! Started when I was young and it\'s been great. Expression of myself and a way to relax."'),
        ],
        "expected_subjects": ["melanie"],
        "forbidden_subjects": ["caroline"],
    },
    {
        "id": "melanie_book_nothing_impossible",
        "kind": "attribution_failure",
        "anchor": "2023-07-12",
        "turns": [
            ("user", 'Caroline said, "Read anything good recently?"'),
            ("assistant", 'Melanie said, "I read \\"Nothing is Impossible\\" back in 2022 — it really stuck with me."'),
        ],
        "expected_subjects": ["melanie"],
        "forbidden_subjects": ["caroline"],
    },
    # ---- Conv 1 (Gina=user / Jon=assistant) --------------------------------
    {
        "id": "jon_paris_trip",
        "kind": "attribution_failure",
        "anchor": "2023-01-30",
        "turns": [
            ("user", 'Gina said, "What have you been up to lately, Jon?"'),
            ("assistant", 'Jon said, "I traveled to Paris on the 28th — it was an incredible trip."'),
        ],
        "expected_subjects": ["jon"],
        "forbidden_subjects": ["gina"],
    },
    # ---- Conv 2 (Maria=user / John=assistant) ------------------------------
    {
        "id": "john_personal_statement",
        "kind": "attribution_failure",
        "anchor": "2023-06-15",
        "turns": [
            ("user", 'Maria said, "How have things been with you, John?"'),
            ("assistant", 'John said, "I started a new job at a tech startup in May 2023 — it\'s been a huge change."'),
        ],
        "expected_subjects": ["john"],
        "forbidden_subjects": ["maria"],
    },
    # ---- Conv 3 (Nate=user / Joanna=assistant) -----------------------------
    {
        "id": "joanna_screenplay",
        "kind": "attribution_failure",
        "anchor": "2022-05-20",
        "turns": [
            ("user", 'Nate said, "How\'s the writing going, Joanna?"'),
            ("assistant", 'Joanna said, "I\'m working on my third screenplay right now — trying to finish a draft by the end of the month."'),
        ],
        "expected_subjects": ["joanna"],
        "forbidden_subjects": ["nate"],
    },
    {
        "id": "joanna_woodhaven_visit",
        "kind": "attribution_failure",
        "anchor": "2022-07-15",
        "turns": [
            ("user", 'Nate said, "Been anywhere fun?"'),
            ("assistant", 'Joanna said, "I visited Woodhaven in July 2022 — beautiful little town."'),
        ],
        "expected_subjects": ["joanna"],
        "forbidden_subjects": ["nate"],
    },
    # ---- Conv 4 (John=user / Tim=assistant) --------------------------------
    {
        "id": "tim_personal_health",
        "kind": "attribution_failure",
        "anchor": "2023-09-10",
        "turns": [
            ("user", 'John said, "How have you been holding up?"'),
            ("assistant", 'Tim said, "Honestly, I started running every morning at the beginning of August. It\'s been good for my head."'),
        ],
        "expected_subjects": ["tim"],
        "forbidden_subjects": ["john"],
    },
    # ---- Conv 5 (Audrey=user / Andrew=assistant) ---------------------------
    {
        "id": "andrew_dog_adoption",
        "kind": "attribution_failure",
        "anchor": "2023-04-12",
        "turns": [
            ("user", 'Audrey said, "Any new pets in your life?"'),
            ("assistant", 'Andrew said, "I adopted a rescue dog named Max last weekend — he\'s already settled in."'),
        ],
        "expected_subjects": ["andrew"],
        "forbidden_subjects": ["audrey"],
    },
    # ---- Conv 6 (John=user / James=assistant) — Cyberpunk case ------------
    {
        "id": "james_cyberpunk",
        "kind": "attribution_failure",
        "anchor": "2022-10-21",
        "turns": [
            ("user", 'John said, "Played anything new lately?"'),
            ("assistant", 'James said, "Those games introduced me to gaming and I\'ve been hooked ever since. By the way, yesterday I tried Cyberpunk 2077. Great game, so immersive."'),
        ],
        "expected_subjects": ["james"],
        "forbidden_subjects": ["john"],
    },
    # ---- Conv 7 (Deborah=user / Jolene=assistant) — more cases ------------
    {
        "id": "jolene_paris_pendant_country",
        "kind": "attribution_failure",
        "anchor": "2023-01-15",
        "turns": [
            ("user", 'Deborah said, "Tell me more about that pendant."'),
            ("assistant", 'Jolene said, "My mother bought it for me in France in 2010 — we were visiting Paris together that summer."'),
        ],
        "expected_subjects": ["jolene"],
        "forbidden_subjects": ["deborah"],
    },
    # ---- Conv 8 (Sam=user / Evan=assistant) — Kustom guitar ---------------
    {
        "id": "evan_vintage_guitar",
        "kind": "attribution_failure",
        "anchor": "2023-11-05",
        "turns": [
            ("user", 'Sam said, "What\'s the story with that guitar?"'),
            ("assistant", 'Evan said, "It\'s a 1968 Kustom K-200A vintage guitar and I got it as a gift from a close friend. I lost my job at the end of October 2023, so it\'s been a tough time."'),
        ],
        "expected_subjects": ["evan"],
        "forbidden_subjects": ["sam"],
    },
    # ---- Conv 9 (Calvin=user / Dave=assistant) -----------------------------
    {
        "id": "dave_garden_project",
        "kind": "attribution_failure",
        "anchor": "2023-05-18",
        "turns": [
            ("user", 'Calvin said, "What have you been working on?"'),
            ("assistant", 'Dave said, "I built a vegetable garden in my backyard last April — first time growing tomatoes."'),
        ],
        "expected_subjects": ["dave"],
        "forbidden_subjects": ["calvin"],
    },
    # ---- More Conv 0 cases for diversity ----------------------------------
    {
        "id": "melanie_camping_beach",
        "kind": "attribution_failure",
        "anchor": "2023-06-15",
        "turns": [
            ("user", 'Caroline said, "Where do you like to camp?"'),
            ("assistant", 'Melanie said, "Here\'s a pic of my family camping at the beach. We love it, it brings us together."'),
        ],
        "expected_subjects": ["melanie"],
        "forbidden_subjects": ["caroline"],
    },
    {
        "id": "melanie_horse_painting",
        "kind": "attribution_failure",
        "anchor": "2023-08-15",
        "turns": [
            ("user", 'Caroline said, "Show me your latest art!"'),
            ("assistant", 'Melanie said, "Here\'s a photo of my horse painting I did recently. I\'ve also painted a sunset and a sunrise."'),
        ],
        "expected_subjects": ["melanie"],
        "forbidden_subjects": ["caroline"],
    },
    {
        "id": "melanie_pets",
        "kind": "attribution_failure",
        "anchor": "2023-08-01",
        "turns": [
            ("user", 'Caroline said, "How are the kids and pets?"'),
            ("assistant", 'Melanie said, "Yeah, they\'re good — we got another cat named Bailey to join Oliver and Luna."'),
        ],
        "expected_subjects": ["melanie"],
        "forbidden_subjects": ["caroline"],
    },
    {
        "id": "melanie_pottery_plate",
        "kind": "attribution_failure",
        "anchor": "2023-08-25",
        "turns": [
            ("user", 'Caroline said, "What\'s new in your art?"'),
            ("assistant", 'Melanie said, "I made a plate in pottery class yesterday — I love how relaxing pottery is."'),
        ],
        "expected_subjects": ["melanie"],
        "forbidden_subjects": ["caroline"],
    },
    {
        "id": "melanie_running_destress",
        "kind": "attribution_failure",
        "anchor": "2023-07-15",
        "turns": [
            ("user", 'Caroline said, "How do you handle stress?"'),
            ("assistant", 'Melanie said, "I\'ve been running farther to de-stress, which has been great for my headspace."'),
        ],
        "expected_subjects": ["melanie"],
        "forbidden_subjects": ["caroline"],
    },

    # =====================================================================
    # KNOWN-GOOD positive controls — role=user side personal statements.
    # Should always pass with any reasonable prompt.  If a candidate prompt
    # fails any of these, the simple single-user case is broken.
    # =====================================================================
    {
        "id": "good_caroline_lgbtq",
        "kind": "known_good",
        "anchor": "2023-04-22",
        "turns": [
            ("user", 'Caroline said, "I came out to my parents three years ago — it was a turning point for me."'),
            ("assistant", 'Melanie said, "Wow, Caroline, that takes courage. How did it go?"'),
        ],
        "expected_subjects": ["caroline"],
        "forbidden_subjects": [],  # no specific forbidden — just expect Caroline-attribution
    },
    {
        "id": "good_deborah_meet_jolene",
        "kind": "known_good",
        "anchor": "2023-01-15",
        "turns": [
            ("user", 'Deborah said, "I work at a publishing house in New York and I just got promoted to senior editor."'),
            ("assistant", 'Jolene said, "Congrats, Deborah! That\'s wonderful."'),
        ],
        "expected_subjects": ["deborah"],
        "forbidden_subjects": [],
    },
    {
        "id": "good_audrey_dogs",
        "kind": "known_good",
        "anchor": "2023-04-12",
        "turns": [
            ("user", 'Audrey said, "I\'ve had two dogs since I was a kid — Bella and Max are family to me."'),
            ("assistant", 'Andrew said, "That\'s sweet. Mine\'s an only-child rescue."'),
        ],
        "expected_subjects": ["audrey"],
        "forbidden_subjects": [],
    },
    {
        "id": "good_calvin_japan",
        "kind": "known_good",
        "anchor": "2023-09-10",
        "turns": [
            ("user", 'Calvin said, "I went to Japan for a month last spring — best trip of my life."'),
            ("assistant", 'Dave said, "Whoa, that sounds amazing."'),
        ],
        "expected_subjects": ["calvin"],
        "forbidden_subjects": [],
    },
    {
        "id": "good_sam_homebrew",
        "kind": "known_good",
        "anchor": "2023-11-05",
        "turns": [
            ("user", 'Sam said, "I\'ve been brewing my own beer at home for about two years now."'),
            ("assistant", 'Evan said, "That\'s a cool hobby."'),
        ],
        "expected_subjects": ["sam"],
        "forbidden_subjects": [],
    },
]
# fmt: on


def build_conversation_json(turns):
    """Format turns as the JSON-array-of-{role,content} that production
    extraction sees in conversation_json (memory_extraction.c:1252)."""
    return json.dumps([{"role": r, "content": c} for r, c in turns])


def build_extraction_prompt(template, anchor_iso, conversation_json,
                            existing_profile="(none)"):
    """Apply the same %s formatting production uses
    (memory_extraction.c:914): anchor_line, conversation_json,
    existing_profile.  anchor_line is empty when no anchor is known;
    here we always provide one because each test case has a date."""
    anchor_line = f"Conversation anchor: {anchor_iso}\n\n"
    return template % (anchor_line, conversation_json, existing_profile)


def parse_extraction_response(raw):
    """Parse the LLM's JSON response into the {facts, ...} dict.  Mirrors
    memory_extraction_parse_json() in C: tries direct parse, then a
    ```json fence, then bracket-fallback."""
    if not raw:
        return None
    try:
        return json.loads(raw)
    except json.JSONDecodeError:
        pass
    m = re.search(r"```(?:json)?\s*(.*?)```", raw, flags=re.DOTALL)
    if m:
        try:
            return json.loads(m.group(1))
        except json.JSONDecodeError:
            pass
    start = None
    for ch in ("{", "["):
        i = raw.find(ch)
        if i >= 0 and (start is None or i < start):
            start = i
    if start is not None:
        try:
            return json.loads(raw[start:])
        except json.JSONDecodeError:
            pass
    return None


def evaluate_attribution(facts, expected_subjects, forbidden_subjects):
    """Score a single test case's facts.

    Pass:
      - at least one fact contains some expected_subject keyword, AND
      - no fact contains a forbidden_subject keyword WITHOUT also
        containing an expected_subject keyword (mixed-mention facts are
        neutral, sole-forbidden facts fail).
    Returns (pass, expected_hits, forbidden_only_hits, neutral_hits)."""
    if not facts:
        return False, 0, 0, 0
    expected_hits = 0
    forbidden_only_hits = 0
    neutral_hits = 0
    exp_lc = [e.lower() for e in expected_subjects]
    forb_lc = [f.lower() for f in forbidden_subjects]
    for fact in facts:
        text = (fact.get("text") or "").lower()
        has_exp = any(e in text for e in exp_lc)
        has_forb = any(f in text for f in forb_lc) if forb_lc else False
        if has_exp:
            expected_hits += 1
        if has_forb and not has_exp:
            forbidden_only_hits += 1
        elif has_forb and has_exp:
            neutral_hits += 1
    passed = expected_hits >= 1 and forbidden_only_hits == 0
    return passed, expected_hits, forbidden_only_hits, neutral_hits


def component2_attribution(args, prompt_template):
    """Run each test case through the live extraction prompt + Anthropic
    Haiku.  Returns (pass, info)."""
    sys.path.insert(0, str(DAWN_ROOT / "benchmarks"))
    from run_benchmark import _anthropic_call

    api_key = load_api_key(args.secrets_path)

    case_results = []
    failures = []
    for i, case in enumerate(TEST_CASES, 1):
        conv_json = build_conversation_json(case["turns"])
        prompt = build_extraction_prompt(
            prompt_template, case["anchor"], conv_json, existing_profile="(none)")
        try:
            raw = _anthropic_call(args.model, "", prompt, api_key,
                                  temperature=0.0, max_tokens=1024)
        except Exception as exc:
            print(f"  [{i}] {case['id']}: API error: {exc}", file=sys.stderr)
            case_results.append({**case, "raw": "", "fact_texts": [],
                                 "passed": False, "reason": f"api_error: {exc}"})
            failures.append(case["id"])
            continue
        parsed = parse_extraction_response(raw)
        facts = parsed.get("facts") if isinstance(parsed, dict) else None
        if facts is None:
            case_results.append({**case, "raw": raw, "fact_texts": [],
                                 "passed": False, "reason": "parse_failed"})
            failures.append(case["id"])
            sys.stdout.write(f"\r  [{i}/{len(TEST_CASES)}] {case['id']:<35} parse_FAIL  ")
            sys.stdout.flush()
            continue
        passed, eh, fh, nh = evaluate_attribution(
            facts, case["expected_subjects"], case["forbidden_subjects"])
        case_results.append({
            **case, "raw": raw,
            "fact_texts": [f.get("text", "") for f in facts],
            "expected_hits": eh, "forbidden_only_hits": fh, "neutral_hits": nh,
            "passed": passed,
            "reason": "ok" if passed
                      else f"exp={eh} forb_only={fh}",
        })
        if not passed:
            failures.append(case["id"])
        marker = "PASS" if passed else "FAIL"
        sys.stdout.write(f"\r  [{i}/{len(TEST_CASES)}] {case['id']:<35} {marker:<6}")
        sys.stdout.flush()
    print()

    n_total = len(case_results)
    n_passed = sum(1 for r in case_results if r["passed"])
    n_known_good = sum(1 for r in case_results if r["kind"] == "known_good")
    n_known_good_passed = sum(
        1 for r in case_results if r["kind"] == "known_good" and r["passed"])
    n_attr = sum(1 for r in case_results if r["kind"] == "attribution_failure")
    n_attr_passed = sum(
        1 for r in case_results if r["kind"] == "attribution_failure" and r["passed"])

    info = {
        "total_cases": n_total,
        "total_passed": n_passed,
        "pass_rate": round(n_passed / n_total, 3) if n_total else 0.0,
        "known_good_passed": n_known_good_passed,
        "known_good_total": n_known_good,
        "attribution_passed": n_attr_passed,
        "attribution_total": n_attr,
        "failures": failures,
        "case_results": case_results,
    }

    # Threshold rules:
    #  - any known-good failure → automatic FAIL (positive-control regression)
    #  - attribution pass rate must be >= BASELINE_ATTR_PASS_RATE - tolerance
    if n_known_good_passed < n_known_good:
        return False, info
    attr_rate = n_attr_passed / max(1, n_attr)
    if attr_rate < BASELINE_ATTR_PASS_RATE - ATTR_PASS_RATE_TOLERANCE:
        return False, info
    return True, info


# =============================================================================
# Helpers
# =============================================================================

def load_api_key(secrets_path):
    body = Path(secrets_path).read_text()
    m = re.search(r'^\s*claude_api_key\s*=\s*"([^"]+)"', body, re.MULTILINE)
    if not m:
        sys.exit(f"error: no claude_api_key in {secrets_path}")
    return m.group(1)


# =============================================================================
# Main
# =============================================================================

def main():
    parser = argparse.ArgumentParser(
        description="Cheap speaker-attribution probe for the extraction prompt.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    parser.add_argument("--secrets-path", type=Path, default=DEFAULT_SECRETS_PATH,
                        help="Path to secrets.toml (claude_api_key)")
    parser.add_argument("--model", default=DEFAULT_MODEL,
                        help="Anthropic model for the test-case extractions")
    parser.add_argument("--extraction-c", type=Path, default=EXTRACTION_C_PATH,
                        help="Path to memory_extraction.c (extracts live prompt)")
    parser.add_argument("--json-output", type=Path,
                        help="Optional path to dump per-case results")
    args = parser.parse_args()

    print(f"=== Speaker-attribution probe ===")
    print(f"  EXTRACTION_PROMPT_TEMPLATE source: {args.extraction_c}")

    prompt_template = extract_prompt_template_from_c(args.extraction_c)
    # Sanity-check: must contain the %s placeholders production relies on.
    if prompt_template.count("%s") < 3:
        sys.exit("error: extracted prompt has fewer than 3 %s placeholders — "
                 "regex parse may have lost fragments")

    n_attr = sum(1 for c in TEST_CASES if c["kind"] == "attribution_failure")
    n_good = sum(1 for c in TEST_CASES if c["kind"] == "known_good")
    print(f"  Prompt template: {len(prompt_template)} chars, "
          f"{prompt_template.count('%s')} %s placeholders")
    print(f"  Test cases: {len(TEST_CASES)} ({n_attr} attribution-failure + "
          f"{n_good} known-good)")
    print(f"  Component 1 baseline: {BASELINE_FACT_COUNT} facts across all cases, "
          f"threshold +{int((FACT_COUNT_THRESHOLD - 1) * 100)}%")
    print(f"  Component 2 baseline: attribution pass rate "
          f"{BASELINE_ATTR_PASS_RATE:.2f}, "
          f"tolerance -{int(ATTR_PASS_RATE_TOLERANCE * 100)}pp")
    print()

    # Single API pass — both components consume the same case_results.
    print(f"--- Running {len(TEST_CASES)} test cases at temperature=0.0 ---")
    c2_pass, c2_info = component2_attribution(args, prompt_template)
    case_results = c2_info["case_results"]

    # Component 1 from aggregated case_results.
    c1_pass, c1_info = component1_fact_count(case_results)

    print(f"--- Component 1: fact-count widening — "
          f"{'PASS' if c1_pass else 'FAIL'} ---")
    print(f"  Baseline (HEAD): {c1_info['baseline_fact_count']} facts "
          f"across {c1_info['n_cases']} cases")
    print(f"  Candidate:       {c1_info['candidate_fact_count']} facts")
    print(f"  Threshold:       <= {c1_info['threshold_fact_count']} "
          f"(BASELINE * {FACT_COUNT_THRESHOLD})")
    print(f"  Delta:           {c1_info['delta_pct']:+.1f}%")
    print()

    print(f"--- Component 2: attribution correctness — "
          f"{'PASS' if c2_pass else 'FAIL'} ---")
    print(f"  Total:           {c2_info['total_passed']}/{c2_info['total_cases']} "
          f"({100 * c2_info['pass_rate']:.1f}%)")
    print(f"  Known-good:      {c2_info['known_good_passed']}/{c2_info['known_good_total']} "
          f"(must be all-pass)")
    print(f"  Attribution:     {c2_info['attribution_passed']}/{c2_info['attribution_total']} "
          f"(threshold >= "
          f"{int((BASELINE_ATTR_PASS_RATE - ATTR_PASS_RATE_TOLERANCE) * c2_info['attribution_total'])}/"
          f"{c2_info['attribution_total']})")
    if c2_info["failures"]:
        print(f"  Failures:        {', '.join(c2_info['failures'][:10])}"
              + (" ..." if len(c2_info['failures']) > 10 else ""))
    print()

    overall = c1_pass and c2_pass
    results = {
        "component_1": {"passed": c1_pass, **c1_info},
        "component_2": {
            "passed": c2_pass,
            "total_passed": c2_info["total_passed"],
            "total_cases": c2_info["total_cases"],
            "pass_rate": c2_info["pass_rate"],
            "known_good_passed": c2_info["known_good_passed"],
            "known_good_total": c2_info["known_good_total"],
            "attribution_passed": c2_info["attribution_passed"],
            "attribution_total": c2_info["attribution_total"],
            "failures": c2_info["failures"],
            "case_results": case_results,
        },
    }

    if args.json_output:
        # Match bench_temporal_arithmetic.py: O_EXCL/O_NOFOLLOW + mode 0o600.
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW
        try:
            fd = os.open(args.json_output, flags, 0o600)
        except FileExistsError:
            sys.exit(f"error: --json-output path {args.json_output} already exists")
        with os.fdopen(fd, "w") as f:
            json.dump(results, f, indent=2, default=str)
        print(f"  Saved results to {args.json_output}")

    print(f"=== Overall: {'PASS' if overall else 'FAIL'} ===")
    return 0 if overall else 1


if __name__ == "__main__":
    sys.exit(main())
