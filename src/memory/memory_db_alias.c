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
 * the project author(s).
 *
 * Memory Database — entity-merge / user-identity-dedup alias surface (v43).
 *
 * Implementation of the equivalence-class API declared in memory_db_aliases.h.
 * The public surface lives there; this file holds the cascade, the composite
 * scorer, the alias_link / unlink writers, and the equivalence-class-aware
 * relation listings.  Layered on top of memory_db_entity_merge() (the existing
 * hard-merge primitive) — see docs/ENTITY_MERGE_DESIGN.md §2.
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "auth/auth_db_internal.h"
#include "dawn_error.h"
#include "logging.h"
#include "memory/memory_db.h"
#include "memory/memory_db_aliases.h"
#include "memory/memory_db_entities.h"
#include "memory/memory_embeddings.h"
#include "memory/memory_types.h"

/* =============================================================================
 * Tokenizer (pure-function helpers shared by Stage 2 and the Jaccard signal)
 *
 * Whitespace + ASCII-punctuation split; tokens shorter than ALIAS_MIN_TOKEN_LEN
 * are dropped (single-letter "a" / "i" tokens swamp the Jaccard denominator
 * with noise without contributing real evidence).  Tokens are case-preserved
 * because canonical_name is already lowercased upstream by
 * memory_make_canonical_name().
 * ============================================================================= */

#define ALIAS_TOKEN_MAX 32 /* per-token byte cap (matches MEMORY_ENTITY_NAME_MAX/2) */
#define ALIAS_MAX_TOKENS 16
#define ALIAS_MIN_TOKEN_LEN 2

typedef struct {
   char tokens[ALIAS_MAX_TOKENS][ALIAS_TOKEN_MAX];
   int count;
} alias_token_set_t;

static bool is_token_separator(unsigned char c) {
   if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
      return true;
   /* ASCII punctuation characters that carve word boundaries. Apostrophes
    * stay attached because "o'brien" / "ai's" etc. shouldn't fragment. */
   switch (c) {
      case '.':
      case ',':
      case ';':
      case ':':
      case '!':
      case '?':
      case '/':
      case '\\':
      case '(':
      case ')':
      case '[':
      case ']':
      case '{':
      case '}':
      case '"':
      case '`':
      case '<':
      case '>':
      case '|':
      case '@':
      case '#':
      case '$':
      case '%':
      case '^':
      case '&':
      case '*':
      case '+':
      case '=':
      case '~':
         return true;
      default:
         return false;
   }
}

static void tokenize_canonical(const char *s, alias_token_set_t *out) {
   out->count = 0;
   if (!s)
      return;

   const char *p = s;
   while (*p && out->count < ALIAS_MAX_TOKENS) {
      while (*p && is_token_separator((unsigned char)*p)) {
         p++;
      }
      if (!*p)
         break;
      const char *start = p;
      while (*p && !is_token_separator((unsigned char)*p)) {
         p++;
      }
      size_t len = (size_t)(p - start);
      if (len >= (size_t)ALIAS_MIN_TOKEN_LEN && len < ALIAS_TOKEN_MAX) {
         memcpy(out->tokens[out->count], start, len);
         out->tokens[out->count][len] = '\0';
         out->count++;
      }
   }
}

static bool token_set_contains(const alias_token_set_t *set, const char *tok) {
   for (int i = 0; i < set->count; i++) {
      if (strcmp(set->tokens[i], tok) == 0)
         return true;
   }
   return false;
}

/* Directional overlap (Phase 1.5 Ckpt C): |tokens_a ∩ tokens_b| / |tokens_b|.
 *
 * Used at Stage 2 in the synthetic-self path: standard Jaccard penalizes
 * single-token candidates against the verbose synthetic seed (real_name +
 * aliases), e.g. candidate "jon" against a 4-token synthetic gives
 * jaccard = 1/4 = 0.25 which falls below the 0.30 floor and gets dropped.
 * Directional overlap from the candidate's perspective is 1/1 = 1.0 so
 * it survives the floor and reaches Stage 6 scoring.
 *
 * Asymmetric: callers must consistently pass the synthetic side as @p a
 * and the candidate side as @p b.  Standard Jaccard remains the right
 * choice for non-synthetic paths (resolver, extraction-time), where the
 * inbound and existing entities are both real and roughly comparable in
 * token count. */
static float compute_directional_overlap(const char *a, const char *b) {
   if (!a || !b || !*a || !*b)
      return 0.0f;

   alias_token_set_t ta, tb;
   tokenize_canonical(a, &ta);
   tokenize_canonical(b, &tb);
   if (tb.count == 0)
      return 0.0f;

   int intersection = 0;
   for (int i = 0; i < tb.count; i++) {
      if (token_set_contains(&ta, tb.tokens[i]))
         intersection++;
   }
   return (float)intersection / (float)tb.count;
}

float memory_alias_compute_name_jaccard(const char *a, const char *b) {
   if (!a || !b || !*a || !*b)
      return 0.0f;

   alias_token_set_t ta;
   alias_token_set_t tb;
   tokenize_canonical(a, &ta);
   tokenize_canonical(b, &tb);
   if (ta.count == 0 || tb.count == 0)
      return 0.0f;

   /* |A ∩ B| — count tokens of A that appear in B. */
   int intersection = 0;
   for (int i = 0; i < ta.count; i++) {
      if (token_set_contains(&tb, ta.tokens[i]))
         intersection++;
   }
   /* |A ∪ B| = |A| + |B| - |A ∩ B|.  Both sets dedup their own duplicate
    * tokens implicitly because tokenize_canonical doesn't currently dedup
    * within a name, so two repeated tokens in `a` would inflate |A|.  In
    * practice canonical_name comes from memory_make_canonical_name() which
    * is single-name lower-cased text — duplicate tokens within one name
    * (e.g. "Mary Mary Sue") are vanishingly rare and self-cancel symmetrically. */
   int union_size = ta.count + tb.count - intersection;
   if (union_size <= 0)
      return 0.0f;

   return (float)intersection / (float)union_size;
}

bool memory_alias_compute_name_substring(const char *a, const char *b) {
   if (!a || !b || !*a || !*b)
      return false;
   if (strcmp(a, b) == 0)
      return false; /* exact equality is not "substring" */
   return strstr(a, b) != NULL || strstr(b, a) != NULL;
}

/* =============================================================================
 * Type filter helpers — pure functions, no DB
 *
 * Per design §6 Stage 3 + §7: the `thing` carve-out matters for the live DB
 * shape (the LLM extracted "Jon" as `thing` and "Jonathan Smith" as
 * `person` — strict type matching would block an obvious correct merge).
 * ============================================================================= */

static bool type_is_thing(const char *t) {
   return t && strcmp(t, "thing") == 0;
}

static bool type_veto_fires(const char *a_type, const char *b_type) {
   /* Veto: both non-thing AND types differ → forced reject. */
   if (!a_type || !b_type)
      return false;
   if (type_is_thing(a_type) || type_is_thing(b_type))
      return false;
   return strcmp(a_type, b_type) != 0;
}

static int type_specificity(const char *t) {
   /* Lower-better is irrelevant here — higher = more specific.  See §9. */
   if (!t)
      return 0;
   if (strcmp(t, "person") == 0)
      return 4;
   if (strcmp(t, "place") == 0 || strcmp(t, "pet") == 0 || strcmp(t, "org") == 0)
      return 3;
   if (strcmp(t, "thing") == 0)
      return 1;
   return 0; /* unknown types rank below thing */
}

static float type_match_signal(const char *a_type, const char *b_type) {
   if (!a_type || !b_type)
      return 0.0f;
   if (type_is_thing(a_type) || type_is_thing(b_type))
      return 0.0f; /* thing/anything contributes 0 (not penalty) */
   return strcmp(a_type, b_type) == 0 ? 1.0f : 0.0f;
}

/* =============================================================================
 * Composite formula (pure function — testable without a DB)
 * ============================================================================= */

void memory_alias_apply_composite(memory_alias_evidence_t *ev) {
   if (!ev)
      return;

   if (ev->type_veto_fired) {
      ev->composite_score = 0.0f;
      return;
   }

   /* Clamp embedding cosine to [0, 1] — negatives never contribute. */
   float cos_clamped = ev->embedding_cosine;
   if (cos_clamped < 0.0f)
      cos_clamped = 0.0f;
   if (cos_clamped > 1.0f)
      cos_clamped = 1.0f;
   ev->embedding_cosine = cos_clamped;

   float composite = MEMORY_ALIAS_W_NAME_JACCARD * ev->name_jaccard +
                     MEMORY_ALIAS_W_EMBEDDING_COSINE * cos_clamped +
                     MEMORY_ALIAS_W_EXCLUSIVE_RELATION_OVERLAP * ev->exclusive_relation_overlap +
                     MEMORY_ALIAS_W_CONTACT_FIELD_OVERLAP * ev->contact_field_overlap +
                     MEMORY_ALIAS_W_TYPE_MATCH * ev->type_match;

   if (ev->name_substring_bonus_applied)
      composite += MEMORY_ALIAS_BONUS_NAME_SUBSTRING;
   if (ev->user_self_bonus_applied)
      composite += MEMORY_ALIAS_BONUS_USER_SELF;

   if (composite > 1.0f)
      composite = 1.0f;
   if (composite < 0.0f)
      composite = 0.0f;
   ev->composite_score = composite;
}

/* =============================================================================
 * canonical_priority comparators (pure — design §9)
 *
 * Two helpers: the four-axis _compare variant operates on memory_entity_t
 * alone (which doesn't carry the is_user_self column), and the five-axis
 * _compare_self sibling accepts explicit is_user_self flags and short-
 * circuits on them before delegating to the four-axis form.  Cascade call
 * sites that loaded is_user_self from the DB use the sibling; tests / pure
 * callers that don't have is_user_self context use the four-axis form
 * (and accept that two rows differing only in that flag will compare equal).
 *
 * Tuple per design §9 (DESC for is_user_self / type_specificity / mention_count,
 * ASC for first_seen / id):
 *   (is_user_self, type_specificity, mention_count, first_seen, id)
 *
 * Returns:
 *   <  0  → @p a should be canonical over b
 *    0   → fully tied (only when same id and, in _compare_self, equal flags)
 *   >  0  → @p b should be canonical over a
 * ============================================================================= */

int memory_alias_canonical_priority_compare(const memory_entity_t *a, const memory_entity_t *b) {
   if (!a && !b)
      return 0;
   if (!a)
      return 1;
   if (!b)
      return -1;

   /* type_specificity DESC, mention_count DESC, first_seen ASC, id ASC.
    * is_user_self is not present here — see _compare_self for the full tuple. */
   int ts_a = type_specificity(a->entity_type);
   int ts_b = type_specificity(b->entity_type);
   if (ts_a != ts_b)
      return ts_b - ts_a; /* higher type_specificity wins → a < b iff ts_a > ts_b */

   if (a->mention_count != b->mention_count)
      return b->mention_count - a->mention_count;

   if (a->first_seen != b->first_seen)
      return (a->first_seen < b->first_seen) ? -1 : 1; /* older first_seen wins */

   if (a->id != b->id)
      return (a->id < b->id) ? -1 : 1;

   return 0;
}

int memory_alias_canonical_priority_compare_self(const memory_entity_t *a,
                                                 bool a_is_user_self,
                                                 const memory_entity_t *b,
                                                 bool b_is_user_self) {
   if (!a && !b)
      return 0;
   if (!a)
      return 1;
   if (!b)
      return -1;

   /* is_user_self DESC short-circuits the lex tuple. */
   if (a_is_user_self != b_is_user_self)
      return a_is_user_self ? -1 : 1;

   return memory_alias_canonical_priority_compare(a, b);
}

int memory_alias_row_compare_by_composite_desc(const void *a, const void *b) {
   const memory_alias_link_user_self_row_t *ra = (const memory_alias_link_user_self_row_t *)a;
   const memory_alias_link_user_self_row_t *rb = (const memory_alias_link_user_self_row_t *)b;
   /* composite_score DESC */
   if (ra->composite_score > rb->composite_score)
      return -1;
   if (ra->composite_score < rb->composite_score)
      return 1;
   /* entity_id ASC tiebreak — deterministic ordering for equal-score rows. */
   if (ra->entity_id < rb->entity_id)
      return -1;
   if (ra->entity_id > rb->entity_id)
      return 1;
   return 0;
}

/* =============================================================================
 * DB-accessing helpers
 *
 * All take the auth_db lock per call — the alias surface is invoked at
 * extraction time (off the conversational hot path) and from the operator
 * paths (link-user-self, manual merge), so per-call lock acquisition is
 * the right granularity.  No caller holds the lock across helper calls.
 * ============================================================================= */

/* Load a single entity by id, plus its canonical_id and is_user_self flag.
 * Returns MEMORY_DB_SUCCESS / MEMORY_DB_NOT_FOUND / MEMORY_DB_FAILURE.
 * canonical_id_out: 0 = NULL = self is canonical; > 0 = soft alias of that id.
 * is_user_self_out: optional (NULL OK). */
static int load_entity_full(int user_id,
                            int64_t entity_id,
                            memory_entity_t *out_entity,
                            int64_t *out_canonical_id,
                            bool *out_is_user_self) {
   if (!out_entity || entity_id <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = NULL;
   const char *sql = "SELECT id, user_id, name, entity_type, canonical_name, "
                     "       mention_count, first_seen, COALESCE(last_seen, 0), "
                     "       canonical_id, is_user_self "
                     "FROM memory_entities WHERE id = ? AND user_id = ?";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int64(stmt, 1, entity_id);
   sqlite3_bind_int(stmt, 2, user_id);

   int rc = sqlite3_step(stmt);
   if (rc == SQLITE_DONE) {
      sqlite3_finalize(stmt);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_NOT_FOUND;
   }
   if (rc != SQLITE_ROW) {
      sqlite3_finalize(stmt);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   memset(out_entity, 0, sizeof(*out_entity));
   out_entity->id = sqlite3_column_int64(stmt, 0);
   out_entity->user_id = sqlite3_column_int(stmt, 1);
   const char *name = (const char *)sqlite3_column_text(stmt, 2);
   const char *etype = (const char *)sqlite3_column_text(stmt, 3);
   const char *canon = (const char *)sqlite3_column_text(stmt, 4);
   if (name) {
      strncpy(out_entity->name, name, MEMORY_ENTITY_NAME_MAX - 1);
      out_entity->name[MEMORY_ENTITY_NAME_MAX - 1] = '\0';
   }
   if (etype) {
      strncpy(out_entity->entity_type, etype, MEMORY_ENTITY_TYPE_MAX - 1);
      out_entity->entity_type[MEMORY_ENTITY_TYPE_MAX - 1] = '\0';
   }
   if (canon) {
      strncpy(out_entity->canonical_name, canon, MEMORY_ENTITY_NAME_MAX - 1);
      out_entity->canonical_name[MEMORY_ENTITY_NAME_MAX - 1] = '\0';
   }
   out_entity->mention_count = sqlite3_column_int(stmt, 5);
   out_entity->first_seen = (time_t)sqlite3_column_int64(stmt, 6);
   out_entity->last_seen = (time_t)sqlite3_column_int64(stmt, 7);

   if (out_canonical_id) {
      *out_canonical_id = (sqlite3_column_type(stmt, 8) == SQLITE_NULL)
                              ? 0
                              : sqlite3_column_int64(stmt, 8);
   }
   if (out_is_user_self) {
      *out_is_user_self = sqlite3_column_int(stmt, 9) != 0;
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return MEMORY_DB_SUCCESS;
}

/* Look up the user's username for the user_self_bonus signal.  Output is
 * lowercase-canonicalized for substring comparison against canonical_name. */
static int get_user_username_canonical(int user_id, char *out_buf, size_t buf_size) {
   if (!out_buf || buf_size == 0)
      return MEMORY_DB_FAILURE;
   out_buf[0] = '\0';

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, "SELECT username FROM users WHERE id = ?", -1, &stmt, NULL) !=
       SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);

   int rc = sqlite3_step(stmt);
   if (rc == SQLITE_ROW) {
      const char *u = (const char *)sqlite3_column_text(stmt, 0);
      if (u) {
         memory_make_canonical_name(u, out_buf, buf_size);
      }
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   return (out_buf[0] != '\0') ? MEMORY_DB_SUCCESS : MEMORY_DB_NOT_FOUND;
}

/* Find the user-self entity id for @p user_id (rows with is_user_self = 1).
 * Returns id > 0 on SUCCESS; 0 with NOT_FOUND if seeding hasn't run. */
static int find_user_self_id(int user_id, int64_t *out_id) {
   if (!out_id)
      return MEMORY_DB_FAILURE;
   *out_id = 0;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT id FROM memory_entities "
                          "WHERE user_id = ? AND is_user_self = 1 LIMIT 1",
                          -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   int rc = sqlite3_step(stmt);
   if (rc == SQLITE_ROW) {
      *out_id = sqlite3_column_int64(stmt, 0);
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   return (*out_id > 0) ? MEMORY_DB_SUCCESS : MEMORY_DB_NOT_FOUND;
}

/* Compute exclusive_relation_overlap signal between two entities.
 *   1.0 if any open exclusive relation shares object_entity_id (or
 *       canonical-equal object_value)
 *   0.5 if any non-exclusive relation shares object
 *   0   otherwise
 * Uses the partial index idx_memory_relations_subject_open. */
static float compute_exclusive_relation_overlap(int user_id, int64_t a_id, int64_t b_id) {
   if (a_id <= 0 || b_id <= 0 || a_id == b_id)
      return 0.0f;

   AUTH_DB_LOCK_OR_RETURN(0.0f);

   /* Single-statement fetch covering both subjects via IN (?, ?) — the
    * Ckpt 5 fold-in for emb-M2.  One prepare + one step loop instead of
    * two bind cycles; the discriminator column (subject_entity_id) routes
    * each row into a_rels[] or b_rels[].
    *
    * LIMIT 32 = 16-per-side × 2 sides.  ORDER BY valid_from DESC so when
    * a subject has more open relations than the per-side cap, we keep the
    * most recently established ones (relation history matters less than
    * "what's true now" for alias detection — fold-in arch-L3). */
   typedef struct {
      char relation[MEMORY_RELATION_MAX];
      int64_t object_entity_id;
      char object_value[MEMORY_ENTITY_NAME_MAX]; /* literal, may be empty */
   } open_rel_t;

   open_rel_t a_rels[16];
   int a_count = 0;
   open_rel_t b_rels[16];
   int b_count = 0;

   const char *sql = "SELECT subject_entity_id, relation, COALESCE(object_entity_id, 0), "
                     "       COALESCE(object_value, '') "
                     "FROM memory_relations "
                     "WHERE user_id = ? AND subject_entity_id IN (?, ?) "
                     "  AND valid_to IS NULL "
                     "ORDER BY COALESCE(valid_from, 0) DESC "
                     "LIMIT 32";
   sqlite3_stmt *stmt = NULL;

   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return 0.0f;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, a_id);
   sqlite3_bind_int64(stmt, 3, b_id);

   while (sqlite3_step(stmt) == SQLITE_ROW) {
      int64_t subj = sqlite3_column_int64(stmt, 0);
      open_rel_t *dest;
      int *dest_count;
      if (subj == a_id) {
         dest = a_rels;
         dest_count = &a_count;
      } else if (subj == b_id) {
         dest = b_rels;
         dest_count = &b_count;
      } else {
         continue; /* defensive — shouldn't happen given the IN filter */
      }
      if (*dest_count >= 16)
         continue;

      const char *rel = (const char *)sqlite3_column_text(stmt, 1);
      if (!rel)
         continue;
      strncpy(dest[*dest_count].relation, rel, MEMORY_RELATION_MAX - 1);
      dest[*dest_count].relation[MEMORY_RELATION_MAX - 1] = '\0';
      dest[*dest_count].object_entity_id = sqlite3_column_int64(stmt, 2);
      const char *ov = (const char *)sqlite3_column_text(stmt, 3);
      if (ov) {
         strncpy(dest[*dest_count].object_value, ov, MEMORY_ENTITY_NAME_MAX - 1);
         dest[*dest_count].object_value[MEMORY_ENTITY_NAME_MAX - 1] = '\0';
      } else {
         dest[*dest_count].object_value[0] = '\0';
      }
      (*dest_count)++;
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   float best = 0.0f;
   for (int i = 0; i < a_count; i++) {
      for (int j = 0; j < b_count; j++) {
         if (strcmp(a_rels[i].relation, b_rels[j].relation) != 0)
            continue;

         /* Same relation type — now check object equality. */
         bool object_match = false;
         if (a_rels[i].object_entity_id != 0 && b_rels[j].object_entity_id != 0 &&
             a_rels[i].object_entity_id == b_rels[j].object_entity_id) {
            object_match = true;
         } else if (a_rels[i].object_value[0] != '\0' && b_rels[j].object_value[0] != '\0' &&
                    strcasecmp(a_rels[i].object_value, b_rels[j].object_value) == 0) {
            object_match = true;
         }
         if (!object_match)
            continue;

         float weight = memory_db_relation_is_exclusive(a_rels[i].relation) ? 1.0f : 0.5f;
         if (weight > best)
            best = weight;
         if (best >= 1.0f)
            return best;
      }
   }
   return best;
}

/* Compute contact_field_overlap signal: 1.0 if any contact email/phone is
 * shared between the two entities (after normalization), 0 otherwise. */
static float compute_contact_field_overlap(int user_id, int64_t a_id, int64_t b_id) {
   if (a_id <= 0 || b_id <= 0 || a_id == b_id)
      return 0.0f;

   AUTH_DB_LOCK_OR_RETURN(0.0f);

   const char *sql = "SELECT COUNT(*) FROM contacts c1 "
                     "JOIN contacts c2 ON c1.field_type = c2.field_type "
                     "                AND lower(c1.value) = lower(c2.value) "
                     "WHERE c1.user_id = ? AND c1.entity_id = ? "
                     "  AND c2.user_id = ? AND c2.entity_id = ? "
                     "  AND c1.field_type IN ('email', 'phone', 'address')";

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return 0.0f;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, a_id);
   sqlite3_bind_int(stmt, 3, user_id);
   sqlite3_bind_int64(stmt, 4, b_id);

   float overlap = 0.0f;
   if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) > 0) {
      overlap = 1.0f;
   }

   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return overlap;
}

/* Forward decl — definition lives further down (build_synthetic_self_entity
 * area).  Used here in user_self_bonus_applies for the alias-substring
 * branch (Phase 1.5 fold-in #2). */
static void trim_trailing_partial_utf8(char *buf);

/* Parse identity_aliases (newline-separated, raw user input) into a
 * canonicalized list.  Each non-empty line is trimmed, run through
 * memory_make_canonical_name, deduped case-insensitively against earlier
 * entries, and stored in @p out[i].  Modifies @p raw in place via
 * strtok_r — caller must pass a writable buffer. */
static void parse_canonical_alias_list(char *raw,
                                       char out[][MEMORY_ENTITY_NAME_MAX],
                                       int max,
                                       int *out_count) {
   *out_count = 0;
   if (!raw || !*raw || max <= 0)
      return;

   char *save = NULL;
   for (char *line = strtok_r(raw, "\n", &save); line != NULL && *out_count < max;
        line = strtok_r(NULL, "\n", &save)) {
      while (*line == ' ' || *line == '\t' || *line == '\r')
         line++;
      char *end = line + strlen(line);
      while (end > line && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
         end--;
      *end = '\0';
      if (*line == '\0')
         continue;

      char canon[MEMORY_ENTITY_NAME_MAX];
      memory_make_canonical_name(line, canon, sizeof(canon));
      if (!canon[0])
         continue;

      bool dup = false;
      for (int i = 0; i < *out_count; i++) {
         if (strcmp(out[i], canon) == 0) {
            dup = true;
            break;
         }
      }
      if (dup)
         continue;

      strncpy(out[*out_count], canon, MEMORY_ENTITY_NAME_MAX - 1);
      out[*out_count][MEMORY_ENTITY_NAME_MAX - 1] = '\0';
      (*out_count)++;
   }
}

/* Determine whether the user_self_bonus applies between two entities.
 * Bonus fires when one side has is_user_self = 1 AND the other side has
 * user-identity signal — any of (cheap-to-expensive):
 *   1. The "other" side is an allow-listed self-reference token (the
 *      "user" canonical entity, flagged at synth_self_allow_list_user
 *      time).  Unconditional; fires regardless of operator config.
 *   2. contact_overlap_fired (email/phone overlap on the contact_t
 *      surface — strong identity signal).
 *   3. The operator's username canonical is a substring of the other
 *      side's canonical_name.  Catches dev=username case directly.
 *   4. One of the operator's seeded identity_aliases (from
 *      users.identity_aliases) is a substring of the other side's
 *      canonical_name.  Catches the realistic Path A case where the
 *      operator's username is generic ("admin") but they've configured
 *      aliases like "Jon" — "Jon" is a substring of "Jon Smith" and
 *      of candidate-name "jon", so the bonus fires for both.
 * Conditions ordered cheapest-first; alias-substring last because it
 * does an extra DB load + parse vs the username path's single load. */
static bool user_self_bonus_applies(int user_id,
                                    bool a_is_user_self,
                                    bool b_is_user_self,
                                    const char *a_canonical_name,
                                    const char *b_canonical_name,
                                    bool contact_overlap_fired,
                                    bool other_is_allow_list_token) {
   /* Exactly one side must be user_self.  If neither or both, no bonus. */
   if (a_is_user_self == b_is_user_self)
      return false;

   const char *other_canon = a_is_user_self ? b_canonical_name : a_canonical_name;
   if (!other_canon || !*other_canon)
      return false;

   /* (1) Allow-listed self-reference token (currently just "user"): the
    * Phase 1.5 brief specifies this branch fires regardless of any
    * username/alias substring conditions, so the dev's actual case
    * (username "admin" or "jon" with a "user" entity in the cluster)
    * still receives the bonus. */
   if (other_is_allow_list_token)
      return true;

   /* (2) Email / phone overlap is a strong user-identity signal. */
   if (contact_overlap_fired)
      return true;

   /* (3) Username substring match on canonical_name. */
   char username_canonical[MEMORY_ENTITY_NAME_MAX];
   if (get_user_username_canonical(user_id, username_canonical, sizeof(username_canonical)) ==
           MEMORY_DB_SUCCESS &&
       *username_canonical && strstr(other_canon, username_canonical) != NULL) {
      return true;
   }

   /* (4) Identity-alias substring match.  Loads users.identity_aliases,
    * parses + canonicalizes, checks each non-empty alias as a substring
    * of @p other_canon.  This is the headline Path-A path for clusters
    * like "Jon" / "Jon Smith" / "smithfabrications@example.com" when
    * the operator has configured identity_aliases via WebUI Settings →
    * User → Aliases. */
   auth_user_identity_t identity;
   if (auth_db_get_user_identity(user_id, &identity) == AUTH_DB_SUCCESS &&
       identity.identity_aliases[0] != '\0') {
      trim_trailing_partial_utf8(identity.identity_aliases);
      char aliases[16][MEMORY_ENTITY_NAME_MAX];
      int alias_count = 0;
      parse_canonical_alias_list(identity.identity_aliases, aliases, 16, &alias_count);
      for (int i = 0; i < alias_count; i++) {
         if (aliases[i][0] && strstr(other_canon, aliases[i]) != NULL) {
            return true;
         }
      }
   }

   return false;
}

/* =============================================================================
 * Pair scoring — used by score_pair, consider_auto_merge, and the WebUI
 * preview surface (Ckpt 5).
 *
 * Synthetic-entity contract: callers may pass a memory_entity_t with
 * id == 0 to represent a not-yet-materialized canonical (the dry-run-
 * with-no-existing-self path uses this).  All DB-touching helpers below
 * (compute_exclusive_relation_overlap, compute_contact_field_overlap,
 * memory_embeddings_entity_cosine) MUST short-circuit on id <= 0 and
 * leave the corresponding signal at 0.  Any future signal that probes DB
 * by id must follow the same rule.
 * ============================================================================= */

static void score_pair_full(int user_id,
                            const memory_entity_t *src,
                            bool src_is_user_self,
                            const memory_entity_t *tgt,
                            bool tgt_is_user_self,
                            bool other_is_allow_list_token,
                            memory_alias_evidence_t *out) {
   memset(out, 0, sizeof(*out));

   /* Veto first — short-circuits everything else. */
   out->type_veto_fired = type_veto_fires(src->entity_type, tgt->entity_type);
   if (out->type_veto_fired) {
      memory_alias_apply_composite(out);
      return;
   }

   /* Name signals. */
   out->name_jaccard = memory_alias_compute_name_jaccard(src->canonical_name, tgt->canonical_name);
   out->name_substring_bonus_applied = memory_alias_compute_name_substring(src->canonical_name,
                                                                           tgt->canonical_name);

   /* Type signal. */
   out->type_match = type_match_signal(src->entity_type, tgt->entity_type);

   /* Stage 5 signals — relation + contact overlap.  Each takes the auth_db
    * lock independently; never nested. */
   out->exclusive_relation_overlap = compute_exclusive_relation_overlap(user_id, src->id, tgt->id);
   out->contact_field_overlap = compute_contact_field_overlap(user_id, src->id, tgt->id);

   /* Embedding cosine — gracefully zero if the engine isn't available
    * (tests run without ONNX) or if either side lacks an embedding.  We
    * embed the source canonical_name once and compare against the target's
    * cached vector. */
   if (memory_embeddings_available()) {
      float query_emb[MAX_EMBEDDING_DIMS];
      int dims = 0;
      if (memory_embeddings_embed(src->canonical_name, query_emb, &dims) == 0 && dims > 0) {
         float norm = memory_embeddings_l2_norm(query_emb, dims);
         float cosine = 0.0f;
         if (memory_embeddings_entity_cosine(user_id, tgt->id, query_emb, dims, norm, &cosine) ==
             SUCCESS) {
            out->embedding_cosine = cosine;
         }
      }
   }

   /* User-self bonus.  Pair-score callers that don't traffic in allow-
    * list tokens (the public memory_db_entity_score_pair WebUI preview)
    * pass false; link-user-self's synth-self path passes true when the
    * candidate is the canonical-name='user' allow-list token. */
   out->user_self_bonus_applied = user_self_bonus_applies(
       user_id, src_is_user_self, tgt_is_user_self, src->canonical_name, tgt->canonical_name,
       out->contact_field_overlap > 0.0f, other_is_allow_list_token);

   memory_alias_apply_composite(out);
}

int memory_db_entity_score_pair(int user_id,
                                int64_t source_id,
                                int64_t target_id,
                                memory_alias_evidence_t *out_evidence) {
   if (!out_evidence || source_id <= 0 || target_id <= 0)
      return MEMORY_DB_FAILURE;
   if (source_id == target_id) {
      memset(out_evidence, 0, sizeof(*out_evidence));
      return MEMORY_DB_FAILURE;
   }

   memory_entity_t src, tgt;
   int64_t src_canonical_id = 0, tgt_canonical_id = 0;
   bool src_is_self = false, tgt_is_self = false;
   int rc = load_entity_full(user_id, source_id, &src, &src_canonical_id, &src_is_self);
   if (rc != MEMORY_DB_SUCCESS)
      return rc;
   rc = load_entity_full(user_id, target_id, &tgt, &tgt_canonical_id, &tgt_is_self);
   if (rc != MEMORY_DB_SUCCESS)
      return rc;

   /* Refuse to score a pair where either entity is itself a soft alias.
    * The canonical-only entity-embedding cache means
    * memory_embeddings_entity_cosine() returns 0 for alias rows, which
    * would silently mis-route the band decision in the WebUI two-click
    * preview.  Surface a distinct return code so callers can prompt the
    * operator to pick the canonical instead. */
   if (src_canonical_id > 0 || tgt_canonical_id > 0) {
      memset(out_evidence, 0, sizeof(*out_evidence));
      return MEMORY_DB_INVALID_ALIAS_TARGET;
   }

   score_pair_full(user_id, &src, src_is_self, &tgt, tgt_is_self,
                   /* other_is_allow_list_token */ false, out_evidence);
   return MEMORY_DB_SUCCESS;
}

/* =============================================================================
 * Cascade — Stage 1 through Stage 6 per design §6
 *
 * Two entry points:
 *   - resolve_alias()      : surface-form lookup, no inbound entity_id.  Stage
 *                            5 contributes 0 (nothing to compare on the
 *                            inbound side).  Returns the canonical the surface
 *                            form should bind to (or 0 = no resolution).
 *   - consider_auto_merge() : a freshly-created entity is the inbound; full
 *                             cascade including Stage 5 against existing rows.
 * ============================================================================= */

/* Stage 1: exact canonical_name match.  Returns the resolved canonical id
 * (post-COALESCE on canonical_id) on hit, or 0 on miss. */
static int stage1_exact_match(int user_id,
                              const char *canonical_name,
                              int64_t exclude_id,
                              int64_t *out_resolved_id) {
   *out_resolved_id = 0;
   if (!canonical_name || !*canonical_name)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = NULL;
   const char *sql = "SELECT id, COALESCE(canonical_id, id) FROM memory_entities "
                     "WHERE user_id = ? AND canonical_name = ?";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, canonical_name, -1, SQLITE_TRANSIENT);

   int64_t resolved = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      int64_t hit_id = sqlite3_column_int64(stmt, 0);
      int64_t canonical = sqlite3_column_int64(stmt, 1);
      if (hit_id != exclude_id) {
         resolved = canonical;
      }
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   *out_resolved_id = resolved;
   return MEMORY_DB_SUCCESS;
}

/* Stage 2 helper: per-token candidate generator using existing keyword search.
 * Aggregates up to MEMORY_ALIAS_STAGE2_MAX_CANDIDATES unique IDs across all
 * tokens, computes name_jaccard for each, drops below floor. */
typedef struct {
   int64_t entity_id;
   char canonical_name[MEMORY_ENTITY_NAME_MAX];
   char entity_type[MEMORY_ENTITY_TYPE_MAX];
   int mention_count;
   time_t first_seen;
   bool is_user_self;
   /* Allow-listed self-reference token (currently just "user").  Receives
    * user_self_bonus regardless of username/alias substring conditions
    * per the Phase 1.5 design intent — synth_self_allow_list_user() sets
    * this true; standard Stage 2 candidates leave it false. */
   bool is_user_self_token;
   float name_jaccard;
   float embedding_cosine;
} alias_candidate_t;

static int stage2_candidates(int user_id,
                             const char *canonical_name,
                             int64_t exclude_id,
                             bool use_synth_self,
                             alias_candidate_t *out,
                             int max,
                             int *out_count) {
   /* Append-mode: respect any pre-added candidates (e.g. the synthetic-
    * self "user" allow-list).  Callers that want a fresh pool zero
    * *out_count before calling. */
   if (!canonical_name || !*canonical_name || max <= 0)
      return MEMORY_DB_SUCCESS; /* empty result, not an error */

   alias_token_set_t tokens;
   tokenize_canonical(canonical_name, &tokens);
   if (tokens.count == 0)
      return MEMORY_DB_SUCCESS;

   /* Per-token keyword search via the existing entity-search helper.
    * Aggregate unique IDs, capped at @p max.  Seed seen_ids with any
    * pre-added candidates so we don't double-add the same entity. */
   int64_t seen_ids[MEMORY_ALIAS_STAGE2_MAX_CANDIDATES];
   int seen_count = 0;
   for (int p = 0; p < *out_count && p < MEMORY_ALIAS_STAGE2_MAX_CANDIDATES; p++) {
      seen_ids[seen_count++] = out[p].entity_id;
   }

   for (int t = 0; t < tokens.count && seen_count < max; t++) {
      memory_entity_t hits[8];
      int hit_count = 0;
      if (memory_db_entity_search(user_id, tokens.tokens[t], hits, 8, &hit_count) !=
          MEMORY_DB_SUCCESS) {
         continue;
      }
      for (int i = 0; i < hit_count && seen_count < max; i++) {
         if (hits[i].id == exclude_id)
            continue;
         /* Dedup against seen_ids. */
         bool dup = false;
         for (int s = 0; s < seen_count; s++) {
            if (seen_ids[s] == hits[i].id) {
               dup = true;
               break;
            }
         }
         if (dup)
            continue;
         seen_ids[seen_count++] = hits[i].id;
      }
   }

   /* Score each candidate by jaccard; drop below floor; populate `out`. */
   for (int i = 0; i < seen_count; i++) {
      memory_entity_t e;
      bool is_self = false;
      int64_t canon_id = 0;
      if (load_entity_full(user_id, seen_ids[i], &e, &canon_id, &is_self) != MEMORY_DB_SUCCESS) {
         continue;
      }
      /* Skip alias rows — they're not canonical candidates.  (At Ckpt 2
       * time no aliases exist in production yet, but link-user-self in
       * Ckpt 4 will create them, so the filter belongs here too — matches
       * design §15's "cache loader filter" cohort, applied at the resolver
       * level since the cache filter ships in Ckpt 3.) */
      if (canon_id != 0)
         continue;

      /* Phase 1.5 Ckpt C: directional overlap when scoring against the
       * synthetic-self seed (verbose canonical from real_name + aliases),
       * standard Jaccard otherwise.  Both apply the same 0.30 floor. */
      float overlap;
      if (use_synth_self) {
         overlap = compute_directional_overlap(canonical_name, e.canonical_name);
      } else {
         overlap = memory_alias_compute_name_jaccard(canonical_name, e.canonical_name);
      }
      if (overlap < MEMORY_ALIAS_NAME_JACCARD_FLOOR)
         continue;

      alias_candidate_t *c = &out[*out_count];
      c->entity_id = e.id;
      strncpy(c->canonical_name, e.canonical_name, MEMORY_ENTITY_NAME_MAX - 1);
      c->canonical_name[MEMORY_ENTITY_NAME_MAX - 1] = '\0';
      strncpy(c->entity_type, e.entity_type, MEMORY_ENTITY_TYPE_MAX - 1);
      c->entity_type[MEMORY_ENTITY_TYPE_MAX - 1] = '\0';
      c->mention_count = e.mention_count;
      c->first_seen = e.first_seen;
      c->is_user_self = is_self;
      c->name_jaccard = overlap;
      c->embedding_cosine = 0.0f;
      (*out_count)++;
   }
   return MEMORY_DB_SUCCESS;
}

/* Stage 3: type-filter survivors in place.  Compacts the array by removing
 * candidates that fail the type-match rule (`thing` carve-out applies). */
static void stage3_type_filter(const char *inbound_type, alias_candidate_t *cands, int *count) {
   int kept = 0;
   for (int i = 0; i < *count; i++) {
      if (type_veto_fires(inbound_type, cands[i].entity_type)) {
         continue; /* drop */
      }
      if (kept != i) {
         cands[kept] = cands[i];
      }
      kept++;
   }
   *count = kept;
}

/* Stage 4: embed inbound canonical_name once, compute cosine vs each
 * candidate's cached embedding, drop those below the cosine floor.
 * Caps the survivor list at MEMORY_ALIAS_STAGE4_MAX_CANDIDATES. */
static void stage4_cosine(int user_id,
                          const char *canonical_name,
                          alias_candidate_t *cands,
                          int *count) {
   if (!memory_embeddings_available()) {
      /* No engine — Stage 4 is a no-op; pass survivors through unchanged
       * but truncate to cap so Stage 5 stays bounded. */
      if (*count > MEMORY_ALIAS_STAGE4_MAX_CANDIDATES)
         *count = MEMORY_ALIAS_STAGE4_MAX_CANDIDATES;
      return;
   }

   float query_emb[MAX_EMBEDDING_DIMS];
   int dims = 0;
   if (memory_embeddings_embed(canonical_name, query_emb, &dims) != 0 || dims <= 0) {
      if (*count > MEMORY_ALIAS_STAGE4_MAX_CANDIDATES)
         *count = MEMORY_ALIAS_STAGE4_MAX_CANDIDATES;
      return;
   }
   float norm = memory_embeddings_l2_norm(query_emb, dims);

   int kept = 0;
   for (int i = 0; i < *count; i++) {
      float cos = 0.0f;
      if (memory_embeddings_entity_cosine(user_id, cands[i].entity_id, query_emb, dims, norm,
                                          &cos) == SUCCESS) {
         cands[i].embedding_cosine = cos;
      } else {
         /* No cached embedding for this candidate — score 0. */
         cands[i].embedding_cosine = 0.0f;
      }
      if (cands[i].embedding_cosine < MEMORY_ALIAS_COSINE_FLOOR)
         continue;
      if (kept != i)
         cands[kept] = cands[i];
      kept++;
   }
   *count = kept;
   if (*count > MEMORY_ALIAS_STAGE4_MAX_CANDIDATES)
      *count = MEMORY_ALIAS_STAGE4_MAX_CANDIDATES;
}

/* Sort candidates by partial composite (name_jaccard + embedding_cosine)
 * descending, then truncate to top N for Stage 5. */
static void rank_and_truncate(alias_candidate_t *cands, int *count, int top_n) {
   /* Insertion sort — N <= 8, simplest and warm for tiny arrays. */
   for (int i = 1; i < *count; i++) {
      alias_candidate_t tmp = cands[i];
      float key = tmp.name_jaccard + tmp.embedding_cosine;
      int j = i - 1;
      while (j >= 0 && (cands[j].name_jaccard + cands[j].embedding_cosine) < key) {
         cands[j + 1] = cands[j];
         j--;
      }
      cands[j + 1] = tmp;
   }
   if (*count > top_n)
      *count = top_n;
}

/* Internal cascade.
 * @p inbound_id: 0 if the inbound has no row yet (resolver pre-upsert),
 *                else the id of the inbound entity (consider_auto_merge).
 *                When non-zero, Stage 5 fires (full overlap signals).
 * @p inbound_is_user_self: only relevant when inbound_id != 0; ignored otherwise.
 *
 * Picks the highest-composite candidate; out_winner_id = 0 on no match.
 * out_winner_evidence holds the final composite for routing. */
/* Phase 1.5 Ckpt C: pre-add the canonical-name='user' entity to the
 * candidate pool when running synthetic-self resolve.
 *
 *   DB scan against /var/lib/dawn/auth.db (May 2026) found exactly one
 *   self-reference entity: 'user' (thing, 292 mentions).  No 'me' /
 *   'myself' / 'admin' / 'operator' / etc. exist as entities.  Extending
 *   this list requires evidence from a real DB scan, not theoretical
 *   alternatives.
 *
 * Adds the row before Stage 2 so it survives the directional-overlap
 * floor (which it would fail by name signal alone — "user" has no token
 * overlap with a real_name like "Jonathan Smith").  Stage 3 then
 * applies the type-veto rule (the carve-out for 'thing' type lets it
 * through against 'person' synthetics), Stage 4-6 apply normal scoring
 * + user_self_bonus.  If the entity doesn't exist in this user's graph
 * the function is a no-op. */
static void synth_self_allow_list_user(int user_id,
                                       int64_t exclude_id,
                                       alias_candidate_t *out,
                                       int max,
                                       int *count) {
   if (!out || max <= 0 || *count >= max)
      return;

   AUTH_DB_LOCK_OR_RETURN_VOID();
   sqlite3_stmt *stmt = NULL;
   const char *sql = "SELECT id FROM memory_entities "
                     "WHERE user_id = ? AND lower(canonical_name) = 'user' "
                     "  AND canonical_id IS NULL "
                     "LIMIT 1";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   int64_t hit_id = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW) {
      hit_id = sqlite3_column_int64(stmt, 0);
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   if (hit_id <= 0 || hit_id == exclude_id)
      return;

   /* Skip if already in pool. */
   for (int i = 0; i < *count; i++) {
      if (out[i].entity_id == hit_id)
         return;
   }

   /* Load full row + push.  load_entity_full takes its own lock. */
   memory_entity_t e;
   bool is_self = false;
   int64_t canon_id = 0;
   if (load_entity_full(user_id, hit_id, &e, &canon_id, &is_self) != MEMORY_DB_SUCCESS)
      return;
   if (canon_id != 0)
      return; /* alias row — not a canonical candidate */

   alias_candidate_t *c = &out[*count];
   memset(c, 0, sizeof(*c));
   c->entity_id = e.id;
   strncpy(c->canonical_name, e.canonical_name, MEMORY_ENTITY_NAME_MAX - 1);
   c->canonical_name[MEMORY_ENTITY_NAME_MAX - 1] = '\0';
   strncpy(c->entity_type, e.entity_type, MEMORY_ENTITY_TYPE_MAX - 1);
   c->entity_type[MEMORY_ENTITY_TYPE_MAX - 1] = '\0';
   c->mention_count = e.mention_count;
   c->first_seen = e.first_seen;
   c->is_user_self = is_self;
   /* Allow-listed self-reference token — Stage 6 will fire user_self_bonus
    * unconditionally (regardless of username substring or alias match).
    * Matches the Phase 1.5 brief's "passes through Stage 3-6 normally
    * with normal scoring + user_self_bonus" requirement. */
   c->is_user_self_token = true;
   /* Score via directional overlap so the entity isn't penalized by
    * name-signal at Stages 5-6.  For "user" against any verbose
    * synthetic, intersection = 0 → overlap = 0; the user_self_bonus
    * carries it instead. */
   c->name_jaccard = 0.0f;
   c->embedding_cosine = 0.0f;
   (*count)++;
}

static int cascade_internal(int user_id,
                            const char *canonical_name,
                            const char *entity_type,
                            int64_t inbound_id,
                            bool inbound_is_user_self,
                            bool use_synth_self,
                            int64_t *out_winner_id,
                            int *out_matched_stage,
                            memory_alias_evidence_t *out_evidence) {
   *out_winner_id = 0;
   *out_matched_stage = 0;
   memset(out_evidence, 0, sizeof(*out_evidence));

   /* Stage 1: exact canonical_name match (resolver-only fast path). */
   int64_t exact_id = 0;
   if (stage1_exact_match(user_id, canonical_name, inbound_id, &exact_id) == MEMORY_DB_SUCCESS &&
       exact_id > 0) {
      *out_winner_id = exact_id;
      *out_matched_stage = 1;
      /* Stage 1 hit — composite N/A; caller short-circuits without the
       * threshold band check. */
      return MEMORY_DB_SUCCESS;
   }

   /* Stage 2: token-Jaccard (or directional overlap, when synth-self)
    * candidate generation.  When synth-self, also pre-add the "user"
    * canonical-name allow-list entity. */
   alias_candidate_t cands[MEMORY_ALIAS_STAGE2_MAX_CANDIDATES];
   int cand_count = 0;
   if (use_synth_self) {
      synth_self_allow_list_user(user_id, inbound_id, cands, MEMORY_ALIAS_STAGE2_MAX_CANDIDATES,
                                 &cand_count);
   }
   stage2_candidates(user_id, canonical_name, inbound_id, use_synth_self, cands,
                     MEMORY_ALIAS_STAGE2_MAX_CANDIDATES, &cand_count);
   if (cand_count == 0)
      return MEMORY_DB_SUCCESS; /* clean miss */

   /* Stage 3: type filter (in-place). */
   stage3_type_filter(entity_type, cands, &cand_count);
   if (cand_count == 0)
      return MEMORY_DB_SUCCESS;

   /* Stage 4: embedding cosine.  Drops cands < 0.50, caps at 8. */
   stage4_cosine(user_id, canonical_name, cands, &cand_count);
   if (cand_count == 0)
      return MEMORY_DB_SUCCESS;

   /* Rank by partial composite (jaccard + cosine), keep top 3 for Stage 5. */
   rank_and_truncate(cands, &cand_count, MEMORY_ALIAS_STAGE5_MAX_CANDIDATES);

   /* Stage 5 + Stage 6: full pair-score each candidate, keep best composite.
    * On exact composite ties — usually the rare case where multiple
    * candidates clear the auto-merge threshold simultaneously — break the
    * tie with canonical_priority_compare_self per design §9, so the
    * binding is order-independent regardless of insertion order. */
   float best_composite = -1.0f;
   int best_idx = -1;
   memory_alias_evidence_t best_ev;
   memset(&best_ev, 0, sizeof(best_ev));

   for (int i = 0; i < cand_count; i++) {
      memory_alias_evidence_t ev;
      memset(&ev, 0, sizeof(ev));

      /* Stage 5 signals only fire when the inbound has an entity_id. */
      if (inbound_id > 0) {
         ev.exclusive_relation_overlap = compute_exclusive_relation_overlap(user_id, inbound_id,
                                                                            cands[i].entity_id);
         ev.contact_field_overlap = compute_contact_field_overlap(user_id, inbound_id,
                                                                  cands[i].entity_id);
      }

      ev.name_jaccard = cands[i].name_jaccard;
      ev.embedding_cosine = cands[i].embedding_cosine;
      ev.type_match = type_match_signal(entity_type, cands[i].entity_type);
      ev.type_veto_fired = type_veto_fires(entity_type, cands[i].entity_type);
      ev.name_substring_bonus_applied = memory_alias_compute_name_substring(
          canonical_name, cands[i].canonical_name);
      /* In synth-self mode the inbound is the synthetic (user_self side)
       * and the candidate is the "other" — pass the candidate's allow-
       * list flag through so the unconditional-bonus branch inside
       * user_self_bonus_applies fires for the "user" canonical. */
      bool other_token = inbound_is_user_self ? cands[i].is_user_self_token : false;
      ev.user_self_bonus_applied = user_self_bonus_applies(
          user_id, inbound_is_user_self, cands[i].is_user_self, canonical_name,
          cands[i].canonical_name, ev.contact_field_overlap > 0.0f, other_token);

      memory_alias_apply_composite(&ev);

      bool replace = false;
      if (ev.composite_score > best_composite) {
         replace = true;
      } else if (best_idx >= 0 && ev.composite_score == best_composite) {
         /* Composite tie — disambiguate via canonical_priority_compare_self.
          * Synthesize memory_entity_t shims from the candidate's stored
          * fields (alias_candidate_t carries enough). */
         memory_entity_t cur, cand;
         memset(&cur, 0, sizeof(cur));
         memset(&cand, 0, sizeof(cand));
         cur.id = cands[best_idx].entity_id;
         strncpy(cur.entity_type, cands[best_idx].entity_type, MEMORY_ENTITY_TYPE_MAX - 1);
         cur.mention_count = cands[best_idx].mention_count;
         cur.first_seen = cands[best_idx].first_seen;
         cand.id = cands[i].entity_id;
         strncpy(cand.entity_type, cands[i].entity_type, MEMORY_ENTITY_TYPE_MAX - 1);
         cand.mention_count = cands[i].mention_count;
         cand.first_seen = cands[i].first_seen;
         /* Returns > 0 when @p b (the new candidate) should be canonical. */
         if (memory_alias_canonical_priority_compare_self(&cur, cands[best_idx].is_user_self, &cand,
                                                          cands[i].is_user_self) > 0) {
            replace = true;
         }
      }
      if (replace) {
         best_composite = ev.composite_score;
         best_idx = i;
         best_ev = ev;
      }
   }

   if (best_idx >= 0 && best_composite > 0.0f) {
      *out_winner_id = cands[best_idx].entity_id;
      *out_evidence = best_ev;
      *out_matched_stage = 6;
   }
   return MEMORY_DB_SUCCESS;
}

/* =============================================================================
 * Public API impl — resolver, consider_auto_merge, alias_link, alias_unlink
 * ============================================================================= */

int memory_db_entity_resolve_alias(int user_id,
                                   const char *name,
                                   const char *entity_type,
                                   const char *canonical_name,
                                   memory_alias_resolve_t *out_resolution) {
   if (!out_resolution)
      return MEMORY_DB_FAILURE;
   memset(out_resolution, 0, sizeof(*out_resolution));
   if (!canonical_name || !*canonical_name)
      return MEMORY_DB_SUCCESS; /* clean miss */
   (void)name;                  /* display-form not needed by the cascade today */

   int64_t winner_id = 0;
   int matched_stage = 0;
   memory_alias_evidence_t ev;
   memset(&ev, 0, sizeof(ev));

   int rc = cascade_internal(user_id, canonical_name, entity_type ? entity_type : "thing",
                             /* inbound_id */ 0, /* inbound_is_user_self */ false,
                             /* use_synth_self */ false, &winner_id, &matched_stage, &ev);
   if (rc != MEMORY_DB_SUCCESS)
      return rc;

   if (matched_stage == 1) {
      /* Stage 1 hit always wins — exact canonical_name match.  Stage 1
       * already returns post-JOIN canonical_id, so the resolver caller
       * can't distinguish alias from canonical anyway. */
      out_resolution->resolved_id = winner_id;
      out_resolution->matched_stage = 1;
      return MEMORY_DB_SUCCESS;
   }

   /* Stage 6: only commit to a resolution when the composite cleared the
    * auto-merge threshold.  Mid-confidence (review band) and below are
    * "no resolution" from the resolver's perspective — the caller should
    * still fall through to upsert; consider_auto_merge() handles the
    * proposal-queue case. */
   if (matched_stage == 6 && ev.composite_score >= MEMORY_ALIAS_AUTO_THRESHOLD) {
      out_resolution->resolved_id = winner_id;
      out_resolution->matched_stage = 6;
      out_resolution->evidence = ev;
   }
   return MEMORY_DB_SUCCESS;
}

int memory_db_entity_resolve_alias_for_self(int user_id,
                                            const char *canonical_name,
                                            const char *entity_type,
                                            memory_alias_resolve_t *out_resolution) {
   if (!out_resolution)
      return MEMORY_DB_FAILURE;
   memset(out_resolution, 0, sizeof(*out_resolution));
   if (!canonical_name || !*canonical_name)
      return MEMORY_DB_SUCCESS; /* clean miss */

   int64_t winner_id = 0;
   int matched_stage = 0;
   memory_alias_evidence_t ev;
   memset(&ev, 0, sizeof(ev));

   /* Synthetic-self mode: directional overlap at Stage 2, "user" allow-list
    * pre-add, inbound treated as is_user_self=true so user_self_bonus
    * fires.  inbound_id=0 (no materialized self yet — that's what
    * "synthetic" means here). */
   int rc = cascade_internal(user_id, canonical_name, entity_type ? entity_type : "person",
                             /* inbound_id */ 0, /* inbound_is_user_self */ true,
                             /* use_synth_self */ true, &winner_id, &matched_stage, &ev);
   if (rc != MEMORY_DB_SUCCESS)
      return rc;

   /* Unlike memory_db_entity_resolve_alias() (which only commits to a
    * resolution at the auto-merge threshold), the synthetic-self resolver
    * surfaces the cascade's best Stage 6 candidate at any composite band
    * — Phase 2 callers need to see review-band and below-threshold hits
    * to make their own band-routing decisions (similar to
    * consider_auto_merge's evaluate_t shape). */
   if (matched_stage == 1) {
      out_resolution->resolved_id = winner_id;
      out_resolution->matched_stage = 1;
      return MEMORY_DB_SUCCESS;
   }
   if (matched_stage == 6 && winner_id > 0) {
      out_resolution->resolved_id = winner_id;
      out_resolution->matched_stage = 6;
      out_resolution->evidence = ev;
   }
   return MEMORY_DB_SUCCESS;
}

/* Forward declaration so consider_auto_merge can call alias_link before its
 * own definition lower in this file. */
int memory_db_entity_alias_link(int user_id,
                                int64_t source_id,
                                int64_t target_id,
                                const char *link_kind,
                                const char *reason,
                                float composite_score,
                                const char *evidence_json,
                                int64_t *out_link_id);

/* Insert a memory_entity_merge_proposals row.  Used by consider_auto_merge
 * for review-band candidates.  No entity-row mutation. */
static int insert_merge_proposal(int user_id,
                                 int64_t source_id,
                                 int64_t target_id,
                                 float composite_score,
                                 const char *evidence_json,
                                 int64_t *out_proposal_id) {
   if (!out_proposal_id)
      return MEMORY_DB_FAILURE;
   *out_proposal_id = 0;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = NULL;
   const char *sql = "INSERT INTO memory_entity_merge_proposals "
                     "(user_id, source_entity_id, target_entity_id, composite_score, "
                     " evidence_json, proposed_at) "
                     "VALUES (?, ?, ?, ?, ?, strftime('%s','now'))";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, source_id);
   sqlite3_bind_int64(stmt, 3, target_id);
   sqlite3_bind_double(stmt, 4, (double)composite_score);
   if (evidence_json && *evidence_json) {
      sqlite3_bind_text(stmt, 5, evidence_json, -1, SQLITE_TRANSIENT);
   } else {
      sqlite3_bind_text(stmt, 5, "{}", -1, SQLITE_STATIC);
   }

   int rc = sqlite3_step(stmt);
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db_alias: insert proposal failed: %s", sqlite3_errmsg(s_db.db));
      sqlite3_finalize(stmt);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   *out_proposal_id = sqlite3_last_insert_rowid(s_db.db);
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return MEMORY_DB_SUCCESS;
}

int memory_db_entity_consider_auto_merge(int user_id,
                                         int64_t entity_id,
                                         memory_alias_evaluate_t *out_eval) {
   if (!out_eval || entity_id <= 0)
      return MEMORY_DB_FAILURE;
   memset(out_eval, 0, sizeof(*out_eval));
   out_eval->source_entity_id = entity_id;

   /* Load the inbound entity. */
   memory_entity_t inbound;
   bool inbound_is_self = false;
   int64_t inbound_canon = 0;
   int rc = load_entity_full(user_id, entity_id, &inbound, &inbound_canon, &inbound_is_self);
   if (rc != MEMORY_DB_SUCCESS)
      return rc;

   /* If inbound is already a soft alias, no auto-merge work to do. */
   if (inbound_canon != 0) {
      out_eval->outcome = MEMORY_ALIAS_OUTCOME_NO_CANDIDATES;
      return MEMORY_DB_SUCCESS;
   }

   int64_t winner_id = 0;
   int matched_stage = 0;
   memory_alias_evidence_t ev;
   memset(&ev, 0, sizeof(ev));

   rc = cascade_internal(user_id, inbound.canonical_name, inbound.entity_type, entity_id,
                         inbound_is_self, /* use_synth_self */ false, &winner_id, &matched_stage,
                         &ev);
   if (rc != MEMORY_DB_SUCCESS)
      return rc;

   if (matched_stage == 0 || winner_id == 0) {
      out_eval->outcome = MEMORY_ALIAS_OUTCOME_NO_CANDIDATES;
      return MEMORY_DB_SUCCESS;
   }

   /* Stage 1 hit short-circuits to auto-merge — exact canonical_name match
    * means the row is a duplicate of an existing canonical.  Synthesize a
    * full-score evidence struct so the audit row has something to record. */
   if (matched_stage == 1) {
      memset(&ev, 0, sizeof(ev));
      ev.name_jaccard = 1.0f;
      ev.composite_score = 1.0f;
   }

   out_eval->target_entity_id = winner_id;
   out_eval->evidence = ev;

   /* Don't merge an entity into itself or into one of its own aliases. */
   if (winner_id == entity_id) {
      out_eval->outcome = MEMORY_ALIAS_OUTCOME_NO_CANDIDATES;
      out_eval->target_entity_id = 0;
      return MEMORY_DB_SUCCESS;
   }

   /* Threshold band routing. */
   if (ev.composite_score >= MEMORY_ALIAS_AUTO_THRESHOLD) {
      int64_t link_id = 0;
      int link_rc = memory_db_entity_alias_link(user_id, entity_id, winner_id, "soft", "auto-merge",
                                                ev.composite_score, NULL, &link_id);
      if (link_rc != MEMORY_DB_SUCCESS) {
         OLOG_WARNING("memory_db_alias: auto-merge soft-link failed for src=%lld tgt=%lld",
                      (long long)entity_id, (long long)winner_id);
         out_eval->outcome = MEMORY_ALIAS_OUTCOME_REJECTED;
         return MEMORY_DB_SUCCESS;
      }
      out_eval->outcome = MEMORY_ALIAS_OUTCOME_AUTO_MERGED;
      out_eval->link_id = link_id;
      return MEMORY_DB_SUCCESS;
   }

   if (ev.composite_score >= MEMORY_ALIAS_REVIEW_THRESHOLD) {
      int64_t prop_id = 0;
      if (insert_merge_proposal(user_id, entity_id, winner_id, ev.composite_score, NULL,
                                &prop_id) == MEMORY_DB_SUCCESS) {
         out_eval->outcome = MEMORY_ALIAS_OUTCOME_PROPOSED;
         out_eval->proposal_id = prop_id;
      } else {
         out_eval->outcome = MEMORY_ALIAS_OUTCOME_REJECTED;
      }
      return MEMORY_DB_SUCCESS;
   }

   out_eval->outcome = MEMORY_ALIAS_OUTCOME_REJECTED;
   return MEMORY_DB_SUCCESS;
}

int memory_db_entity_alias_link(int user_id,
                                int64_t source_id,
                                int64_t target_id,
                                const char *link_kind,
                                const char *reason,
                                float composite_score,
                                const char *evidence_json,
                                int64_t *out_link_id) {
   if (out_link_id)
      *out_link_id = 0;
   if (source_id <= 0 || target_id <= 0 || source_id == target_id)
      return MEMORY_DB_FAILURE;
   if (!link_kind || !reason)
      return MEMORY_DB_FAILURE;
   if (strcmp(link_kind, "soft") != 0 && strcmp(link_kind, "hard") != 0)
      return MEMORY_DB_FAILURE;
   /* Phase 1 ships soft-link only; "hard" is reserved for the operator
    * consolidate path that lands in Phase 3.  Refuse it here so a future
    * caller can't bypass the unimplemented promotion logic by passing
    * "hard" through this entry point. */
   if (strcmp(link_kind, "hard") == 0) {
      OLOG_WARNING("memory_db_alias: link_kind='hard' is reserved for Phase 3 consolidate; "
                   "refusing");
      return MEMORY_DB_FAILURE;
   }

   /* Load both entities (verifies ownership AND captures canonical_name
    * snapshots for the audit row).  Each load takes the lock on its own. */
   memory_entity_t src, tgt;
   int64_t src_canon = 0, tgt_canon = 0;
   int rc = load_entity_full(user_id, source_id, &src, &src_canon, NULL);
   if (rc != MEMORY_DB_SUCCESS)
      return rc;
   rc = load_entity_full(user_id, target_id, &tgt, &tgt_canon, NULL);
   if (rc != MEMORY_DB_SUCCESS)
      return rc;

   /* Refuse to link if the source already has aliases pointing at it (it's
    * canonical for an equivalence class) — would orphan its dependents. */
   {
      AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);
      sqlite3_stmt *chk = NULL;
      if (sqlite3_prepare_v2(s_db.db,
                             "SELECT 1 FROM memory_entities WHERE user_id = ? AND "
                             "canonical_id = ? LIMIT 1",
                             -1, &chk, NULL) != SQLITE_OK) {
         AUTH_DB_UNLOCK();
         return MEMORY_DB_FAILURE;
      }
      sqlite3_bind_int(chk, 1, user_id);
      sqlite3_bind_int64(chk, 2, source_id);
      bool has_dependents = (sqlite3_step(chk) == SQLITE_ROW);
      sqlite3_finalize(chk);
      AUTH_DB_UNLOCK();
      if (has_dependents) {
         OLOG_WARNING("memory_db_alias: refusing to link entity %lld — has dependent aliases",
                      (long long)source_id);
         return MEMORY_DB_FAILURE;
      }
   }

   /* Atomic write: UPDATE source.canonical_id + INSERT audit row, all
    * inside BEGIN IMMEDIATE.  Rollback on any failure. */
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   char *errmsg = NULL;
   if (sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("memory_db_alias: BEGIN IMMEDIATE failed: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   int64_t link_id = 0;
   int result_rc = MEMORY_DB_SUCCESS;

   /* UPDATE memory_entities SET canonical_id = ?. */
   sqlite3_stmt *upd = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "UPDATE memory_entities SET canonical_id = ? "
                          "WHERE id = ? AND user_id = ?",
                          -1, &upd, NULL) != SQLITE_OK) {
      result_rc = MEMORY_DB_FAILURE;
      goto link_fail;
   }
   sqlite3_bind_int64(upd, 1, target_id);
   sqlite3_bind_int64(upd, 2, source_id);
   sqlite3_bind_int(upd, 3, user_id);
   if (sqlite3_step(upd) != SQLITE_DONE) {
      OLOG_ERROR("memory_db_alias: UPDATE canonical_id failed: %s", sqlite3_errmsg(s_db.db));
      sqlite3_finalize(upd);
      result_rc = MEMORY_DB_FAILURE;
      goto link_fail;
   }
   sqlite3_finalize(upd);

   /* INSERT INTO memory_entity_aliases. */
   sqlite3_stmt *ins = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "INSERT INTO memory_entity_aliases "
                          "(user_id, source_entity_id, target_entity_id, "
                          " source_canonical_name, target_canonical_name, "
                          " link_kind, reason, composite_score, evidence_json, "
                          " linked_at) "
                          "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, strftime('%s','now'))",
                          -1, &ins, NULL) != SQLITE_OK) {
      result_rc = MEMORY_DB_FAILURE;
      goto link_fail;
   }
   sqlite3_bind_int(ins, 1, user_id);
   sqlite3_bind_int64(ins, 2, source_id);
   sqlite3_bind_int64(ins, 3, target_id);
   sqlite3_bind_text(ins, 4, src.canonical_name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(ins, 5, tgt.canonical_name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(ins, 6, link_kind, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(ins, 7, reason, -1, SQLITE_TRANSIENT);
   if (composite_score >= 0.0f) {
      sqlite3_bind_double(ins, 8, (double)composite_score);
   } else {
      sqlite3_bind_null(ins, 8);
   }
   if (evidence_json && *evidence_json) {
      sqlite3_bind_text(ins, 9, evidence_json, -1, SQLITE_TRANSIENT);
   } else {
      sqlite3_bind_null(ins, 9);
   }
   if (sqlite3_step(ins) != SQLITE_DONE) {
      OLOG_ERROR("memory_db_alias: INSERT alias failed: %s", sqlite3_errmsg(s_db.db));
      sqlite3_finalize(ins);
      result_rc = MEMORY_DB_FAILURE;
      goto link_fail;
   }
   link_id = sqlite3_last_insert_rowid(s_db.db);
   sqlite3_finalize(ins);

   if (sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("memory_db_alias: COMMIT failed: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      result_rc = MEMORY_DB_FAILURE;
      goto link_fail;
   }

   AUTH_DB_UNLOCK();

   /* Post-commit: invalidate the entity-embedding cache so subsequent
    * reads see the new alias state.  Safe outside the lock — the
    * invalidator is just an atomic dirty-bit flip (memory_embeddings.c). */
   memory_embeddings_invalidate_entity_cache();

   if (out_link_id)
      *out_link_id = link_id;

   OLOG_INFO("memory_db_alias: linked %lld -> %lld (%s, %s, composite=%.2f, link_id=%lld)",
             (long long)source_id, (long long)target_id, link_kind, reason,
             (composite_score < 0.0f) ? -1.0 : (double)composite_score, (long long)link_id);
   return MEMORY_DB_SUCCESS;

link_fail:
   sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
   AUTH_DB_UNLOCK();
   return result_rc;
}

int memory_db_entity_alias_unlink(int user_id, int64_t link_id, const char *unlink_reason) {
   if (link_id <= 0)
      return MEMORY_DB_FAILURE;
   const char *reason = (unlink_reason && *unlink_reason) ? unlink_reason : "split-by-operator";

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   /* Load + validate the link row.  Refuse if hard-merged or already unlinked. */
   sqlite3_stmt *load = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT source_entity_id, link_kind, unlinked_at "
                          "FROM memory_entity_aliases WHERE id = ? AND user_id = ?",
                          -1, &load, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int64(load, 1, link_id);
   sqlite3_bind_int(load, 2, user_id);

   int rc = sqlite3_step(load);
   if (rc == SQLITE_DONE) {
      sqlite3_finalize(load);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_NOT_FOUND;
   }
   if (rc != SQLITE_ROW) {
      sqlite3_finalize(load);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   int64_t source_id = sqlite3_column_int64(load, 0);
   const unsigned char *kind = sqlite3_column_text(load, 1);
   bool is_hard = kind && strcmp((const char *)kind, "hard") == 0;
   bool already_unlinked = sqlite3_column_type(load, 2) != SQLITE_NULL;
   sqlite3_finalize(load);

   if (is_hard) {
      AUTH_DB_UNLOCK();
      OLOG_WARNING("memory_db_alias: refusing to split hard-merged link %lld", (long long)link_id);
      return MEMORY_DB_FAILURE;
   }
   if (already_unlinked) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_NOT_FOUND;
   }

   /* Atomic split: UPDATE memory_entities.canonical_id = NULL + UPDATE
    * memory_entity_aliases SET unlinked_at, unlink_reason. */
   char *errmsg = NULL;
   if (sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("memory_db_alias: BEGIN IMMEDIATE failed: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   int result_rc = MEMORY_DB_SUCCESS;

   sqlite3_stmt *clr = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "UPDATE memory_entities SET canonical_id = NULL "
                          "WHERE id = ? AND user_id = ?",
                          -1, &clr, NULL) != SQLITE_OK) {
      result_rc = MEMORY_DB_FAILURE;
      goto split_fail;
   }
   sqlite3_bind_int64(clr, 1, source_id);
   sqlite3_bind_int(clr, 2, user_id);
   if (sqlite3_step(clr) != SQLITE_DONE) {
      sqlite3_finalize(clr);
      result_rc = MEMORY_DB_FAILURE;
      goto split_fail;
   }
   sqlite3_finalize(clr);

   sqlite3_stmt *audit = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "UPDATE memory_entity_aliases SET unlinked_at = strftime('%s','now'), "
                          "       unlink_reason = ? WHERE id = ?",
                          -1, &audit, NULL) != SQLITE_OK) {
      result_rc = MEMORY_DB_FAILURE;
      goto split_fail;
   }
   sqlite3_bind_text(audit, 1, reason, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(audit, 2, link_id);
   if (sqlite3_step(audit) != SQLITE_DONE) {
      sqlite3_finalize(audit);
      result_rc = MEMORY_DB_FAILURE;
      goto split_fail;
   }
   sqlite3_finalize(audit);

   if (sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, &errmsg) != SQLITE_OK) {
      OLOG_ERROR("memory_db_alias: COMMIT failed: %s", errmsg ? errmsg : "unknown");
      sqlite3_free(errmsg);
      result_rc = MEMORY_DB_FAILURE;
      goto split_fail;
   }

   AUTH_DB_UNLOCK();

   /* Post-commit cache invalidation; same contract as alias_link. */
   memory_embeddings_invalidate_entity_cache();

   OLOG_INFO("memory_db_alias: split link %lld (source=%lld, reason=%s)", (long long)link_id,
             (long long)source_id, reason);
   return MEMORY_DB_SUCCESS;

split_fail:
   sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
   AUTH_DB_UNLOCK();
   return result_rc;
}

/* =============================================================================
 * Equivalence-class-aware relation listings (design §5)
 *
 * The IN-subquery form keeps the API shape compatible with
 * memory_db_relation_list_by_subject.  EXPLAIN QUERY PLAN was used to
 * confirm the planner uses idx_memory_entities_canonical for the inner
 * SELECT (see ckpt summary).  The fallback UNION-ALL form was deemed
 * unnecessary after the verification step.
 * ============================================================================= */

/* Bounded helper: populate a memory_relation_t row from the standard
 * outgoing-relation SELECT shape used below.  Keeps the three list
 * variants free of struct-copy boilerplate. */
static void populate_relation_outgoing_row(sqlite3_stmt *stmt, memory_relation_t *out) {
   memset(out, 0, sizeof(*out));
   out->id = sqlite3_column_int64(stmt, 0);
   out->subject_entity_id = sqlite3_column_int64(stmt, 1);
   const char *rel = (const char *)sqlite3_column_text(stmt, 2);
   if (rel) {
      strncpy(out->relation, rel, MEMORY_RELATION_MAX - 1);
      out->relation[MEMORY_RELATION_MAX - 1] = '\0';
   }
   out->object_entity_id = (sqlite3_column_type(stmt, 3) == SQLITE_NULL)
                               ? 0
                               : sqlite3_column_int64(stmt, 3);
   const char *obj_name = (const char *)sqlite3_column_text(stmt, 4);
   if (obj_name) {
      strncpy(out->object_name, obj_name, MEMORY_ENTITY_NAME_MAX - 1);
      out->object_name[MEMORY_ENTITY_NAME_MAX - 1] = '\0';
   }
   out->confidence = (float)sqlite3_column_double(stmt, 5);
   out->valid_from = (sqlite3_column_type(stmt, 6) == SQLITE_NULL) ? 0
                                                                   : sqlite3_column_int64(stmt, 6);
   out->valid_to = (sqlite3_column_type(stmt, 7) == SQLITE_NULL) ? 0
                                                                 : sqlite3_column_int64(stmt, 7);
}

int memory_db_relation_list_by_subject_class(int user_id,
                                             int64_t canonical_id,
                                             memory_relation_t *out,
                                             int max,
                                             int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out || !count_out || max <= 0 || canonical_id <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   /* The IN-subquery shape passes EXPLAIN QUERY PLAN with both
    * idx_memory_entities_user (for id = ?) and idx_memory_entities_canonical
    * (for canonical_id = ?) on the inner SELECT.  See ckpt summary. */
   const char *sql = "SELECT r.id, r.subject_entity_id, r.relation, r.object_entity_id, "
                     "       COALESCE(e.name, r.object_value, ''), r.confidence, "
                     "       r.valid_from, r.valid_to "
                     "FROM memory_relations r "
                     "LEFT JOIN memory_entities e ON e.id = r.object_entity_id "
                     "WHERE r.user_id = ? AND r.subject_entity_id IN ( "
                     "   SELECT id FROM memory_entities WHERE user_id = ? AND id = ? "
                     "   UNION ALL "
                     "   SELECT id FROM memory_entities WHERE user_id = ? AND canonical_id = ? "
                     ") "
                     "ORDER BY r.id ASC LIMIT ?";

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, user_id);
   sqlite3_bind_int64(stmt, 3, canonical_id);
   sqlite3_bind_int(stmt, 4, user_id);
   sqlite3_bind_int64(stmt, 5, canonical_id);
   sqlite3_bind_int(stmt, 6, max);

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_relation_outgoing_row(stmt, &out[n]);
      n++;
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   *count_out = n;
   return MEMORY_DB_SUCCESS;
}

int memory_db_relation_list_by_subject_class_at(int user_id,
                                                int64_t canonical_id,
                                                int64_t as_of_ts,
                                                memory_relation_t *out,
                                                int max,
                                                int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out || !count_out || max <= 0 || canonical_id <= 0)
      return MEMORY_DB_FAILURE;

   if (as_of_ts == 0)
      as_of_ts = (int64_t)time(NULL);

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   const char *sql = "SELECT r.id, r.subject_entity_id, r.relation, r.object_entity_id, "
                     "       COALESCE(e.name, r.object_value, ''), r.confidence, "
                     "       r.valid_from, r.valid_to "
                     "FROM memory_relations r "
                     "LEFT JOIN memory_entities e ON e.id = r.object_entity_id "
                     "WHERE r.user_id = ? AND r.subject_entity_id IN ( "
                     "   SELECT id FROM memory_entities WHERE user_id = ? AND id = ? "
                     "   UNION ALL "
                     "   SELECT id FROM memory_entities WHERE user_id = ? AND canonical_id = ? "
                     ") "
                     "  AND (r.valid_from IS NULL OR r.valid_from <= ?) "
                     "  AND (r.valid_to IS NULL OR r.valid_to > ?) "
                     "ORDER BY r.id ASC LIMIT ?";

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, user_id);
   sqlite3_bind_int64(stmt, 3, canonical_id);
   sqlite3_bind_int(stmt, 4, user_id);
   sqlite3_bind_int64(stmt, 5, canonical_id);
   sqlite3_bind_int64(stmt, 6, as_of_ts);
   sqlite3_bind_int64(stmt, 7, as_of_ts);
   sqlite3_bind_int(stmt, 8, max);

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_relation_outgoing_row(stmt, &out[n]);
      n++;
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   *count_out = n;
   return MEMORY_DB_SUCCESS;
}

int memory_db_relation_list_by_object_class(int user_id,
                                            int64_t canonical_id,
                                            memory_relation_t *out,
                                            int max,
                                            int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out || !count_out || max <= 0 || canonical_id <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   /* Incoming-relation listing — the object_name field carries the SUBJECT's
    * resolved name (matches existing memory_db_relation_list_by_object
    * convention). */
   const char *sql = "SELECT r.id, r.subject_entity_id, r.relation, r.object_entity_id, "
                     "       COALESCE(s.name, ''), r.confidence, r.valid_from, r.valid_to "
                     "FROM memory_relations r "
                     "LEFT JOIN memory_entities s ON s.id = r.subject_entity_id "
                     "WHERE r.user_id = ? AND r.object_entity_id IN ( "
                     "   SELECT id FROM memory_entities WHERE user_id = ? AND id = ? "
                     "   UNION ALL "
                     "   SELECT id FROM memory_entities WHERE user_id = ? AND canonical_id = ? "
                     ") "
                     "ORDER BY r.id ASC LIMIT ?";

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, user_id);
   sqlite3_bind_int64(stmt, 3, canonical_id);
   sqlite3_bind_int(stmt, 4, user_id);
   sqlite3_bind_int64(stmt, 5, canonical_id);
   sqlite3_bind_int(stmt, 6, max);

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW) {
      populate_relation_outgoing_row(stmt, &out[n]);
      n++;
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   *count_out = n;
   return MEMORY_DB_SUCCESS;
}

/* =============================================================================
 * Listing helpers used by the dawn-admin entity subcommands and the WebUI
 * REST/WebSocket handlers (Ckpt 4).  These three are read-only views into
 * the alias surface — no DB writes — so they live in the alias module
 * rather than reach into s_db from outside the memory layer.
 * ============================================================================= */

int memory_db_entity_alias_list(int user_id,
                                int64_t target_entity_id,
                                memory_alias_listing_row_t *out,
                                int max,
                                int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out || max <= 0 || target_entity_id <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = NULL;
   const char *sql = "SELECT id, COALESCE(source_entity_id, 0), source_canonical_name, "
                     "       link_kind, COALESCE(composite_score, -1.0), linked_at "
                     "FROM memory_entity_aliases "
                     "WHERE user_id = ? AND target_entity_id = ? AND unlinked_at IS NULL "
                     "ORDER BY linked_at DESC LIMIT ?";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, target_entity_id);
   sqlite3_bind_int(stmt, 3, max);

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW) {
      memory_alias_listing_row_t *row = &out[n];
      memset(row, 0, sizeof(*row));
      row->link_id = sqlite3_column_int64(stmt, 0);
      row->source_entity_id = sqlite3_column_int64(stmt, 1);
      const char *src = (const char *)sqlite3_column_text(stmt, 2);
      if (src) {
         strncpy(row->source_canonical_name, src, MEMORY_ENTITY_NAME_MAX - 1);
         row->source_canonical_name[MEMORY_ENTITY_NAME_MAX - 1] = '\0';
      }
      const char *kind = (const char *)sqlite3_column_text(stmt, 3);
      if (kind) {
         strncpy(row->link_kind, kind, sizeof(row->link_kind) - 1);
         row->link_kind[sizeof(row->link_kind) - 1] = '\0';
      }
      row->composite_score = (float)sqlite3_column_double(stmt, 4);
      row->linked_at = sqlite3_column_int64(stmt, 5);
      n++;
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = n;
   return MEMORY_DB_SUCCESS;
}

int memory_db_entity_alias_history(int user_id,
                                   int64_t entity_id,
                                   memory_alias_history_row_t *out,
                                   int max,
                                   int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out || max <= 0 || entity_id <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = NULL;
   const char *sql = "SELECT id, source_canonical_name, target_canonical_name, "
                     "       link_kind, reason, linked_at, "
                     "       COALESCE(unlinked_at, 0), COALESCE(unlink_reason, '') "
                     "FROM memory_entity_aliases "
                     "WHERE user_id = ? AND (source_entity_id = ? OR target_entity_id = ?) "
                     "ORDER BY linked_at ASC LIMIT ?";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, entity_id);
   sqlite3_bind_int64(stmt, 3, entity_id);
   sqlite3_bind_int(stmt, 4, max);

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW) {
      memory_alias_history_row_t *row = &out[n];
      memset(row, 0, sizeof(*row));
      row->link_id = sqlite3_column_int64(stmt, 0);
      const char *src = (const char *)sqlite3_column_text(stmt, 1);
      if (src) {
         strncpy(row->source_canonical_name, src, MEMORY_ENTITY_NAME_MAX - 1);
         row->source_canonical_name[MEMORY_ENTITY_NAME_MAX - 1] = '\0';
      }
      const char *tgt = (const char *)sqlite3_column_text(stmt, 2);
      if (tgt) {
         strncpy(row->target_canonical_name, tgt, MEMORY_ENTITY_NAME_MAX - 1);
         row->target_canonical_name[MEMORY_ENTITY_NAME_MAX - 1] = '\0';
      }
      const char *kind = (const char *)sqlite3_column_text(stmt, 3);
      if (kind) {
         strncpy(row->link_kind, kind, sizeof(row->link_kind) - 1);
         row->link_kind[sizeof(row->link_kind) - 1] = '\0';
      }
      const char *reason = (const char *)sqlite3_column_text(stmt, 4);
      if (reason) {
         strncpy(row->reason, reason, sizeof(row->reason) - 1);
         row->reason[sizeof(row->reason) - 1] = '\0';
      }
      row->linked_at = sqlite3_column_int64(stmt, 5);
      row->unlinked_at = sqlite3_column_int64(stmt, 6);
      const char *unlink_r = (const char *)sqlite3_column_text(stmt, 7);
      if (unlink_r) {
         strncpy(row->unlink_reason, unlink_r, sizeof(row->unlink_reason) - 1);
         row->unlink_reason[sizeof(row->unlink_reason) - 1] = '\0';
      }
      n++;
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = n;
   return MEMORY_DB_SUCCESS;
}

int memory_db_entity_list_for_admin(int user_id,
                                    bool include_aliases,
                                    memory_alias_entity_row_t *out,
                                    int max,
                                    int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out || max <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   /* Pass 1 — canonicals (canonical_id IS NULL), mention_count DESC. */
   const char *sql_canonical = "SELECT id, name, entity_type, mention_count, is_user_self "
                               "FROM memory_entities "
                               "WHERE user_id = ? AND canonical_id IS NULL "
                               "ORDER BY mention_count DESC, id ASC LIMIT ?";
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(s_db.db, sql_canonical, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, max);

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW) {
      memory_alias_entity_row_t *row = &out[n];
      memset(row, 0, sizeof(*row));
      row->entity_id = sqlite3_column_int64(stmt, 0);
      const char *name = (const char *)sqlite3_column_text(stmt, 1);
      if (name) {
         strncpy(row->name, name, MEMORY_ENTITY_NAME_MAX - 1);
         row->name[MEMORY_ENTITY_NAME_MAX - 1] = '\0';
      }
      const char *etype = (const char *)sqlite3_column_text(stmt, 2);
      if (etype) {
         strncpy(row->entity_type, etype, MEMORY_ENTITY_TYPE_MAX - 1);
         row->entity_type[MEMORY_ENTITY_TYPE_MAX - 1] = '\0';
      }
      row->mention_count = sqlite3_column_int(stmt, 3);
      row->is_user_self = sqlite3_column_int(stmt, 4) != 0;
      row->is_alias = false;
      row->canonical_id = 0;
      n++;
   }
   sqlite3_finalize(stmt);

   /* Pass 2 — aliases, ordered by canonical_id so callers can iterate
    * canonicals and find their aliases by linear scan over a contiguous
    * tail.  Separate budget from canonicals: with the historic single-query
    * shape, 254 canonicals saturated max=200 and silently dropped every
    * alias.  Quota now lives in `max - n`, so the operator's link state
    * surfaces even on dense user graphs. */
   if (include_aliases && n < max) {
      const char *sql_aliases =
          "SELECT id, name, entity_type, mention_count, is_user_self, canonical_id "
          "FROM memory_entities "
          "WHERE user_id = ? AND canonical_id IS NOT NULL "
          "ORDER BY canonical_id ASC, mention_count DESC, id ASC LIMIT ?";
      if (sqlite3_prepare_v2(s_db.db, sql_aliases, -1, &stmt, NULL) != SQLITE_OK) {
         AUTH_DB_UNLOCK();
         if (count_out)
            *count_out = n;
         return MEMORY_DB_FAILURE;
      }
      sqlite3_bind_int(stmt, 1, user_id);
      sqlite3_bind_int(stmt, 2, max - n);

      while (n < max && sqlite3_step(stmt) == SQLITE_ROW) {
         memory_alias_entity_row_t *row = &out[n];
         memset(row, 0, sizeof(*row));
         row->entity_id = sqlite3_column_int64(stmt, 0);
         const char *name = (const char *)sqlite3_column_text(stmt, 1);
         if (name) {
            strncpy(row->name, name, MEMORY_ENTITY_NAME_MAX - 1);
            row->name[MEMORY_ENTITY_NAME_MAX - 1] = '\0';
         }
         const char *etype = (const char *)sqlite3_column_text(stmt, 2);
         if (etype) {
            strncpy(row->entity_type, etype, MEMORY_ENTITY_TYPE_MAX - 1);
            row->entity_type[MEMORY_ENTITY_TYPE_MAX - 1] = '\0';
         }
         row->mention_count = sqlite3_column_int(stmt, 3);
         row->is_user_self = sqlite3_column_int(stmt, 4) != 0;
         row->canonical_id = sqlite3_column_int64(stmt, 5);
         row->is_alias = true;
         n++;
      }
      sqlite3_finalize(stmt);
   }

   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = n;
   return MEMORY_DB_SUCCESS;
}

/* =============================================================================
 * Bulk alias summary — single-shot scan of every alias row for a user.
 *
 * Used by the WebUI Graph-tab list handler to:
 *   1. Drop alias rows from the canonical-only list (the (alias_entity_id)
 *      set tells the caller which `memory_db_entity_list` rows to skip);
 *   2. Tally per-canonical alias counts so canonical cards render the
 *      "X aliases" badge without an N+1 round-trip.
 *
 * Driven by the partial index `idx_memory_entities_canonical`, which already
 * filters to `canonical_id IS NOT NULL`.  At the dev's 2k-entity scale this
 * is a sub-millisecond scan over a small partial index; even with thousands
 * of aliases the cost stays negligible.
 * ============================================================================= */

int memory_db_entity_alias_summary(int user_id,
                                   memory_alias_summary_row_t *out,
                                   int max,
                                   int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out || max <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = NULL;
   /* Partial index `idx_memory_entities_canonical` filters to
    * canonical_id IS NOT NULL — restate the predicate so the planner picks it. */
   const char *sql = "SELECT id, canonical_id "
                     "FROM memory_entities "
                     "WHERE user_id = ? AND canonical_id IS NOT NULL "
                     "ORDER BY id ASC "
                     "LIMIT ?";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, max);

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW) {
      out[n].alias_entity_id = sqlite3_column_int64(stmt, 0);
      out[n].canonical_entity_id = sqlite3_column_int64(stmt, 1);
      n++;
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = n;
   return MEMORY_DB_SUCCESS;
}

/* =============================================================================
 * Merge-proposal listing + resolve (Phase 1 fold-in to Ckpt 4).
 *
 * link-user-self queues 0.70-0.90 candidates as proposals; the WebUI surfaces
 * them via these two helpers.  Phase 2's auto-merge gate at extraction time
 * will be the dominant producer once it ships.
 * ============================================================================= */

int memory_db_proposal_list_pending(int user_id,
                                    memory_alias_proposal_row_t *out,
                                    int max,
                                    int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out || max <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = NULL;
   /* JOIN once with COALESCE so soft-deleted entity rows render gracefully
    * (canonical_name = '' rather than NULL).  idx_merge_proposals_pending
    * (partial on resolved_at IS NULL) drives the outer scan. */
   const char *sql = "SELECT p.id, p.source_entity_id, p.target_entity_id, "
                     "       COALESCE(s.canonical_name, ''), "
                     "       COALESCE(t.canonical_name, ''), "
                     "       p.composite_score, p.proposed_at "
                     "FROM memory_entity_merge_proposals p "
                     "LEFT JOIN memory_entities s ON s.id = p.source_entity_id "
                     "LEFT JOIN memory_entities t ON t.id = p.target_entity_id "
                     "WHERE p.user_id = ? AND p.resolved_at IS NULL "
                     "ORDER BY p.proposed_at DESC LIMIT ?";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int(stmt, 2, max);

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW) {
      memory_alias_proposal_row_t *row = &out[n];
      memset(row, 0, sizeof(*row));
      row->proposal_id = sqlite3_column_int64(stmt, 0);
      row->source_entity_id = sqlite3_column_int64(stmt, 1);
      row->target_entity_id = sqlite3_column_int64(stmt, 2);
      const char *src = (const char *)sqlite3_column_text(stmt, 3);
      if (src) {
         strncpy(row->source_canonical_name, src, MEMORY_ENTITY_NAME_MAX - 1);
         row->source_canonical_name[MEMORY_ENTITY_NAME_MAX - 1] = '\0';
      }
      const char *tgt = (const char *)sqlite3_column_text(stmt, 4);
      if (tgt) {
         strncpy(row->target_canonical_name, tgt, MEMORY_ENTITY_NAME_MAX - 1);
         row->target_canonical_name[MEMORY_ENTITY_NAME_MAX - 1] = '\0';
      }
      row->composite_score = (float)sqlite3_column_double(stmt, 5);
      row->proposed_at = sqlite3_column_int64(stmt, 6);
      n++;
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   if (count_out)
      *count_out = n;
   return MEMORY_DB_SUCCESS;
}

int memory_db_proposal_resolve(int user_id,
                               int64_t proposal_id,
                               bool approved,
                               int64_t *out_link_id) {
   if (out_link_id)
      *out_link_id = 0;
   if (proposal_id <= 0)
      return MEMORY_DB_FAILURE;

   /* Race-discipline (Ckpt 5 fold-in for arch-M2 / emb-L4):
    *
    *   Phase 1: claim the proposal atomically — UPDATE …
    *   resolution=? WHERE resolved_at IS NULL inside BEGIN IMMEDIATE +
    *   COMMIT, capturing changes().  If changes()==0 a concurrent operator
    *   resolved it first and we return NOT_FOUND with no further side
    *   effects.
    *
    *   Phase 2: only when changes()==1 AND approved==true, call
    *   memory_db_entity_alias_link.  If alias_link fails post-claim, the
    *   proposal stays stamped resolved="approved" but no link materialized
    *   — log loudly so an operator can split-and-reproposal-or-reject; the
    *   row is still in a clean closed state.
    *
    * Inverted ordering vs Ckpt 4 fold-in: previously alias_link ran first
    * and a no-op UPDATE risked leaving an orphan alias.  Claim-first
    * eliminates the orphan-on-success-with-noop-update window. */

   /* Phase 1: claim. */
   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   /* BEGIN IMMEDIATE so we hold the reserved-lock across the load+update
    * pair; this is also the explicit signal that we're stamping ownership
    * before any later side effect. */
   if (sqlite3_exec(s_db.db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }

   /* Load source/target so we can fire alias_link after commit. */
   sqlite3_stmt *load = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "SELECT source_entity_id, target_entity_id "
                          "FROM memory_entity_merge_proposals "
                          "WHERE id = ? AND user_id = ? AND resolved_at IS NULL",
                          -1, &load, NULL) != SQLITE_OK) {
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int64(load, 1, proposal_id);
   sqlite3_bind_int(load, 2, user_id);

   int rc = sqlite3_step(load);
   if (rc == SQLITE_DONE) {
      sqlite3_finalize(load);
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_NOT_FOUND;
   }
   if (rc != SQLITE_ROW) {
      sqlite3_finalize(load);
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   int64_t source_id = sqlite3_column_int64(load, 0);
   int64_t target_id = sqlite3_column_int64(load, 1);
   sqlite3_finalize(load);

   /* Stamp resolved_at + resolution.  Idempotent against concurrent
    * resolvers via the resolved_at IS NULL predicate. */
   sqlite3_stmt *upd = NULL;
   if (sqlite3_prepare_v2(s_db.db,
                          "UPDATE memory_entity_merge_proposals "
                          "SET resolved_at = strftime('%s','now'), resolution = ? "
                          "WHERE id = ? AND user_id = ? AND resolved_at IS NULL",
                          -1, &upd, NULL) != SQLITE_OK) {
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_text(upd, 1, approved ? "approved" : "rejected", -1, SQLITE_STATIC);
   sqlite3_bind_int64(upd, 2, proposal_id);
   sqlite3_bind_int(upd, 3, user_id);
   rc = sqlite3_step(upd);
   int changes = sqlite3_changes(s_db.db);
   sqlite3_finalize(upd);

   if (rc != SQLITE_DONE) {
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   if (changes == 0) {
      /* Lost the race — another operator stamped the row between our load
       * and our UPDATE (only possible if BEGIN IMMEDIATE was deferred or
       * if a non-AUTH_DB_LOCK writer beat us — rare in practice but the
       * predicate guards correctness either way). */
      sqlite3_exec(s_db.db, "ROLLBACK", NULL, NULL, NULL);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_NOT_FOUND;
   }
   if (sqlite3_exec(s_db.db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   AUTH_DB_UNLOCK();

   /* Phase 2: on approve, materialize the alias link.  If this fails
    * after a successful claim the proposal stays stamped but no alias was
    * created — log loudly so an operator can intervene. */
   int64_t link_id = 0;
   if (approved) {
      int link_rc = memory_db_entity_alias_link(user_id, source_id, target_id, "soft",
                                                "operator-approved-from-proposal", -1.0f, NULL,
                                                &link_id);
      if (link_rc != MEMORY_DB_SUCCESS) {
         OLOG_WARNING("memory_db_alias: proposal %lld claimed approved but alias_link failed "
                      "(rc=%d, source=%lld, target=%lld) — proposal stays stamped, no alias "
                      "materialized; operator may need to manually re-link or split",
                      (long long)proposal_id, link_rc, (long long)source_id, (long long)target_id);
         return MEMORY_DB_FAILURE;
      }
   }

   if (out_link_id)
      *out_link_id = link_id;
   return MEMORY_DB_SUCCESS;
}

/* =============================================================================
 * link-user-self orchestrator (design §8 Path B)
 *
 * Synchronous — at the dev's ~2k entity scale the resolver cascade per
 * entity (~10-20ms) puts the call in the tens-of-seconds range, fine for an
 * operator-driven backfill.  Phase 2 may revisit with a detached-thread +
 * status-query pattern if entities-per-user crosses ~10k.
 * ============================================================================= */

/* Insert a fresh user-self canonical row.  Acquires its own auth_db lock. */
static int seed_user_self_entity(int user_id,
                                 const char *display_name,
                                 const char *canonical_name,
                                 int64_t *out_id) {
   if (out_id)
      *out_id = 0;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = NULL;
   const char *sql = "INSERT INTO memory_entities "
                     "(user_id, name, entity_type, canonical_name, is_user_self, mention_count) "
                     "VALUES (?, ?, 'person', ?, 1, 0)";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_text(stmt, 2, display_name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, canonical_name, -1, SQLITE_TRANSIENT);

   int rc = sqlite3_step(stmt);
   if (rc != SQLITE_DONE) {
      int err = sqlite3_extended_errcode(s_db.db);
      sqlite3_finalize(stmt);
      AUTH_DB_UNLOCK();
      /* UNIQUE(user_id, canonical_name) collision — see MEMORY_DB_SELF_NAME_COLLISION
       * in memory_types.h for resolution path.  Caller surfaces the
       * actionable hint to the operator. */
      if (err == SQLITE_CONSTRAINT_UNIQUE || err == SQLITE_CONSTRAINT_PRIMARYKEY) {
         OLOG_WARNING("memory_db_alias: seed user-self UNIQUE collision on canonical_name "
                      "'%s' for user %d — composite scored below promotion threshold; "
                      "operator must promote manually or change real_name",
                      canonical_name, user_id);
         return MEMORY_DB_SELF_NAME_COLLISION;
      }
      OLOG_ERROR("memory_db_alias: seed user-self failed: %s", sqlite3_errmsg(s_db.db));
      return MEMORY_DB_FAILURE;
   }
   if (out_id)
      *out_id = sqlite3_last_insert_rowid(s_db.db);
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   return MEMORY_DB_SUCCESS;
}

/* Set is_user_self=1 on an existing entity (Path B step 1 — promote existing
 * match instead of inserting a fresh seed).  Caller is responsible for
 * ensuring no other entity for this user currently has is_user_self=1
 * (the partial UNIQUE index would otherwise block the UPDATE). */
static int promote_to_user_self_entity(int user_id, int64_t entity_id) {
   if (entity_id <= 0 || user_id <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = NULL;
   const char *sql = "UPDATE memory_entities SET is_user_self = 1 "
                     "WHERE id = ? AND user_id = ? AND is_user_self = 0";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int64(stmt, 1, entity_id);
   sqlite3_bind_int(stmt, 2, user_id);

   int rc = sqlite3_step(stmt);
   if (rc != SQLITE_DONE) {
      OLOG_ERROR("memory_db_alias: promote-to-user-self failed: %s", sqlite3_errmsg(s_db.db));
      sqlite3_finalize(stmt);
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   int changes = sqlite3_changes(s_db.db);
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();

   if (changes == 0) {
      OLOG_WARNING("memory_db_alias: promote-to-user-self matched no rows (entity_id=%lld, "
                   "user_id=%d, may already be is_user_self=1 or canonical_id != NULL)",
                   (long long)entity_id, user_id);
      return MEMORY_DB_NOT_FOUND;
   }
   return MEMORY_DB_SUCCESS;
}

/* Canonical-only entity ids for the user, mention_count DESC. */
static int list_canonical_entity_ids(int user_id,
                                     int64_t exclude_id,
                                     int64_t *out_ids,
                                     int max,
                                     int *count_out) {
   if (count_out)
      *count_out = 0;
   if (!out_ids || max <= 0)
      return MEMORY_DB_FAILURE;

   AUTH_DB_LOCK_OR_RETURN(MEMORY_DB_FAILURE);

   sqlite3_stmt *stmt = NULL;
   const char *sql = "SELECT id FROM memory_entities WHERE user_id = ? AND id != ? "
                     "  AND canonical_id IS NULL "
                     "ORDER BY mention_count DESC, id ASC LIMIT ?";
   if (sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL) != SQLITE_OK) {
      AUTH_DB_UNLOCK();
      return MEMORY_DB_FAILURE;
   }
   sqlite3_bind_int(stmt, 1, user_id);
   sqlite3_bind_int64(stmt, 2, exclude_id);
   sqlite3_bind_int(stmt, 3, max);

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW) {
      out_ids[n++] = sqlite3_column_int64(stmt, 0);
   }
   sqlite3_finalize(stmt);
   AUTH_DB_UNLOCK();
   *count_out = n;
   return MEMORY_DB_SUCCESS;
}

/* Walk backwards over continuation bytes (0x80-0xBF) and trim if the
 * leader's expected sequence length isn't fully satisfied — prevents
 * downstream tokenizers from seeing a malformed UTF-8 byte at the cutoff
 * when strncpy split a multi-byte character. */
static void trim_trailing_partial_utf8(char *buf) {
   if (!buf)
      return;
   size_t len = strlen(buf);
   if (len == 0)
      return;
   size_t i = len;
   while (i > 0 && ((unsigned char)buf[i - 1] & 0xC0) == 0x80) {
      i--;
   }
   if (i == 0)
      return;
   unsigned char lead = (unsigned char)buf[i - 1];
   size_t expected;
   if ((lead & 0x80) == 0) {
      expected = 1; /* ASCII */
   } else if ((lead & 0xE0) == 0xC0) {
      expected = 2;
   } else if ((lead & 0xF0) == 0xE0) {
      expected = 3;
   } else if ((lead & 0xF8) == 0xF0) {
      expected = 4;
   } else {
      buf[i - 1] = '\0'; /* invalid leader — drop it */
      return;
   }
   if ((len - (i - 1)) < expected) {
      buf[i - 1] = '\0'; /* incomplete tail sequence — drop it */
   }
}

/* Load the user-identity fields (real_name, preferred_address,
 * identity_aliases) via the public auth_db_get_user_identity() helper.
 * UTF-8 trim is applied to every TEXT field so a downstream tokenizer
 * doesn't see a malformed leader at any column boundary.
 *
 * Returns MEMORY_DB_SUCCESS with all-empty fields if the user row exists
 * but no identity fields are populated; MEMORY_DB_NOT_FOUND if the user
 * doesn't exist; MEMORY_DB_FAILURE on bind/step error. */
static int load_user_identity(int user_id, auth_user_identity_t *out_identity) {
   if (!out_identity)
      return MEMORY_DB_FAILURE;
   memset(out_identity, 0, sizeof(*out_identity));

   int rc = auth_db_get_user_identity(user_id, out_identity);
   if (rc == AUTH_DB_NOT_FOUND)
      return MEMORY_DB_NOT_FOUND;
   if (rc != AUTH_DB_SUCCESS)
      return MEMORY_DB_FAILURE;

   /* Trailing-partial-UTF-8 trim on every TEXT field — auth_db_get_user_identity
    * applies a hard byte cap on each column, which can split a multi-byte
    * sequence at the cap boundary.  The downstream tokenizer (memory_make_
    * canonical_name) is byte-oriented and needs valid input. */
   trim_trailing_partial_utf8(out_identity->real_name);
   trim_trailing_partial_utf8(out_identity->preferred_address);
   trim_trailing_partial_utf8(out_identity->identity_aliases);
   return MEMORY_DB_SUCCESS;
}

/* Build the synthetic memory_entity_t for the dry-run-with-no-self path
 * from the user's identity record.
 *
 *   name           = real_name (display)
 *   entity_type    = "person"
 *   canonical_name = tokens(real_name) ∪ tokens(each alias line)
 *
 * preferred_address is intentionally NOT used here — it's display-only
 * (system prompt, UI), not a search anchor.  If the same string also
 * appears on a candidate's canonical_name, token-Jaccard will still pick
 * it up via real_name overlap.
 *
 * Aliases parsing matches the brief: split on '\n', strip whitespace per
 * token, drop empties, dedupe case-insensitive, canonicalize each kept
 * line, append unique tokens to the synthetic canonical_name (whitespace-
 * separated so memory_alias_compute_name_jaccard's tokenizer sees them as
 * separate tokens). */
static void build_synthetic_self_entity(int user_id,
                                        const auth_user_identity_t *identity,
                                        memory_entity_t *out_synth) {
   memset(out_synth, 0, sizeof(*out_synth));
   out_synth->id = 0; /* sentinel — score_pair_full's DB-touching helpers
                       * (relations, contacts, embedding cache) all return
                       * 0 for id=0, which is the correct behavior for a
                       * not-yet-materialized entity. */
   out_synth->user_id = user_id;
   strncpy(out_synth->name, identity->real_name, MEMORY_ENTITY_NAME_MAX - 1);
   out_synth->name[MEMORY_ENTITY_NAME_MAX - 1] = '\0';
   strncpy(out_synth->entity_type, "person", MEMORY_ENTITY_TYPE_MAX - 1);
   out_synth->entity_type[MEMORY_ENTITY_TYPE_MAX - 1] = '\0';

   /* Start canonical_name with real_name canonicalized; this is the
    * primary token surface. */
   char canon[MEMORY_ENTITY_NAME_MAX];
   memory_make_canonical_name(identity->real_name, canon, sizeof(canon));

   /* Union with alias-line tokens.  Up to 16 aliases tracked for dedupe. */
   if (identity->identity_aliases[0] != '\0') {
      char buf[AUTH_IDENTITY_ALIASES_MAX];
      strncpy(buf, identity->identity_aliases, sizeof(buf) - 1);
      buf[sizeof(buf) - 1] = '\0';

      char *seen[16];
      int seen_count = 0;
      char *save = NULL;
      for (char *line = strtok_r(buf, "\n", &save); line != NULL && seen_count < 16;
           line = strtok_r(NULL, "\n", &save)) {
         while (*line == ' ' || *line == '\t' || *line == '\r')
            line++;
         char *end = line + strlen(line);
         while (end > line && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
            end--;
         *end = '\0';
         if (*line == '\0')
            continue;
         /* Case-insensitive dedupe. */
         bool dup = false;
         for (int i = 0; i < seen_count; i++) {
            if (strcasecmp(seen[i], line) == 0) {
               dup = true;
               break;
            }
         }
         if (dup)
            continue;
         seen[seen_count++] = line;

         char canon_alias[MEMORY_ENTITY_NAME_MAX];
         memory_make_canonical_name(line, canon_alias, sizeof(canon_alias));
         if (!canon_alias[0])
            continue;
         size_t cur = strlen(canon);
         size_t need = (cur > 0 ? 1 : 0) + strlen(canon_alias);
         if (cur + need + 1 >= sizeof(canon))
            break;
         if (cur > 0) {
            canon[cur++] = ' ';
            canon[cur] = '\0';
         }
         strncat(canon, canon_alias, sizeof(canon) - cur - 1);
      }
   }
   strncpy(out_synth->canonical_name, canon, MEMORY_ENTITY_NAME_MAX - 1);
   out_synth->canonical_name[MEMORY_ENTITY_NAME_MAX - 1] = '\0';

   out_synth->mention_count = 0;
   out_synth->first_seen = time(NULL);
   out_synth->last_seen = 0;
}

int memory_alias_link_user_self_run(int user_id,
                                    bool dry_run,
                                    memory_alias_link_user_self_result_t *result) {
   if (!result || user_id <= 0)
      return MEMORY_DB_FAILURE;
   memset(result, 0, sizeof(*result));

   /* Step 1: locate (or seed) the user-self canonical. */
   int64_t self_id = 0;
   int find_rc = find_user_self_id(user_id, &self_id);
   if (find_rc == MEMORY_DB_FAILURE)
      return MEMORY_DB_FAILURE;

   /* Load the user's identity record up-front — used for both the seed
    * path (when no is_user_self=1 row exists) and the synthetic-self
    * dry-run path.  v44 (Phase 1.5) replaces the previous username-as-
    * identity reach-around. */
   auth_user_identity_t identity;
   memset(&identity, 0, sizeof(identity));
   int id_rc = load_user_identity(user_id, &identity);
   if (id_rc == MEMORY_DB_NOT_FOUND)
      return MEMORY_DB_NOT_FOUND;
   if (id_rc != MEMORY_DB_SUCCESS)
      return MEMORY_DB_FAILURE;

   /* Phase 1.5 Ckpt D gate: real_name must be set (non-empty, not all
    * whitespace) for either the synthetic seed OR the on-commit seeded
    * canonical to have a meaningful name to anchor.  Without it the run
    * would degrade silently; the explicit error code lets the admin
    * handler surface a "Configure WebUI Settings → User → Real name"
    * hint to the operator. */
   const char *rn = identity.real_name;
   while (*rn == ' ' || *rn == '\t' || *rn == '\r' || *rn == '\n')
      rn++;
   if (*rn == '\0')
      return MEMORY_DB_REAL_NAME_REQUIRED;

   if (find_rc == MEMORY_DB_SUCCESS && self_id > 0) {
      memory_entity_t self_ent;
      if (load_entity_full(user_id, self_id, &self_ent, NULL, NULL) != MEMORY_DB_SUCCESS) {
         return MEMORY_DB_FAILURE;
      }
      result->self_entity_id = self_id;
      result->self_was_seeded = false;
      strncpy(result->self_canonical_name, self_ent.canonical_name, MEMORY_ENTITY_NAME_MAX - 1);
      result->self_canonical_name[MEMORY_ENTITY_NAME_MAX - 1] = '\0';
   } else {
      /* No is_user_self=1 row exists — per design §8 Path B step 1, first
       * try to find an existing entity that matches the synthetic strongly
       * (composite ≥ MEMORY_ALIAS_SELF_PROMOTION_THRESHOLD) and promote it
       * (UPDATE is_user_self=1).  Only if no strong match exists do we seed
       * a fresh entity.  Phase 1.5 Ckpt D added an explicit real_name gate
       * above this point; an empty real_name has already returned an error. */
      char display_name[MEMORY_ENTITY_NAME_MAX];
      strncpy(display_name, identity.real_name, sizeof(display_name) - 1);
      display_name[sizeof(display_name) - 1] = '\0';

      char canonical_name[MEMORY_ENTITY_NAME_MAX];
      memory_make_canonical_name(display_name, canonical_name, sizeof(canonical_name));

      /* Step 1 — search for a promotion candidate via the synthetic resolver.
       * Reuses the Stage 1-6 cascade with use_synth_self=true; returns the
       * highest-composite match (or 0 if no candidate cleared the cascade).
       *
       * Stage 1 (exact canonical_name match) is a fast-path that bypasses
       * scoring — composite_score stays 0, but the match itself is the
       * strongest possible signal (the user's real_name canonicalizes to
       * an entity that already exists).  Treat Stage 1 as an unconditional
       * promote-trigger; the composite-threshold gate applies only to
       * Stage 6 winners (fuzzy matches via name similarity + bonuses). */
      memory_alias_resolve_t self_resolve;
      memset(&self_resolve, 0, sizeof(self_resolve));
      int resolve_rc = memory_db_entity_resolve_alias_for_self(user_id, canonical_name, "person",
                                                               &self_resolve);

      bool stage1_hit = (resolve_rc == MEMORY_DB_SUCCESS && self_resolve.resolved_id > 0 &&
                         self_resolve.matched_stage == 1);
      bool stage6_strong = (resolve_rc == MEMORY_DB_SUCCESS && self_resolve.resolved_id > 0 &&
                            self_resolve.matched_stage == 6 &&
                            self_resolve.evidence.composite_score >=
                                MEMORY_ALIAS_SELF_PROMOTION_THRESHOLD);
      bool promoted = false;
      if (stage1_hit || stage6_strong) {
         /* Strong match — promote it (commit) or report it (dry-run). */
         if (!dry_run) {
            int prc = promote_to_user_self_entity(user_id, self_resolve.resolved_id);
            if (prc != MEMORY_DB_SUCCESS) {
               return MEMORY_DB_FAILURE;
            }
            self_id = self_resolve.resolved_id;
         } else {
            self_id = self_resolve.resolved_id;
         }
         promoted = true;
         result->self_was_promoted = true;
         result->self_was_seeded = false;

         /* Pull the canonical_name from the entity row so the report
          * accurately reflects what got promoted. */
         memory_entity_t self_ent;
         if (load_entity_full(user_id, self_id, &self_ent, NULL, NULL) == MEMORY_DB_SUCCESS) {
            strncpy(result->self_canonical_name, self_ent.canonical_name,
                    MEMORY_ENTITY_NAME_MAX - 1);
            result->self_canonical_name[MEMORY_ENTITY_NAME_MAX - 1] = '\0';
         } else {
            strncpy(result->self_canonical_name, canonical_name, MEMORY_ENTITY_NAME_MAX - 1);
            result->self_canonical_name[MEMORY_ENTITY_NAME_MAX - 1] = '\0';
         }
         result->self_entity_id = self_id;
      }

      if (!promoted) {
         /* Step 2 — fall through to fresh seed (commit) or no-self preview (dry-run). */
         if (!dry_run) {
            int seed_rc = seed_user_self_entity(user_id, display_name, canonical_name, &self_id);
            if (seed_rc == MEMORY_DB_SELF_NAME_COLLISION) {
               return MEMORY_DB_SELF_NAME_COLLISION;
            }
            if (seed_rc != MEMORY_DB_SUCCESS || self_id <= 0) {
               return MEMORY_DB_FAILURE;
            }
            result->self_was_seeded = true;
            result->self_was_promoted = false;
         } else {
            /* Dry-run preview when no canonical exists yet: self_id stays 0
             * (the report renders "(would create)") and the candidate scoring
             * loop below uses a synthetic entity for accurate band routing. */
            self_id = 0;
            result->self_was_seeded = true;
            result->self_was_promoted = false;
         }
         result->self_entity_id = self_id;
         strncpy(result->self_canonical_name, canonical_name, MEMORY_ENTITY_NAME_MAX - 1);
         result->self_canonical_name[MEMORY_ENTITY_NAME_MAX - 1] = '\0';
      }
   }

   /* Build the self-entity used for dry-run pair scoring.  Three cases:
    *   (a) dry-run + no canonical (self_id == 0): build synthetic from
    *       real_name + identity_aliases (use_synth_self path).
    *   (b) dry-run + would-promote (self_id > 0 && self_was_promoted):
    *       load the would-promoted entity but treat it as user_self for
    *       scoring (the DB UPDATE hasn't fired yet in dry-run, so
    *       memory_db_entity_score_pair would see is_user_self=0 on
    *       both sides and the bonus couldn't fire).  Use score_pair_full
    *       directly with src_is_user_self=true.
    *   (c) commit (any flavor) or pre-existing canonical: fall through
    *       to memory_db_entity_score_pair / consider_auto_merge which
    *       reads is_user_self from the DB directly. */
   memory_entity_t self_ent_for_dryrun;
   bool use_synth_self = (dry_run && self_id == 0);
   bool use_promote_dryrun = (dry_run && self_id > 0 && result->self_was_promoted);
   if (use_synth_self) {
      build_synthetic_self_entity(user_id, &identity, &self_ent_for_dryrun);
   } else if (use_promote_dryrun) {
      if (load_entity_full(user_id, self_id, &self_ent_for_dryrun, NULL, NULL) !=
          MEMORY_DB_SUCCESS) {
         return MEMORY_DB_FAILURE;
      }
   }

   /* Step 2: iterate every other canonical entity for the user. */
   int64_t entity_ids[MEMORY_ALIAS_LINK_USER_SELF_MAX_ROWS];
   int entity_count = 0;
   if (list_canonical_entity_ids(user_id, self_id, entity_ids, MEMORY_ALIAS_LINK_USER_SELF_MAX_ROWS,
                                 &entity_count) != MEMORY_DB_SUCCESS) {
      return MEMORY_DB_FAILURE;
   }
   result->considered = entity_count;

   for (int i = 0; i < entity_count; i++) {
      int64_t eid = entity_ids[i];

      memory_entity_t cand;
      if (load_entity_full(user_id, eid, &cand, NULL, NULL) != MEMORY_DB_SUCCESS) {
         continue;
      }

      memory_alias_link_user_self_row_t *row = NULL;
      if (result->row_count < MEMORY_ALIAS_LINK_USER_SELF_MAX_ROWS) {
         row = &result->rows[result->row_count++];
         row->entity_id = eid;
         strncpy(row->canonical_name, cand.canonical_name, MEMORY_ENTITY_NAME_MAX - 1);
         row->canonical_name[MEMORY_ENTITY_NAME_MAX - 1] = '\0';
         strncpy(row->entity_type, cand.entity_type, MEMORY_ENTITY_TYPE_MAX - 1);
         row->entity_type[MEMORY_ENTITY_TYPE_MAX - 1] = '\0';
      }

      if (dry_run) {
         memory_alias_evidence_t ev;
         memset(&ev, 0, sizeof(ev));
         if (use_synth_self || use_promote_dryrun) {
            /* Both dry-run cases use score_pair_full directly so we can
             * flag the self side as user_self even when the DB doesn't
             * carry is_user_self=1 yet (synthetic has id=0; promote-dryrun
             * is "would promote", UPDATE deferred until commit).  This
             * lets user_self_bonus_applies fire correctly on candidates
             * whose name matches the username/alias-substring conditions
             * or who are the "user" allow-list token.
             *
             * Phase 1.5 fold-in: pass the allow-list token flag through
             * for the canonical "user" entity — the unconditional bonus
             * branch fires regardless of whether the operator's username
             * happens to be a substring of "user". */
            bool other_token = (strcmp(cand.canonical_name, "user") == 0);
            score_pair_full(user_id, &cand, /* src_is_user_self */ false, &self_ent_for_dryrun,
                            /* tgt_is_user_self */ true, other_token, &ev);
         } else if (memory_db_entity_score_pair(user_id, eid, self_id, &ev) != MEMORY_DB_SUCCESS) {
            if (row)
               row->outcome = MEMORY_ALIAS_OUTCOME_REJECTED;
            result->rejected++;
            continue;
         }
         if (row) {
            row->composite_score = ev.composite_score;
            row->user_self_bonus_applied = ev.user_self_bonus_applied;
         }
         if (ev.composite_score >= MEMORY_ALIAS_AUTO_THRESHOLD) {
            if (row)
               row->outcome = MEMORY_ALIAS_OUTCOME_AUTO_MERGED;
            result->auto_merged++;
         } else if (ev.composite_score >= MEMORY_ALIAS_REVIEW_THRESHOLD) {
            if (row)
               row->outcome = MEMORY_ALIAS_OUTCOME_PROPOSED;
            result->proposed++;
         } else {
            if (row)
               row->outcome = MEMORY_ALIAS_OUTCOME_REJECTED;
            result->rejected++;
         }
      } else {
         /* Commit path: score the candidate against the user-self
          * specifically, NOT via consider_auto_merge (which would find
          * the candidate's best generic match — for "llama 3.1" that's
          * its sibling "llama", not the user-self).  link-user-self
          * semantics: cluster scoring is always against the chosen
          * canonical, with band routing to alias_link / proposal. */
         memory_alias_evidence_t ev;
         memset(&ev, 0, sizeof(ev));
         memory_entity_t self_ent;
         if (load_entity_full(user_id, self_id, &self_ent, NULL, NULL) != MEMORY_DB_SUCCESS) {
            if (row)
               row->outcome = MEMORY_ALIAS_OUTCOME_REJECTED;
            result->rejected++;
            continue;
         }
         /* Allow-list flag: candidate is "user" canonical → unconditional bonus.
          * Self side is the would-be-canonical; src_is_user_self=false (cand) and
          * tgt_is_user_self=true (self) so user_self_bonus_applies fires correctly. */
         bool other_token = (strcmp(cand.canonical_name, "user") == 0);
         score_pair_full(user_id, &cand, /* src_is_user_self */ false, &self_ent,
                         /* tgt_is_user_self */ true, other_token, &ev);

         int outcome = MEMORY_ALIAS_OUTCOME_REJECTED;
         int64_t link_id = 0, proposal_id = 0;
         if (ev.composite_score >= MEMORY_ALIAS_AUTO_THRESHOLD) {
            /* Soft-link: alias source (cand) to target (self).  Reason
             * tag identifies the operator workflow that fired this. */
            int link_rc = memory_db_entity_alias_link(user_id, eid, self_id, "soft",
                                                      "link-user-self", ev.composite_score,
                                                      /* evidence_json */ NULL, &link_id);
            if (link_rc == MEMORY_DB_SUCCESS) {
               outcome = MEMORY_ALIAS_OUTCOME_AUTO_MERGED;
               result->auto_merged++;
            } else {
               outcome = MEMORY_ALIAS_OUTCOME_REJECTED;
               result->rejected++;
            }
         } else if (ev.composite_score >= MEMORY_ALIAS_REVIEW_THRESHOLD) {
            /* Queue for operator review via the WebUI Suggested-Merges panel. */
            int prop_rc = insert_merge_proposal(user_id, eid, self_id, ev.composite_score,
                                                /* evidence_json */ NULL, &proposal_id);
            if (prop_rc == MEMORY_DB_SUCCESS) {
               outcome = MEMORY_ALIAS_OUTCOME_PROPOSED;
               result->proposed++;
            } else {
               outcome = MEMORY_ALIAS_OUTCOME_REJECTED;
               result->rejected++;
            }
         } else {
            outcome = MEMORY_ALIAS_OUTCOME_REJECTED;
            result->rejected++;
         }

         if (row) {
            row->outcome = outcome;
            row->composite_score = ev.composite_score;
            row->user_self_bonus_applied = ev.user_self_bonus_applied;
            row->link_id = link_id;
            row->proposal_id = proposal_id;
         }
      }
   }
   return MEMORY_DB_SUCCESS;
}
