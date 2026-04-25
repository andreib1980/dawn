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
 * WebUI Memory Handlers - Memory management for WebSocket clients
 *
 * This module handles WebSocket messages for memory operations:
 * - get_memory_stats, list_memory_facts, list_memory_preferences
 * - list_memory_summaries, search_memory, delete_memory_fact
 * - delete_memory_preference, delete_all_memories
 */

#include <string.h>

#include "logging.h"
#include "memory/contacts_db.h"
#include "memory/memory_db.h"
#include "memory/memory_filter.h"
#include "memory/memory_similarity.h"
#include "webui/webui_internal.h"

/* Default pagination limits */
#define DEFAULT_MEMORY_LIMIT 20
#define MAX_MEMORY_LIMIT 50

/* =============================================================================
 * Memory Statistics Handler
 * ============================================================================= */

/**
 * @brief Get memory statistics for the current user
 */
void handle_get_memory_stats(ws_connection_t *conn) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("get_memory_stats_response"));
   json_object *resp_payload = json_object_new_object();

   memory_stats_t stats;
   int result = memory_db_get_stats(conn->auth_user_id, &stats);

   if (result == MEMORY_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "fact_count", json_object_new_int(stats.fact_count));
      json_object_object_add(resp_payload, "pref_count", json_object_new_int(stats.pref_count));
      json_object_object_add(resp_payload, "summary_count",
                             json_object_new_int(stats.summary_count));
      json_object_object_add(resp_payload, "entity_count", json_object_new_int(stats.entity_count));
      json_object_object_add(resp_payload, "oldest_fact", json_object_new_int64(stats.oldest_fact));
      json_object_object_add(resp_payload, "newest_fact", json_object_new_int64(stats.newest_fact));

      /* Count contacts for this user */
      int contact_count = contacts_count(conn->auth_user_id);
      if (contact_count < 0)
         contact_count = 0;
      json_object_object_add(resp_payload, "contact_count", json_object_new_int(contact_count));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to get memory stats"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Memory Facts Handlers
 * ============================================================================= */

/**
 * @brief List memory facts for the current user (paginated)
 */
void handle_list_memory_facts(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("list_memory_facts_response"));
   json_object *resp_payload = json_object_new_object();

   /* Parse pagination params */
   int limit = DEFAULT_MEMORY_LIMIT;
   int offset = 0;
   if (payload) {
      json_object *limit_obj, *offset_obj;
      if (json_object_object_get_ex(payload, "limit", &limit_obj)) {
         limit = json_object_get_int(limit_obj);
         if (limit < 1 || limit > MAX_MEMORY_LIMIT) {
            limit = DEFAULT_MEMORY_LIMIT;
         }
      }
      if (json_object_object_get_ex(payload, "offset", &offset_obj)) {
         offset = json_object_get_int(offset_obj);
         if (offset < 0) {
            offset = 0;
         }
      }
   }

   /* Query database */
   memory_fact_t facts[MAX_MEMORY_LIMIT];
   memset(facts, 0, sizeof(facts));
   int count = memory_db_fact_list(conn->auth_user_id, facts, limit, offset);

   if (count >= 0) {
      json_object *facts_array = json_object_new_array();
      for (int i = 0; i < count; i++) {
         json_object *fact_obj = json_object_new_object();
         json_object_object_add(fact_obj, "id", json_object_new_int64(facts[i].id));
         json_object_object_add(fact_obj, "fact_text", json_object_new_string(facts[i].fact_text));
         json_object_object_add(fact_obj, "confidence",
                                json_object_new_double(facts[i].confidence));
         json_object_object_add(fact_obj, "source", json_object_new_string(facts[i].source));
         json_object_object_add(fact_obj, "created_at", json_object_new_int64(facts[i].created_at));
         json_object_object_add(fact_obj, "last_accessed",
                                json_object_new_int64(facts[i].last_accessed));
         json_object_object_add(fact_obj, "access_count",
                                json_object_new_int(facts[i].access_count));
         json_object_array_add(facts_array, fact_obj);
      }

      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "facts", facts_array);
      json_object_object_add(resp_payload, "count", json_object_new_int(count));
      json_object_object_add(resp_payload, "has_more", json_object_new_boolean(count == limit));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to list memory facts"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/**
 * @brief Delete a memory fact
 */
void handle_delete_memory_fact(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("delete_memory_fact_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get fact ID */
   json_object *id_obj;
   if (!json_object_object_get_ex(payload, "fact_id", &id_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing fact_id"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   int64_t fact_id = json_object_get_int64(id_obj);
   int result = memory_db_fact_delete(fact_id, conn->auth_user_id);

   if (result == MEMORY_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message", json_object_new_string("Fact deleted"));
      OLOG_INFO("WebUI: User %d deleted memory fact %lld", conn->auth_user_id, (long long)fact_id);
   } else if (result == MEMORY_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Fact not found"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to delete fact"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Memory Preferences Handlers
 * ============================================================================= */

/**
 * @brief List memory preferences for the current user
 */
void handle_list_memory_preferences(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("list_memory_preferences_response"));
   json_object *resp_payload = json_object_new_object();

   /* Parse pagination params */
   int limit = DEFAULT_MEMORY_LIMIT;
   int offset = 0;
   if (payload) {
      json_object *limit_obj, *offset_obj;
      if (json_object_object_get_ex(payload, "limit", &limit_obj)) {
         limit = json_object_get_int(limit_obj);
         if (limit < 1 || limit > MAX_MEMORY_LIMIT) {
            limit = DEFAULT_MEMORY_LIMIT;
         }
      }
      if (json_object_object_get_ex(payload, "offset", &offset_obj)) {
         offset = json_object_get_int(offset_obj);
         if (offset < 0) {
            offset = 0;
         }
      }
   }

   /* Query database */
   memory_preference_t prefs[MAX_MEMORY_LIMIT];
   memset(prefs, 0, sizeof(prefs));
   int count = memory_db_pref_list(conn->auth_user_id, prefs, limit, offset);

   if (count >= 0) {
      json_object *prefs_array = json_object_new_array();
      for (int i = 0; i < count; i++) {
         json_object *pref_obj = json_object_new_object();
         json_object_object_add(pref_obj, "id", json_object_new_int64(prefs[i].id));
         json_object_object_add(pref_obj, "category", json_object_new_string(prefs[i].category));
         json_object_object_add(pref_obj, "value", json_object_new_string(prefs[i].value));
         json_object_object_add(pref_obj, "confidence",
                                json_object_new_double(prefs[i].confidence));
         json_object_object_add(pref_obj, "source", json_object_new_string(prefs[i].source));
         json_object_object_add(pref_obj, "created_at", json_object_new_int64(prefs[i].created_at));
         json_object_object_add(pref_obj, "updated_at", json_object_new_int64(prefs[i].updated_at));
         json_object_object_add(pref_obj, "reinforcement_count",
                                json_object_new_int(prefs[i].reinforcement_count));
         json_object_array_add(prefs_array, pref_obj);
      }

      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "preferences", prefs_array);
      json_object_object_add(resp_payload, "count", json_object_new_int(count));
      json_object_object_add(resp_payload, "has_more", json_object_new_boolean(count == limit));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to list preferences"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/**
 * @brief Delete a memory preference by category
 */
void handle_delete_memory_preference(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("delete_memory_preference_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get category */
   json_object *cat_obj;
   if (!json_object_object_get_ex(payload, "category", &cat_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing category"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   const char *category = json_object_get_string(cat_obj);
   int result = memory_db_pref_delete(conn->auth_user_id, category);

   if (result == MEMORY_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message", json_object_new_string("Preference deleted"));
      OLOG_INFO("WebUI: User %d deleted memory preference '%s'", conn->auth_user_id, category);
   } else if (result == MEMORY_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Preference not found"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to delete preference"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Memory Summaries Handler
 * ============================================================================= */

/**
 * @brief List memory summaries for the current user
 */
void handle_list_memory_summaries(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("list_memory_summaries_response"));
   json_object *resp_payload = json_object_new_object();

   /* Parse pagination params */
   int limit = DEFAULT_MEMORY_LIMIT;
   int offset = 0;
   if (payload) {
      json_object *limit_obj, *offset_obj;
      if (json_object_object_get_ex(payload, "limit", &limit_obj)) {
         limit = json_object_get_int(limit_obj);
         if (limit < 1 || limit > MAX_MEMORY_LIMIT) {
            limit = DEFAULT_MEMORY_LIMIT;
         }
      }
      if (json_object_object_get_ex(payload, "offset", &offset_obj)) {
         offset = json_object_get_int(offset_obj);
         if (offset < 0) {
            offset = 0;
         }
      }
   }

   /* Query database */
   memory_summary_t summaries[MAX_MEMORY_LIMIT];
   memset(summaries, 0, sizeof(summaries));
   int count = memory_db_summary_list(conn->auth_user_id, summaries, limit, offset);

   if (count >= 0) {
      json_object *summaries_array = json_object_new_array();
      for (int i = 0; i < count; i++) {
         json_object *summary_obj = json_object_new_object();
         json_object_object_add(summary_obj, "id", json_object_new_int64(summaries[i].id));
         json_object_object_add(summary_obj, "session_id",
                                json_object_new_string(summaries[i].session_id));
         json_object_object_add(summary_obj, "summary",
                                json_object_new_string(summaries[i].summary));
         json_object_object_add(summary_obj, "topics", json_object_new_string(summaries[i].topics));
         json_object_object_add(summary_obj, "sentiment",
                                json_object_new_string(summaries[i].sentiment));
         json_object_object_add(summary_obj, "created_at",
                                json_object_new_int64(summaries[i].created_at));
         json_object_object_add(summary_obj, "message_count",
                                json_object_new_int(summaries[i].message_count));
         json_object_object_add(summary_obj, "duration_seconds",
                                json_object_new_int(summaries[i].duration_seconds));
         json_object_array_add(summaries_array, summary_obj);
      }

      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "summaries", summaries_array);
      json_object_object_add(resp_payload, "count", json_object_new_int(count));
      json_object_object_add(resp_payload, "has_more", json_object_new_boolean(count == limit));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to list summaries"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/**
 * @brief Delete a memory summary
 */
void handle_delete_memory_summary(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("delete_memory_summary_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get summary ID */
   json_object *id_obj;
   if (!json_object_object_get_ex(payload, "summary_id", &id_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing summary_id"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   int64_t summary_id = json_object_get_int64(id_obj);
   int result = memory_db_summary_delete(summary_id, conn->auth_user_id);

   if (result == MEMORY_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message", json_object_new_string("Summary deleted"));
      OLOG_INFO("WebUI: User %d deleted memory summary %lld", conn->auth_user_id,
                (long long)summary_id);
   } else if (result == MEMORY_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Summary not found"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to delete summary"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Memory Entities Handlers
 * ============================================================================= */

/**
 * @brief List memory entities with their relations for the current user
 */
void handle_list_memory_entities(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("list_memory_entities_response"));
   json_object *resp_payload = json_object_new_object();

   /* Parse pagination params */
   int limit = DEFAULT_MEMORY_LIMIT;
   int offset = 0;
   if (payload) {
      json_object *limit_obj, *offset_obj;
      if (json_object_object_get_ex(payload, "limit", &limit_obj)) {
         limit = json_object_get_int(limit_obj);
         if (limit < 1 || limit > MAX_MEMORY_LIMIT) {
            limit = DEFAULT_MEMORY_LIMIT;
         }
      }
      if (json_object_object_get_ex(payload, "offset", &offset_obj)) {
         offset = json_object_get_int(offset_obj);
         if (offset < 0) {
            offset = 0;
         }
      }
   }

   /* Load entities (sorted by mention_count desc) */
   memory_entity_t entities[MAX_MEMORY_LIMIT];
   memset(entities, 0, sizeof(entities));
   int count = memory_db_entity_list(conn->auth_user_id, entities, limit, offset);

/* Bulk-load all relations in a single query (avoids N+1 per-entity queries) */
#define MAX_RELATIONS_BULK 400
   memory_relation_t all_rels[MAX_RELATIONS_BULK];
   int total_rels = 0;
   if (count > 0) {
      memset(all_rels, 0, sizeof(all_rels));
      total_rels = memory_db_relation_list_all_by_user(conn->auth_user_id, all_rels,
                                                       MAX_RELATIONS_BULK);
      if (total_rels < 0)
         total_rels = 0;
   }

   if (count >= 0) {
      json_object *entities_array = json_object_new_array();
      for (int i = 0; i < count; i++) {
         json_object *entity_obj = json_object_new_object();
         json_object_object_add(entity_obj, "id", json_object_new_int64(entities[i].id));
         json_object_object_add(entity_obj, "name", json_object_new_string(entities[i].name));
         json_object_object_add(entity_obj, "entity_type",
                                json_object_new_string(entities[i].entity_type));
         json_object_object_add(entity_obj, "mention_count",
                                json_object_new_int(entities[i].mention_count));
         json_object_object_add(entity_obj, "first_seen",
                                json_object_new_int64(entities[i].first_seen));
         json_object_object_add(entity_obj, "last_seen",
                                json_object_new_int64(entities[i].last_seen));

         json_object *relations_array = json_object_new_array();

         /* Scan bulk relations: outgoing (this entity is subject) */
         for (int r = 0; r < total_rels; r++) {
            if (all_rels[r].subject_entity_id != entities[i].id)
               continue;
            json_object *rel_obj = json_object_new_object();
            json_object_object_add(rel_obj, "relation",
                                   json_object_new_string(all_rels[r].relation));
            json_object_object_add(rel_obj, "object_name",
                                   json_object_new_string(all_rels[r].object_name));
            json_object_object_add(rel_obj, "object_entity_id",
                                   json_object_new_int64(all_rels[r].object_entity_id));
            json_object_object_add(rel_obj, "direction", json_object_new_string("out"));
            json_object_object_add(rel_obj, "confidence",
                                   json_object_new_double(all_rels[r].confidence));
            json_object_array_add(relations_array, rel_obj);
         }

         /* Scan bulk relations: incoming (this entity is object) */
         for (int r = 0; r < total_rels; r++) {
            if (all_rels[r].object_entity_id != entities[i].id)
               continue;
            /* For incoming, find subject entity name */
            const char *subj_name = "";
            for (int e = 0; e < count; e++) {
               if (entities[e].id == all_rels[r].subject_entity_id) {
                  subj_name = entities[e].name;
                  break;
               }
            }
            json_object *rel_obj = json_object_new_object();
            json_object_object_add(rel_obj, "relation",
                                   json_object_new_string(all_rels[r].relation));
            json_object_object_add(rel_obj, "object_name", json_object_new_string(subj_name));
            json_object_object_add(rel_obj, "object_entity_id",
                                   json_object_new_int64(all_rels[r].subject_entity_id));
            json_object_object_add(rel_obj, "direction", json_object_new_string("in"));
            json_object_object_add(rel_obj, "confidence",
                                   json_object_new_double(all_rels[r].confidence));
            json_object_array_add(relations_array, rel_obj);
         }

         json_object_object_add(entity_obj, "relations", relations_array);
         json_object_array_add(entities_array, entity_obj);
      }

      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "entities", entities_array);
      json_object_object_add(resp_payload, "count", json_object_new_int(count));
      json_object_object_add(resp_payload, "has_more", json_object_new_boolean(count == limit));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to list entities"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/**
 * @brief Delete a memory entity and its relations
 */
void handle_delete_memory_entity(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("delete_memory_entity_response"));
   json_object *resp_payload = json_object_new_object();

   json_object *id_obj;
   if (!json_object_object_get_ex(payload, "entity_id", &id_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing entity_id"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   int64_t entity_id = json_object_get_int64(id_obj);
   int result = memory_db_entity_delete(entity_id, conn->auth_user_id);

   if (result == MEMORY_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message", json_object_new_string("Entity deleted"));
      OLOG_INFO("WebUI: User %d deleted memory entity %lld", conn->auth_user_id,
                (long long)entity_id);
   } else if (result == MEMORY_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Entity not found"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to delete entity"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Entity Merge Handler
 * ============================================================================= */

/**
 * @brief Merge two memory entities (source absorbed into target)
 */
void handle_merge_memory_entities(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("merge_memory_entities_response"));
   json_object *resp_payload = json_object_new_object();

   json_object *source_obj, *target_obj;
   if (!json_object_object_get_ex(payload, "source_id", &source_obj) ||
       !json_object_object_get_ex(payload, "target_id", &target_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing source_id or target_id"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   int64_t source_id = json_object_get_int64(source_obj);
   int64_t target_id = json_object_get_int64(target_obj);

   if (source_id == target_id) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Cannot merge an entity with itself"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   int result = memory_db_entity_merge(conn->auth_user_id, source_id, target_id);

   if (result == MEMORY_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message", json_object_new_string("Entities merged"));
      OLOG_INFO("WebUI: User %d merged entity %lld into %lld", conn->auth_user_id,
                (long long)source_id, (long long)target_id);
   } else if (result == MEMORY_DB_NOT_FOUND) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Entity not found or not owned by user"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to merge entities"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Memory Search Handler
 * ============================================================================= */

/**
 * @brief Search memory facts by keyword
 */
void handle_search_memory(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("search_memory_response"));
   json_object *resp_payload = json_object_new_object();

   /* Get search query */
   json_object *query_obj;
   if (!json_object_object_get_ex(payload, "query", &query_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing query"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   const char *query = json_object_get_string(query_obj);
   if (!query || strlen(query) == 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Empty query"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   /* Limit query length to prevent resource exhaustion */
   if (strlen(query) > 256) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Query too long"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   /* Search facts */
   memory_fact_t facts[MAX_MEMORY_LIMIT];
   memset(facts, 0, sizeof(facts));
   int count = memory_db_fact_search(conn->auth_user_id, query, facts, MAX_MEMORY_LIMIT);

   /* Also search summaries */
   memory_summary_t summaries[MEMORY_MAX_SUMMARIES];
   memset(summaries, 0, sizeof(summaries));
   int summary_count = memory_db_summary_search(conn->auth_user_id, query, summaries,
                                                MEMORY_MAX_SUMMARIES);

   if (count >= 0 && summary_count >= 0) {
      /* Build facts array */
      json_object *facts_array = json_object_new_array();
      for (int i = 0; i < count; i++) {
         json_object *fact_obj = json_object_new_object();
         json_object_object_add(fact_obj, "id", json_object_new_int64(facts[i].id));
         json_object_object_add(fact_obj, "fact_text", json_object_new_string(facts[i].fact_text));
         json_object_object_add(fact_obj, "confidence",
                                json_object_new_double(facts[i].confidence));
         json_object_object_add(fact_obj, "source", json_object_new_string(facts[i].source));
         json_object_object_add(fact_obj, "created_at", json_object_new_int64(facts[i].created_at));
         json_object_array_add(facts_array, fact_obj);
      }

      /* Build summaries array */
      json_object *summaries_array = json_object_new_array();
      for (int i = 0; i < summary_count; i++) {
         json_object *summary_obj = json_object_new_object();
         json_object_object_add(summary_obj, "id", json_object_new_int64(summaries[i].id));
         json_object_object_add(summary_obj, "summary",
                                json_object_new_string(summaries[i].summary));
         json_object_object_add(summary_obj, "topics", json_object_new_string(summaries[i].topics));
         json_object_object_add(summary_obj, "created_at",
                                json_object_new_int64(summaries[i].created_at));
         json_object_array_add(summaries_array, summary_obj);
      }

      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "facts", facts_array);
      json_object_object_add(resp_payload, "summaries", summaries_array);
      json_object_object_add(resp_payload, "fact_count", json_object_new_int(count));
      json_object_object_add(resp_payload, "summary_count", json_object_new_int(summary_count));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Search failed"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Delete All Memories Handler
 * ============================================================================= */

/**
 * @brief Delete all memories for the current user
 *
 * Requires explicit confirmation via "confirm" field in payload.
 */
void handle_delete_all_memories(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("delete_all_memories_response"));
   json_object *resp_payload = json_object_new_object();

   /* Require explicit confirmation */
   json_object *confirm_obj;
   if (!json_object_object_get_ex(payload, "confirm", &confirm_obj) ||
       strcmp(json_object_get_string(confirm_obj), "DELETE") != 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Must confirm by setting confirm=\"DELETE\""));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   int result = memory_db_delete_user_memories(conn->auth_user_id);

   if (result == MEMORY_DB_SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "message",
                             json_object_new_string("All memories deleted"));
      OLOG_INFO("WebUI: User %d deleted all memories", conn->auth_user_id);
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to delete memories"));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Memory Export Handler
 * ============================================================================= */

/* Maximum items to export per type */
#define EXPORT_MAX_FACTS 500
#define EXPORT_MAX_PREFS 200
#define EXPORT_MAX_ENTITIES 200
#define EXPORT_MAX_RELATIONS 400

/**
 * @brief Export all memories for the current user
 *
 * Supports two formats:
 * - "json": Full DAWN JSON with metadata (lossless, for backup/restore)
 * - "text": Human-readable text (portable to other AIs)
 */
void handle_export_memories(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("export_memories_response"));
   json_object *resp_payload = json_object_new_object();

   /* Parse format (default: "json") */
   const char *format = "json";
   if (payload) {
      json_object *fmt_obj;
      if (json_object_object_get_ex(payload, "format", &fmt_obj)) {
         format = json_object_get_string(fmt_obj);
      }
   }

   /* Validate format */
   if (strcmp(format, "json") != 0 && strcmp(format, "text") != 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Invalid format (use 'json' or 'text')"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   if (strcmp(format, "text") == 0) {
      /* --- Human-readable text export --- */
      memory_fact_t *facts = calloc(EXPORT_MAX_FACTS, sizeof(memory_fact_t));
      memory_preference_t *prefs = calloc(EXPORT_MAX_PREFS, sizeof(memory_preference_t));
      if (!facts || !prefs) {
         free(facts);
         free(prefs);
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Memory allocation failed"));
         json_object_object_add(response, "payload", resp_payload);
         send_json_response(conn, response);
         json_object_put(response);
         return;
      }

      int fact_count = memory_db_fact_list(conn->auth_user_id, facts, EXPORT_MAX_FACTS, 0);
      int pref_count = memory_db_pref_list(conn->auth_user_id, prefs, EXPORT_MAX_PREFS, 0);
      if (fact_count < 0)
         fact_count = 0;
      if (pref_count < 0)
         pref_count = 0;

      /* Build text output: one line per memory */
      size_t buf_size = (size_t)(fact_count + pref_count + 10) * 600;
      char *text_buf = malloc(buf_size);
      if (!text_buf) {
         free(facts);
         free(prefs);
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Memory allocation failed"));
         json_object_object_add(response, "payload", resp_payload);
         send_json_response(conn, response);
         json_object_put(response);
         return;
      }

      size_t off = 0;
      off += snprintf(text_buf + off, buf_size - off, "# My Memories\n\n");

      if (fact_count > 0) {
         off += snprintf(text_buf + off, buf_size - off, "## Facts\n");
         for (int i = 0; i < fact_count && off < buf_size - 128; i++) {
            off += snprintf(text_buf + off, buf_size - off, "- %s\n", facts[i].fact_text);
         }
         off += snprintf(text_buf + off, buf_size - off, "\n");
      }

      if (pref_count > 0) {
         off += snprintf(text_buf + off, buf_size - off, "## Preferences\n");
         for (int i = 0; i < pref_count && off < buf_size - 128; i++) {
            off += snprintf(text_buf + off, buf_size - off, "- %s: %s\n", prefs[i].category,
                            prefs[i].value);
         }
      }

      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "format", json_object_new_string("text"));
      json_object_object_add(resp_payload, "data", json_object_new_string(text_buf));
      json_object_object_add(resp_payload, "fact_count", json_object_new_int(fact_count));
      json_object_object_add(resp_payload, "pref_count", json_object_new_int(pref_count));

      free(text_buf);
      free(facts);
      free(prefs);

   } else {
      /* --- DAWN JSON export (lossless with metadata) --- */
      memory_fact_t *facts = calloc(EXPORT_MAX_FACTS, sizeof(memory_fact_t));
      memory_preference_t *prefs = calloc(EXPORT_MAX_PREFS, sizeof(memory_preference_t));
      memory_entity_t *entities = calloc(EXPORT_MAX_ENTITIES, sizeof(memory_entity_t));
      memory_relation_t *relations = calloc(EXPORT_MAX_RELATIONS, sizeof(memory_relation_t));
      if (!facts || !prefs || !entities || !relations) {
         free(facts);
         free(prefs);
         free(entities);
         free(relations);
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Memory allocation failed"));
         json_object_object_add(response, "payload", resp_payload);
         send_json_response(conn, response);
         json_object_put(response);
         return;
      }

      int fact_count = memory_db_fact_list(conn->auth_user_id, facts, EXPORT_MAX_FACTS, 0);
      int pref_count = memory_db_pref_list(conn->auth_user_id, prefs, EXPORT_MAX_PREFS, 0);
      int entity_count = memory_db_entity_list(conn->auth_user_id, entities, EXPORT_MAX_ENTITIES,
                                               0);
      int relation_count = memory_db_relation_list_all_by_user(conn->auth_user_id, relations,
                                                               EXPORT_MAX_RELATIONS);
      if (fact_count < 0)
         fact_count = 0;
      if (pref_count < 0)
         pref_count = 0;
      if (entity_count < 0)
         entity_count = 0;
      if (relation_count < 0)
         relation_count = 0;

      /* Build export JSON */
      json_object *export_obj = json_object_new_object();
      json_object_object_add(export_obj, "version", json_object_new_int(1));
      json_object_object_add(export_obj, "format", json_object_new_string("dawn_memory"));
      json_object_object_add(export_obj, "exported_at", json_object_new_int64((int64_t)time(NULL)));

      /* Facts array */
      json_object *facts_arr = json_object_new_array();
      for (int i = 0; i < fact_count; i++) {
         json_object *f = json_object_new_object();
         json_object_object_add(f, "fact_text", json_object_new_string(facts[i].fact_text));
         json_object_object_add(f, "confidence", json_object_new_double(facts[i].confidence));
         json_object_object_add(f, "source", json_object_new_string(facts[i].source));
         json_object_object_add(f, "created_at", json_object_new_int64(facts[i].created_at));
         json_object_array_add(facts_arr, f);
      }
      json_object_object_add(export_obj, "facts", facts_arr);

      /* Preferences array */
      json_object *prefs_arr = json_object_new_array();
      for (int i = 0; i < pref_count; i++) {
         json_object *p = json_object_new_object();
         json_object_object_add(p, "category", json_object_new_string(prefs[i].category));
         json_object_object_add(p, "value", json_object_new_string(prefs[i].value));
         json_object_object_add(p, "confidence", json_object_new_double(prefs[i].confidence));
         json_object_object_add(p, "source", json_object_new_string(prefs[i].source));
         json_object_object_add(p, "created_at", json_object_new_int64(prefs[i].created_at));
         json_object_array_add(prefs_arr, p);
      }
      json_object_object_add(export_obj, "preferences", prefs_arr);

      /* Entities array */
      json_object *entities_arr = json_object_new_array();
      for (int i = 0; i < entity_count; i++) {
         json_object *e = json_object_new_object();
         json_object_object_add(e, "name", json_object_new_string(entities[i].name));
         json_object_object_add(e, "entity_type", json_object_new_string(entities[i].entity_type));
         json_object_object_add(e, "mention_count", json_object_new_int(entities[i].mention_count));
         json_object_object_add(e, "first_seen", json_object_new_int64(entities[i].first_seen));

         /* Attach relations for this entity */
         json_object *rels_arr = json_object_new_array();
         for (int r = 0; r < relation_count; r++) {
            if (relations[r].subject_entity_id != entities[i].id)
               continue;
            json_object *rel = json_object_new_object();
            json_object_object_add(rel, "relation", json_object_new_string(relations[r].relation));
            json_object_object_add(rel, "object_name",
                                   json_object_new_string(relations[r].object_name));
            json_object_object_add(rel, "confidence",
                                   json_object_new_double(relations[r].confidence));
            json_object_array_add(rels_arr, rel);
         }
         json_object_object_add(e, "relations", rels_arr);

         json_object_array_add(entities_arr, e);
      }
      json_object_object_add(export_obj, "entities", entities_arr);

      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
      json_object_object_add(resp_payload, "format", json_object_new_string("json"));
      json_object_object_add(resp_payload, "data", export_obj);
      json_object_object_add(resp_payload, "fact_count", json_object_new_int(fact_count));
      json_object_object_add(resp_payload, "pref_count", json_object_new_int(pref_count));
      json_object_object_add(resp_payload, "entity_count", json_object_new_int(entity_count));

      free(facts);
      free(prefs);
      free(entities);
      free(relations);
   }

   /* Copy format before response is freed (format may point into payload JSON) */
   bool is_text = (strcmp(format, "text") == 0);

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);

   OLOG_INFO("WebUI: User %d exported memories (format=%s)", conn->auth_user_id,
             is_text ? "text" : "json");
}

/* =============================================================================
 * Memory Import Handler
 * ============================================================================= */

/* Maximum items per import (limits DB queries on LWS service thread) */
#define IMPORT_MAX_LINES 200
#define IMPORT_MAX_TEXT_LEN (256 * 1024) /* 256KB max paste */

/**
 * @brief Check if a fact is a duplicate of existing memories
 *
 * Uses hash lookup + Jaccard similarity fallback.
 *
 * @return true if duplicate found
 */
static bool is_fact_duplicate(int user_id, const char *fact_text) {
   uint32_t hash = memory_normalize_and_hash(fact_text);
   if (hash == 0)
      return false;

   memory_fact_t existing[5];
   int found = memory_db_fact_find_by_hash(user_id, hash, existing, 5);
   if (found > 0)
      return true;

   /* Fallback: Jaccard similarity against recent matches */
   memory_fact_t similar[3];
   int sim_count = memory_db_fact_find_similar(user_id, fact_text, similar, 3);
   for (int i = 0; i < sim_count; i++) {
      if (memory_is_duplicate(fact_text, similar[i].fact_text, MEMORY_SIMILARITY_THRESHOLD))
         return true;
   }
   return false;
}

/**
 * @brief Import memories from JSON or plain text
 *
 * Two import paths:
 * - "json" format: DAWN JSON export (direct restore with metadata)
 * - "text" format: Plain text paste (from Claude/ChatGPT, one fact per line)
 *
 * Supports preview mode (commit=false) that returns what would be imported
 * without actually writing to the database.
 */
void handle_import_memories(ws_connection_t *conn, struct json_object *payload) {
   if (!conn_require_auth(conn)) {
      return;
   }

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("import_memories_response"));
   json_object *resp_payload = json_object_new_object();

   if (!payload) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing payload"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   /* Parse common fields */
   json_object *format_obj, *commit_obj;
   const char *format = "text";
   bool commit = false;

   if (json_object_object_get_ex(payload, "format", &format_obj))
      format = json_object_get_string(format_obj);
   if (json_object_object_get_ex(payload, "commit", &commit_obj))
      commit = json_object_get_boolean(commit_obj);

   /* Validate format */
   if (strcmp(format, "json") != 0 && strcmp(format, "text") != 0) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Invalid format (use 'json' or 'text')"));
      json_object_object_add(response, "payload", resp_payload);
      send_json_response(conn, response);
      json_object_put(response);
      return;
   }

   int imported_facts = 0;
   int imported_prefs = 0;
   int skipped_dupes = 0;
   int skipped_empty = 0;
   int skipped_blocked = 0;
   json_object *preview_arr = json_object_new_array();

   if (strcmp(format, "json") == 0) {
      /* --- DAWN JSON import --- */
      json_object *data_obj;
      if (!json_object_object_get_ex(payload, "data", &data_obj)) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error", json_object_new_string("Missing data"));
         json_object_put(preview_arr);
         json_object_object_add(response, "payload", resp_payload);
         send_json_response(conn, response);
         json_object_put(response);
         return;
      }

      /* Import facts */
      json_object *facts_arr;
      if (json_object_object_get_ex(data_obj, "facts", &facts_arr)) {
         int len = json_object_array_length(facts_arr);
         for (int i = 0; i < len && i < IMPORT_MAX_LINES; i++) {
            json_object *f = json_object_array_get_idx(facts_arr, i);
            json_object *text_obj, *conf_obj, *src_obj;

            if (!json_object_object_get_ex(f, "fact_text", &text_obj))
               continue;
            const char *text = json_object_get_string(text_obj);
            if (!text || strlen(text) == 0) {
               skipped_empty++;
               continue;
            }

            float confidence = 0.8f;
            if (json_object_object_get_ex(f, "confidence", &conf_obj))
               confidence = (float)json_object_get_double(conf_obj);

            /* Truncate to max fact length (same as text path) */
            char truncated[MEMORY_FACT_TEXT_MAX];
            strncpy(truncated, text, sizeof(truncated) - 1);
            truncated[sizeof(truncated) - 1] = '\0';

            if (memory_filter_check(truncated)) {
               skipped_blocked++;
               continue;
            }

            if (is_fact_duplicate(conn->auth_user_id, truncated)) {
               skipped_dupes++;
               continue;
            }

            /* Add to preview */
            json_object *preview_item = json_object_new_object();
            json_object_object_add(preview_item, "type", json_object_new_string("fact"));
            json_object_object_add(preview_item, "text", json_object_new_string(truncated));
            json_object_object_add(preview_item, "confidence", json_object_new_double(confidence));
            json_object_array_add(preview_arr, preview_item);

            if (commit) {
               memory_db_fact_create(conn->auth_user_id, truncated, confidence, "import", NULL);
            }
            imported_facts++;
         }
      }

      /* Import preferences */
      json_object *prefs_arr;
      if (json_object_object_get_ex(data_obj, "preferences", &prefs_arr)) {
         int len = json_object_array_length(prefs_arr);
         for (int i = 0; i < len && i < IMPORT_MAX_LINES; i++) {
            json_object *p = json_object_array_get_idx(prefs_arr, i);
            json_object *cat_obj, *val_obj, *conf_obj;

            if (!json_object_object_get_ex(p, "category", &cat_obj) ||
                !json_object_object_get_ex(p, "value", &val_obj))
               continue;

            const char *category = json_object_get_string(cat_obj);
            const char *value = json_object_get_string(val_obj);
            if (!category || !value || strlen(category) == 0 || strlen(value) == 0) {
               skipped_empty++;
               continue;
            }

            float confidence = 0.8f;
            if (json_object_object_get_ex(p, "confidence", &conf_obj))
               confidence = (float)json_object_get_double(conf_obj);

            if (memory_filter_check(value) || memory_filter_check(category)) {
               skipped_blocked++;
               continue;
            }

            json_object *preview_item = json_object_new_object();
            json_object_object_add(preview_item, "type", json_object_new_string("preference"));
            json_object_object_add(preview_item, "category", json_object_new_string(category));
            json_object_object_add(preview_item, "value", json_object_new_string(value));
            json_object_array_add(preview_arr, preview_item);

            if (commit) {
               memory_db_pref_upsert(conn->auth_user_id, category, value, confidence, "import");
            }
            imported_prefs++;
         }
      }

   } else {
      /* --- Plain text import (from Claude/ChatGPT) --- */
      json_object *text_obj;
      if (!json_object_object_get_ex(payload, "text", &text_obj)) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error", json_object_new_string("Missing text"));
         json_object_put(preview_arr);
         json_object_object_add(response, "payload", resp_payload);
         send_json_response(conn, response);
         json_object_put(response);
         return;
      }

      const char *text = json_object_get_string(text_obj);
      if (!text || strlen(text) == 0) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error", json_object_new_string("Empty text"));
         json_object_put(preview_arr);
         json_object_object_add(response, "payload", resp_payload);
         send_json_response(conn, response);
         json_object_put(response);
         return;
      }

      if (strlen(text) > IMPORT_MAX_TEXT_LEN) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Text too large (256KB max)"));
         json_object_put(preview_arr);
         json_object_object_add(response, "payload", resp_payload);
         send_json_response(conn, response);
         json_object_put(response);
         return;
      }

      /* Parse line by line: strip "- " bullet prefix, skip headers/blank lines */
      char *text_copy = strdup(text);
      if (!text_copy) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Memory allocation failed"));
         json_object_put(preview_arr);
         json_object_object_add(response, "payload", resp_payload);
         send_json_response(conn, response);
         json_object_put(response);
         return;
      }

      int line_count = 0;
      char *saveptr = NULL;
      char *line = strtok_r(text_copy, "\n", &saveptr);

      while (line && line_count < IMPORT_MAX_LINES) {
         line_count++;

         /* Skip leading whitespace */
         while (*line == ' ' || *line == '\t')
            line++;

         /* Skip headers (lines starting with #) */
         if (*line == '#' || *line == '\0') {
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
         }

         /* Strip bullet prefix "- " or "* " */
         const char *fact_text = line;
         if ((line[0] == '-' || line[0] == '*') && line[1] == ' ')
            fact_text = line + 2;

         /* Skip if too short (likely noise) */
         if (strlen(fact_text) < 3) {
            skipped_empty++;
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
         }

         /* Truncate to max fact length */
         char truncated[MEMORY_FACT_TEXT_MAX];
         strncpy(truncated, fact_text, sizeof(truncated) - 1);
         truncated[sizeof(truncated) - 1] = '\0';

         if (memory_filter_check(truncated)) {
            skipped_blocked++;
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
         }

         if (is_fact_duplicate(conn->auth_user_id, truncated)) {
            skipped_dupes++;
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
         }

         json_object *preview_item = json_object_new_object();
         json_object_object_add(preview_item, "type", json_object_new_string("fact"));
         json_object_object_add(preview_item, "text", json_object_new_string(truncated));
         json_object_object_add(preview_item, "confidence", json_object_new_double(0.7));
         json_object_array_add(preview_arr, preview_item);

         if (commit) {
            memory_db_fact_create(conn->auth_user_id, truncated, 0.7f, "import", NULL);
         }
         imported_facts++;

         line = strtok_r(NULL, "\n", &saveptr);
      }

      free(text_copy);
   }

   json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
   json_object_object_add(resp_payload, "committed", json_object_new_boolean(commit));
   json_object_object_add(resp_payload, "imported_facts", json_object_new_int(imported_facts));
   json_object_object_add(resp_payload, "imported_prefs", json_object_new_int(imported_prefs));
   json_object_object_add(resp_payload, "skipped_dupes", json_object_new_int(skipped_dupes));
   json_object_object_add(resp_payload, "skipped_empty", json_object_new_int(skipped_empty));
   json_object_object_add(resp_payload, "skipped_blocked", json_object_new_int(skipped_blocked));
   if (!commit) {
      json_object_object_add(resp_payload, "preview", preview_arr);
   } else {
      json_object_put(preview_arr);
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);

   if (commit) {
      OLOG_INFO(
          "WebUI: User %d imported memories (format=%s, facts=%d, prefs=%d, dupes=%d, blocked=%d)",
          conn->auth_user_id, format, imported_facts, imported_prefs, skipped_dupes,
          skipped_blocked);
   }
}
