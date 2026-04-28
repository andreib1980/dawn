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
 * WebUI Document Library Handlers
 *
 * WebSocket handlers for the Document Library panel. Provides list, delete,
 * and index (chunk + embed + store) operations for RAG document search.
 */

#include "webui/webui_doc_library.h"

#include <json-c/json.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db.h"
#include "dawn_error.h"
#include "logging.h"
#include "tools/document_db.h"
#include "tools/document_index_pipeline.h"
#include "webui/webui_internal.h"

/* =============================================================================
 * Helper: quiet admin check (no error response sent)
 * ============================================================================= */

static bool conn_check_admin_quiet(ws_connection_t *conn) {
   if (!conn->authenticated)
      return false;
   auth_session_t session;
   if (auth_db_get_session(conn->auth_session_token, &session) != AUTH_DB_SUCCESS)
      return false;
   return session.is_admin;
}

/* =============================================================================
 * List Documents
 * ============================================================================= */

#define DOC_LIBRARY_DEFAULT_LIMIT 20
#define DOC_LIBRARY_MAX_LIMIT 100

void handle_doc_library_list(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("doc_library_list_response"));

   json_object *resp_payload = json_object_new_object();

   /* Parse pagination params */
   int limit = DOC_LIBRARY_DEFAULT_LIMIT;
   int offset = 0;
   bool show_all = false;
   if (payload) {
      json_object *limit_obj, *offset_obj, *show_all_obj;
      if (json_object_object_get_ex(payload, "limit", &limit_obj)) {
         limit = json_object_get_int(limit_obj);
         if (limit < 1 || limit > DOC_LIBRARY_MAX_LIMIT)
            limit = DOC_LIBRARY_DEFAULT_LIMIT;
      }
      if (json_object_object_get_ex(payload, "offset", &offset_obj)) {
         offset = json_object_get_int(offset_obj);
         if (offset < 0)
            offset = 0;
      }
      if (json_object_object_get_ex(payload, "show_all", &show_all_obj)) {
         show_all = json_object_get_boolean(show_all_obj);
      }
   }

   /* show_all requires admin — silently fall back to user-only if not admin */
   if (show_all && !conn_check_admin_quiet(conn))
      show_all = false;

   document_t docs[DOC_LIBRARY_DEFAULT_LIMIT]; /* Stack-friendly: sized to typical page */
   if (limit > DOC_LIBRARY_DEFAULT_LIMIT)
      limit = DOC_LIBRARY_DEFAULT_LIMIT;

   int count = 0;
   int list_rc = show_all ? document_db_list_all(docs, limit, offset, &count)
                          : document_db_list(conn->auth_user_id, docs, limit, offset, &count);

   if (list_rc != SUCCESS) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Failed to list documents"));
   } else {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(1));

      json_object *docs_array = json_object_new_array();
      for (int i = 0; i < count; i++) {
         json_object *doc = json_object_new_object();
         json_object_object_add(doc, "id", json_object_new_int64(docs[i].id));
         json_object_object_add(doc, "filename", json_object_new_string(docs[i].filename));
         json_object_object_add(doc, "filetype", json_object_new_string(docs[i].filetype));
         json_object_object_add(doc, "num_chunks", json_object_new_int(docs[i].num_chunks));
         json_object_object_add(doc, "is_global", json_object_new_boolean(docs[i].is_global));
         json_object_object_add(doc, "created_at", json_object_new_int64(docs[i].created_at));
         if (show_all) {
            json_object_object_add(doc, "user_id", json_object_new_int(docs[i].user_id));
            json_object_object_add(doc, "owner_name", json_object_new_string(docs[i].owner_name));
         }
         json_object_array_add(docs_array, doc);
      }
      json_object_object_add(resp_payload, "documents", docs_array);
      json_object_object_add(resp_payload, "count", json_object_new_int(count));
      json_object_object_add(resp_payload, "has_more", json_object_new_boolean(count == limit));
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Delete Document
 * ============================================================================= */

void handle_doc_library_delete(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("doc_library_delete_response"));

   json_object *resp_payload = json_object_new_object();

   json_object *id_obj;
   if (!json_object_object_get_ex(payload, "id", &id_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error", json_object_new_string("Missing document id"));
   } else {
      int64_t doc_id = json_object_get_int64(id_obj);

      /* Verify ownership: get the doc first */
      document_t doc;
      if (document_db_get(doc_id, &doc) != 0) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Document not found"));
      } else if (doc.user_id != conn->auth_user_id && !conn_check_admin_quiet(conn)) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error", json_object_new_string("Permission denied"));
      } else {
         int rc = document_db_delete(doc_id);
         json_object_object_add(resp_payload, "success", json_object_new_boolean(rc == 0));
         if (rc != 0) {
            json_object_object_add(resp_payload, "error", json_object_new_string("Delete failed"));
         } else {
            json_object_object_add(resp_payload, "id", json_object_new_int64(doc_id));
            if (doc.user_id != conn->auth_user_id) {
               OLOG_INFO("doc_library: admin user %d deleted document %lld (%s) owned by user %d",
                         conn->auth_user_id, (long long)doc_id, doc.filename, doc.user_id);
            } else {
               OLOG_INFO("doc_library: deleted document %lld (%s)", (long long)doc_id,
                         doc.filename);
            }
         }
      }
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Index Document (delegates to shared pipeline)
 * ============================================================================= */

void handle_doc_library_index(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("doc_library_index_response"));
   json_object *resp_payload = json_object_new_object();

   /* Extract fields */
   json_object *filename_obj, *filetype_obj, *text_obj, *global_obj;
   if (!json_object_object_get_ex(payload, "filename", &filename_obj) ||
       !json_object_object_get_ex(payload, "filetype", &filetype_obj) ||
       !json_object_object_get_ex(payload, "text", &text_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing required fields"));
      goto done;
   }

   const char *filename = json_object_get_string(filename_obj);
   const char *filetype = json_object_get_string(filetype_obj);
   const char *text = json_object_get_string(text_obj);
   bool is_global = json_object_object_get_ex(payload, "is_global", &global_obj) &&
                    json_object_get_boolean(global_obj);

   /* Only admins can mark documents as global */
   if (is_global && !conn_check_admin_quiet(conn))
      is_global = false;

   {
      size_t text_len = text ? strlen(text) : 0;
      doc_index_result_t result;
      int rc = document_index_text(conn->auth_user_id, filename, filetype, text, text_len,
                                   is_global, &result);

      if (rc != DOC_INDEX_SUCCESS) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string(result.error_msg[0]
                                                           ? result.error_msg
                                                           : document_index_error_string(rc)));
         if (rc == DOC_INDEX_ERROR_DUPLICATE && result.doc_id > 0) {
            /* Legacy: WebUI checks for existing_id on duplicate */
         }
      } else {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(1));
         json_object_object_add(resp_payload, "id", json_object_new_int64(result.doc_id));
         json_object_object_add(resp_payload, "filename", json_object_new_string(filename));
         json_object_object_add(resp_payload, "num_chunks", json_object_new_int(result.num_chunks));
         if (result.failed_chunks > 0) {
            json_object_object_add(resp_payload, "failed_chunks",
                                   json_object_new_int(result.failed_chunks));
         }
      }
   }

done:
   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}

/* =============================================================================
 * Toggle Global Visibility
 * ============================================================================= */

void handle_doc_library_toggle_global(ws_connection_t *conn, json_object *payload) {
   if (!conn_require_auth(conn))
      return;

   json_object *response = json_object_new_object();
   json_object_object_add(response, "type",
                          json_object_new_string("doc_library_toggle_global_response"));
   json_object *resp_payload = json_object_new_object();

   json_object *id_obj, *global_obj;
   if (!json_object_object_get_ex(payload, "id", &id_obj) ||
       !json_object_object_get_ex(payload, "is_global", &global_obj)) {
      json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
      json_object_object_add(resp_payload, "error",
                             json_object_new_string("Missing id or is_global"));
   } else {
      int64_t doc_id = json_object_get_int64(id_obj);
      bool new_global = json_object_get_boolean(global_obj);

      document_t doc;
      if (document_db_get(doc_id, &doc) != 0) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error",
                                json_object_new_string("Document not found"));
      } else if (doc.user_id != conn->auth_user_id && !conn_check_admin_quiet(conn)) {
         json_object_object_add(resp_payload, "success", json_object_new_boolean(0));
         json_object_object_add(resp_payload, "error", json_object_new_string("Permission denied"));
      } else {
         int rc = document_db_update_global(doc_id, new_global);
         json_object_object_add(resp_payload, "success", json_object_new_boolean(rc == 0));
         if (rc != 0) {
            json_object_object_add(resp_payload, "error", json_object_new_string("Update failed"));
         } else {
            json_object_object_add(resp_payload, "id", json_object_new_int64(doc_id));
            json_object_object_add(resp_payload, "is_global", json_object_new_boolean(new_global));
            OLOG_INFO("doc_library: user %d toggled document %lld (%s) global=%s",
                      conn->auth_user_id, (long long)doc_id, doc.filename,
                      new_global ? "true" : "false");
         }
      }
   }

   json_object_object_add(response, "payload", resp_payload);
   send_json_response(conn, response);
   json_object_put(response);
}
