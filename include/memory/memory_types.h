/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * By contributing to this project, you agree to license your contributions
 * under the GPLv3 (or any later version) or any future licenses chosen by
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Memory System Type Definitions
 *
 * Defines data structures for the persistent memory system including
 * facts, preferences, and conversation summaries.
 */

#ifndef MEMORY_TYPES_H
#define MEMORY_TYPES_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Buffer Size Constants
 * ============================================================================= */

#define MEMORY_FACT_TEXT_MAX 512
#define MEMORY_SOURCE_MAX 16
#define MEMORY_CATEGORY_MAX 32
#define MEMORY_PREF_VALUE_MAX 256
#define MEMORY_SUMMARY_MAX 2048
#define MEMORY_TOPICS_MAX 256
#define MEMORY_SESSION_ID_MAX 64
#define MEMORY_SENTIMENT_MAX 16

/* Maximum items for batch operations */
#define MEMORY_MAX_FACTS 50
#define MEMORY_MAX_PREFS 20
#define MEMORY_MAX_SUMMARIES 10

/* =============================================================================
 * Memory Fact Structure
 *
 * Represents a single fact about the user, e.g., "User has a golden retriever
 * named Max" or "User works as a software engineer".
 *
 * Facts can be:
 * - Explicit: User directly stated it ("Remember that I prefer dark mode")
 * - Inferred: Extracted from conversation context
 *
 * Confidence ranges from 0.0 (uncertain) to 1.0 (definite).
 * ============================================================================= */

typedef struct {
   int64_t id;
   int user_id;
   char fact_text[MEMORY_FACT_TEXT_MAX];
   float confidence;
   char source[MEMORY_SOURCE_MAX];     /* "explicit" or "inferred" */
   char category[MEMORY_CATEGORY_MAX]; /* One of MEMORY_FACT_CATEGORIES (v34) */
   time_t created_at;
   time_t last_accessed;
   int access_count;
   int64_t superseded_by; /* ID of fact that replaced this one, or 0 */
} memory_fact_t;

/* =============================================================================
 * Canonical fact category labels (v34).
 *
 * Single source of truth referenced by:
 *   - extraction allowlist  (memory_extraction.c)
 *   - LLM tool enum_values  (memory_tool.c)
 *   - centroid seed table   (memory_embeddings.c)
 *
 * NULL-terminated.  "general" is the always-present fallback when a fact does
 * not clearly fit any other category.  Add new labels at the end and bump
 * MEMORY_FACT_CATEGORY_COUNT — the schema accepts arbitrary TEXT, so old DBs
 * tolerate additions.
 * ============================================================================= */

extern const char *const MEMORY_FACT_CATEGORIES[];
extern const int MEMORY_FACT_CATEGORY_COUNT;

/* =============================================================================
 * Memory Preference Structure
 *
 * Represents a user preference with a category and value.
 * Categories are normalized (e.g., "theme", "units", "communication_style").
 *
 * Unlike facts, preferences use upsert semantics - storing a new value for
 * an existing category updates rather than creates a new record.
 *
 * reinforcement_count tracks how many times a preference has been expressed,
 * used to increase confidence over time.
 * ============================================================================= */

typedef struct {
   int64_t id;
   int user_id;
   char category[MEMORY_CATEGORY_MAX];
   char value[MEMORY_PREF_VALUE_MAX];
   float confidence;
   char source[MEMORY_SOURCE_MAX]; /* "explicit" or "inferred" */
   time_t created_at;
   time_t updated_at;
   int reinforcement_count;
} memory_preference_t;

/* =============================================================================
 * Memory Summary Structure
 *
 * Stores a summary of a conversation session for later recall.
 * Summaries are generated during memory extraction at session end.
 *
 * Topics is a comma-separated list of main discussion topics.
 * Sentiment captures overall emotional tone ("positive", "neutral", "negative").
 * ============================================================================= */

typedef struct {
   int64_t id;
   int user_id;
   char session_id[MEMORY_SESSION_ID_MAX]; /* Links to metrics/conversation */
   char summary[MEMORY_SUMMARY_MAX];
   char topics[MEMORY_TOPICS_MAX];
   char sentiment[MEMORY_SENTIMENT_MAX];
   time_t created_at;
   int message_count;
   int duration_seconds;
   bool consolidated; /* True if rolled up into a larger summary */
} memory_summary_t;

/* =============================================================================
 * Memory Statistics Structure
 *
 * Used by memory_db_get_stats() to return aggregate counts.
 * ============================================================================= */

typedef struct {
   int fact_count;
   int pref_count;
   int summary_count;
   int entity_count;
   time_t oldest_fact;
   time_t newest_fact;
} memory_stats_t;

/* =============================================================================
 * Extraction Request Structure
 *
 * Passed to memory_trigger_extraction() with all data needed to run
 * extraction asynchronously after session ends.
 * ============================================================================= */

typedef struct {
   int user_id;
   int64_t conversation_id;
   char session_id[MEMORY_SESSION_ID_MAX];
   int message_count;
   int duration_seconds;
} memory_extraction_request_t;

/* =============================================================================
 * Extraction Result Structures
 *
 * Used to parse the JSON response from the extraction LLM.
 * ============================================================================= */

typedef struct {
   char text[MEMORY_FACT_TEXT_MAX];
   char source[MEMORY_SOURCE_MAX];
   char category[MEMORY_CATEGORY_MAX]; /* Validated against MEMORY_FACT_CATEGORIES (v34) */
   float confidence;
} memory_extracted_fact_t;

typedef struct {
   char category[MEMORY_CATEGORY_MAX];
   char value[MEMORY_PREF_VALUE_MAX];
   float confidence;
} memory_extracted_preference_t;

typedef struct {
   char old_fact[MEMORY_FACT_TEXT_MAX];
   char new_fact[MEMORY_FACT_TEXT_MAX];
} memory_extracted_correction_t;

/* =============================================================================
 * Entity Graph Structures (Phase S4)
 *
 * Entities represent named people, pets, places, organizations, and things
 * mentioned by the user. Relations capture typed links between entities
 * (e.g., "Bruno" is_a "golden retriever", "Kris" lives_in "Atlanta").
 * ============================================================================= */

#define MEMORY_ENTITY_NAME_MAX 64
#define MEMORY_ENTITY_TYPE_MAX 32
#define MEMORY_RELATION_MAX 64

typedef struct {
   int64_t id;
   int user_id;
   char name[MEMORY_ENTITY_NAME_MAX];
   char entity_type[MEMORY_ENTITY_TYPE_MAX];
   char canonical_name[MEMORY_ENTITY_NAME_MAX];
   int mention_count;
   time_t first_seen;
   time_t last_seen;
} memory_entity_t;

typedef struct {
   int64_t id;
   int64_t subject_entity_id;
   char relation[MEMORY_RELATION_MAX];
   int64_t object_entity_id;                 /* 0 if literal value */
   char object_name[MEMORY_ENTITY_NAME_MAX]; /* Resolved entity name or literal */
   float confidence;
   /* Validity period (v33).  0 = NULL in SQL = open-ended.
    * "currently true" predicate: valid_to == 0 || valid_to > now() */
   int64_t valid_from;
   int64_t valid_to;
} memory_relation_t;

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_TYPES_H */
