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
 * LLM document-management tool (v61): the LLM-facing document ingestion +
 * management API.  Lets the assistant SAVE authored reference text as a note
 * (single-chunk, filed under a label, retrievable exactly) or as a general
 * (chunked) document, LIST what it has stored, and DELETE — with a mandatory
 * two-step user-approval flow for deletion (stage on 'delete', execute only on
 * 'confirm_delete'), mirroring the email trash confirmation pattern.
 *
 * Retrieval is NOT here: use document_search (hybrid) / document_read (verbatim
 * by label).  is_global is always false — the LLM cannot publish to all users.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/embedding_engine.h"
#include "core/strbuf.h"
#include "dawn_error.h"
#include "logging.h"
#include "tools/document_db.h"
#include "tools/document_index_pipeline.h"
#include "tools/document_manage.h"
#include "tools/toml.h"
#include "tools/tool_registry.h"

/* =============================================================================
 * Config — TOOL_CAP_DANGEROUS tools must supply a config struct + parser, and
 * the registry reads the first `bool enabled` field to honor an operator on/off
 * in the [document_manage] TOML section.  Defaults to ENABLED (the destructive
 * action — delete — is already gated behind a two-step user confirmation), so
 * the tool works out of the box; set `[document_manage] enabled = false` to
 * remove the LLM's document write/delete access.
 * ============================================================================= */

typedef struct {
   bool enabled; /**< MUST be the first field (dangerous-tool convention) */
} document_manage_config_t;

static document_manage_config_t s_config = { .enabled = true };

static void doc_manage_parse_config(toml_table_t *table, void *config) {
   document_manage_config_t *cfg = (document_manage_config_t *)config;
   if (!table)
      return; /* no [document_manage] section — keep the enabled-by-default */
   toml_datum_t enabled = toml_bool_in(table, "enabled");
   if (enabled.ok)
      cfg->enabled = enabled.u.b;
}

/* =============================================================================
 * Two-step delete approval — per-user staged pending deletion
 * ============================================================================= */

#define DOCMGMT_MAX_PENDING 8
#define DOCMGMT_PENDING_EXPIRY_SEC 120
#define DOCMGMT_CONFIRM_MSG_MAX 384 /* prompt text + up to DOC_FILENAME_MAX label */

typedef struct {
   int user_id;
   int64_t doc_id;
   char label[DOC_FILENAME_MAX];
   bool is_note;
   time_t created_at;
   bool active;
} docmgmt_pending_t;

static docmgmt_pending_t s_pending[DOCMGMT_MAX_PENDING];
static pthread_mutex_t s_pending_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Stage (or replace) the pending deletion for a user.  Caller-locked. */
static void stage_pending_locked(int user_id, int64_t doc_id, const char *label, bool is_note) {
   time_t now = time(NULL);
   int slot = -1;
   /* Reuse the user's existing slot first, else a free/expired one, else slot 0. */
   for (int i = 0; i < DOCMGMT_MAX_PENDING; i++) {
      if (s_pending[i].active && s_pending[i].user_id == user_id) {
         slot = i;
         break;
      }
   }
   if (slot < 0) {
      for (int i = 0; i < DOCMGMT_MAX_PENDING; i++) {
         if (!s_pending[i].active || now - s_pending[i].created_at > DOCMGMT_PENDING_EXPIRY_SEC) {
            slot = i;
            break;
         }
      }
   }
   if (slot < 0)
      slot = 0;
   s_pending[slot].user_id = user_id;
   s_pending[slot].doc_id = doc_id;
   snprintf(s_pending[slot].label, sizeof(s_pending[slot].label), "%s", label ? label : "");
   s_pending[slot].is_note = is_note;
   s_pending[slot].created_at = now;
   s_pending[slot].active = true;
}

/* Take (consume) a non-expired pending deletion for a user.  Returns true and
 * fills *out_id / *out_label / *out_is_note on success; clears the slot. */
static bool take_pending(int user_id,
                         int64_t *out_id,
                         char *out_label,
                         size_t label_sz,
                         bool *out_is_note) {
   bool found = false;
   time_t now = time(NULL);
   pthread_mutex_lock(&s_pending_mutex);
   for (int i = 0; i < DOCMGMT_MAX_PENDING; i++) {
      if (s_pending[i].active && s_pending[i].user_id == user_id) {
         if (now - s_pending[i].created_at <= DOCMGMT_PENDING_EXPIRY_SEC) {
            *out_id = s_pending[i].doc_id;
            snprintf(out_label, label_sz, "%s", s_pending[i].label);
            *out_is_note = s_pending[i].is_note;
            found = true;
         }
         memset(&s_pending[i], 0, sizeof(s_pending[i])); /* consume either way */
         break;
      }
   }
   pthread_mutex_unlock(&s_pending_mutex);
   return found;
}

/* =============================================================================
 * Tool metadata
 * ============================================================================= */

static char *doc_manage_callback(const char *action, char *value, int *should_respond);
static bool doc_manage_is_available(void);

static const treg_param_t doc_manage_params[] = {
   {
       .name = "action",
       .description =
           "Document management action: 'save_note' (file a short piece of authored reference "
           "text — a bio, pitch, address, canned answer — under a LABEL so it can be retrieved "
           "EXACTLY later; re-saving the same label OVERWRITES it), 'save_text' (store a longer "
           "piece of text as a normal searchable document under a title), 'list' (show the "
           "documents and notes the user has stored), 'delete' (request deletion of a note or "
           "document by its label — this does NOT delete immediately; it asks the user to "
           "confirm), 'confirm_delete' (carry out the deletion the user just approved).",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "save_note", "save_text", "list", "delete", "confirm_delete" },
       .enum_count = 5,
   },
   {
       .name = "label",
       .description = "For save_note: the label/name to file the text under (e.g. 'Public Bio'). "
                      "For save_text: the document title. For delete: the exact label/name of the "
                      "note or document to delete. Not used by list / confirm_delete.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
   {
       .name = "id",
       .description = "Optional numeric document id (from a 'list' result) — an alternative to "
                      "'label' when deleting. Only used by 'delete'.",
       .type = TOOL_PARAM_TYPE_INT,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "id",
   },
   {
       .name = "text",
       .description = "The text to store. Required by save_note and save_text; the full verbatim "
                      "content the user wants kept. Must be the LAST argument. Not used by other "
                      "actions.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "text",
   },
};

static const tool_metadata_t doc_manage_metadata = {
   .name = "document_manage",
   .device_string = "document manager",
   .description =
       "Save, list, and delete the user's stored documents and notes. Use 'save_note' to file "
       "exact reference text (a bio, an elevator pitch, an address, a saved answer) under a "
       "label the user can ask for later by name; 'save_text' for longer documents; 'list' to "
       "see what's stored; and 'delete' to remove something (which always asks the user to "
       "confirm first). To READ or SEARCH stored content, use document_read / document_search "
       "instead.",
   .params = doc_manage_params,
   .param_count = 5,
   .device_type = TOOL_DEVICE_TYPE_TRIGGER,
   .capabilities = TOOL_CAP_DANGEROUS, /* mutates + deletes user data */
   .is_getter = false,
   .default_local = true,
   .default_remote = true,
   .config = &s_config,
   .config_size = sizeof(s_config),
   .config_parser = doc_manage_parse_config,
   .config_section = "document_manage",
   .callback = doc_manage_callback,
   .is_available = doc_manage_is_available,
};

static bool doc_manage_is_available(void) {
   return embedding_engine_available();
}

/* =============================================================================
 * Action handlers
 * ============================================================================= */

static char *do_save_note(int user_id, const char *label, const char *text) {
   if (!label || !label[0] || !text || !text[0])
      return strdup("To save a note, provide both a label and the text.");

   /* Exact-label overwrite routing (M-5): re-saving a label edits in place.
    * Only treat it as an overwrite when the existing note is the CALLER'S own —
    * find_by_label_exact also matches global notes, and editing one of those
    * isn't ours to do (the DB gate would reject it anyway). */
   document_t existing;
   bool overwrite = (document_db_find_by_label_exact(user_id, label, true, &existing) == SUCCESS &&
                     existing.user_id == user_id);

   doc_index_result_t res;
   int rc;
   if (overwrite) {
      rc = document_note_update(user_id, existing.id, label, text, strlen(text), &res);
   } else {
      rc = document_index_note(user_id, label, text, strlen(text), false, &res);
   }
   if (rc != DOC_INDEX_SUCCESS) {
      char msg[256];
      snprintf(msg, sizeof(msg), "Couldn't save the note: %s", res.error_msg);
      return strdup(msg);
   }
   char msg[256];
   snprintf(msg, sizeof(msg), "%s note '%s'.", overwrite ? "Updated (overwrote existing)" : "Saved",
            label);
   return strdup(msg);
}

static char *do_save_text(int user_id, const char *title, const char *text) {
   if (!title || !title[0] || !text || !text[0])
      return strdup("To save a document, provide both a title and the text.");

   doc_index_result_t res;
   int rc = document_index_text(user_id, title, "text", text, strlen(text), false, &res);
   if (rc != DOC_INDEX_SUCCESS) {
      char msg[256];
      snprintf(msg, sizeof(msg), "Couldn't save the document: %s", res.error_msg);
      return strdup(msg);
   }
   char msg[256];
   snprintf(msg, sizeof(msg), "Saved document '%s' (%d chunk%s).", title, res.num_chunks,
            res.num_chunks == 1 ? "" : "s");
   return strdup(msg);
}

static char *do_list(int user_id) {
   document_t docs[DOC_MAX_RESULTS];
   int count = 0;
   if (document_db_list(user_id, docs, DOC_MAX_RESULTS, 0, &count) != SUCCESS)
      return strdup("Couldn't list your documents.");
   if (count == 0)
      return strdup("You have no saved documents or notes.");

   strbuf_t sb;
   strbuf_init(&sb, 1024);
   strbuf_appendf(&sb, "Stored documents and notes (%d):\n", count);
   for (int i = 0; i < count; i++) {
      bool is_note = (strcmp(docs[i].filetype, "note") == 0);
      strbuf_appendf(&sb, "- [id %lld] %s (%s)\n", (long long)docs[i].id, docs[i].filename,
                     is_note ? "note" : docs[i].filetype);
   }
   if (strbuf_oom(&sb)) {
      strbuf_free(&sb);
      return strdup("Your document list is too long to display in full.");
   }
   char *out = strbuf_steal(&sb);
   return out ? out : strdup("Couldn't list your documents.");
}

/* delete: resolve the target and STAGE it — never deletes here.  The user must
 * approve, after which the model calls confirm_delete. */
static char *do_delete_request(int user_id, const char *label, int64_t id) {
   document_t doc;
   bool found = false;
   if (id > 0) {
      found = (document_db_get(id, &doc) == SUCCESS && doc.user_id == user_id);
   } else if (label && label[0]) {
      /* Owner-scoped: find_by_label_exact also matches GLOBAL docs, so without
       * this ownership check a user could delete an admin-published global doc
       * by its label (IDOR — delete_indexed performs no ownership check). */
      found = (document_db_find_by_label_exact(user_id, label, false, &doc) == SUCCESS &&
               doc.user_id == user_id);
   } else {
      return strdup("To delete, give the exact label/name (or the id from 'list').");
   }
   if (!found)
      return strdup("No note or document by that name was found (it may not be yours).");

   bool is_note = (strcmp(doc.filetype, "note") == 0);
   pthread_mutex_lock(&s_pending_mutex);
   stage_pending_locked(user_id, doc.id, doc.filename, is_note);
   pthread_mutex_unlock(&s_pending_mutex);

   char msg[DOCMGMT_CONFIRM_MSG_MAX]; /* base copy + up to DOC_FILENAME_MAX label */
   snprintf(msg, sizeof(msg),
            "This will PERMANENTLY delete the %s '%s'. This cannot be undone. Ask the user to "
            "confirm, then call document_manage with action 'confirm_delete' to proceed.",
            is_note ? "note" : "document", doc.filename);
   return strdup(msg);
}

static char *do_confirm_delete(int user_id) {
   int64_t doc_id = 0;
   char label[DOC_FILENAME_MAX] = "";
   bool is_note = false;
   if (!take_pending(user_id, &doc_id, label, sizeof(label), &is_note))
      return strdup("There's nothing staged to delete (the request may have expired). Run "
                    "'delete' again first.");

   /* Re-validate ownership at confirm time: the staged doc could have been
    * deleted and its rowid reused by a DIFFERENT doc in the up-to-120s window
    * (documents.id is not AUTOINCREMENT).  Confirm we still own this exact id
    * before deleting (TOCTOU / CWE-367 guard). */
   document_t doc;
   if (document_db_get(doc_id, &doc) != SUCCESS || doc.user_id != user_id)
      return strdup("That item is no longer available to delete.");

   if (document_db_delete_indexed(doc_id) != SUCCESS)
      return strdup("The deletion failed — the item may have already been removed.");

   char msg[256];
   snprintf(msg, sizeof(msg), "Deleted the %s '%s'.", is_note ? "note" : "document", label);
   return strdup(msg);
}

/* =============================================================================
 * Callback + registration
 * ============================================================================= */

static char *doc_manage_callback(const char *action, char *value, int *should_respond) {
   (void)action;
   *should_respond = 1;
   int user_id = tool_get_current_user_id();

   char act[32] = "";
   if (action)
      snprintf(act, sizeof(act), "%s", action);

   if (strcmp(act, "list") == 0)
      return do_list(user_id);
   if (strcmp(act, "confirm_delete") == 0)
      return do_confirm_delete(user_id);

   /* The remaining actions read a label (base) and possibly id / text (custom). */
   char label[DOC_FILENAME_MAX] = "";
   tool_param_extract_base(value, label, sizeof(label));

   if (strcmp(act, "delete") == 0) {
      char id_str[24] = "";
      int64_t id = 0;
      if (tool_param_extract_custom(value, "id", id_str, sizeof(id_str)) && id_str[0])
         id = (int64_t)strtoll(id_str, NULL, 10);
      return do_delete_request(user_id, label, id);
   }

   /* save_note / save_text: text is the terminal (tail) param. */
   char *text = malloc(DOC_CHUNK_TEXT_MAX);
   if (!text)
      return strdup("Out of memory.");
   text[0] = '\0';
   tool_param_extract_custom_tail(value, "text", text, DOC_CHUNK_TEXT_MAX);

   char *result;
   if (strcmp(act, "save_note") == 0)
      result = do_save_note(user_id, label, text);
   else if (strcmp(act, "save_text") == 0)
      result = do_save_text(user_id, label, text);
   else
      result = strdup("Unknown document_manage action.");

   free(text);
   return result;
}

int document_manage_tool_register(void) {
   return tool_registry_register(&doc_manage_metadata);
}
