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
 * Document Read Tool - Paginated retrieval of full document contents
 *
 * Allows the LLM to read through a document page by page (in chunk ranges).
 * Complements document_search which finds relevant snippets across all docs.
 */

#include "tools/document_read.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/embedding_engine.h"
#include "dawn_error.h"
#include "logging.h"
#include "tools/document_db.h"
#include "tools/tool_registry.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

#define DOC_READ_DEFAULT_COUNT 10
#define DOC_READ_MAX_COUNT 20
#define DOC_READ_RESULT_BUF_SIZE (DOC_READ_MAX_COUNT * (DOC_CHUNK_TEXT_MAX + 64) + 512)

/* =============================================================================
 * Forward Declarations
 * ============================================================================= */

static char *doc_read_callback(const char *action, char *value, int *should_respond);
static bool doc_read_is_available(void);

/* =============================================================================
 * Tool Metadata
 * ============================================================================= */

static const treg_param_t doc_read_params[] = {
   {
       .name = "document",
       .description = "The document filename (or partial name) to read. "
                      "Use the name from document_search results.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = true,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
   {
       .name = "start_chunk",
       .description = "Starting chunk index (0-based). Default 0. "
                      "Use the value from the pagination hint in previous results.",
       .type = TOOL_PARAM_TYPE_INT,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "start_chunk",
   },
   {
       .name = "count",
       .description = "Number of chunks to retrieve (default 10, max 20).",
       .type = TOOL_PARAM_TYPE_INT,
       .required = false,
       .maps_to = TOOL_MAPS_TO_CUSTOM,
       .field_name = "count",
   },
};

static const tool_metadata_t doc_read_metadata = {
   .name = "document_read",
   .device_string = "document reader",
   .description = "Read the contents of a specific uploaded document, page by page. "
                  "Use this when you need to read or summarize an entire document rather than "
                  "searching for specific information. Returns chunks in order with pagination. "
                  "First call with just the document name, then use start_chunk to read more.",
   .params = doc_read_params,
   .param_count = 3,
   .device_type = TOOL_DEVICE_TYPE_GETTER,
   .capabilities = 0,
   .is_getter = true,
   .is_available = doc_read_is_available,
   .callback = doc_read_callback,
};

/* =============================================================================
 * Registration
 * ============================================================================= */

int document_read_tool_register(void) {
   return tool_registry_register(&doc_read_metadata);
}

/* =============================================================================
 * Availability Check
 * ============================================================================= */

static bool doc_read_is_available(void) {
   return embedding_engine_available();
}

/* =============================================================================
 * Read Callback
 * ============================================================================= */

static char *doc_read_callback(const char *action, char *value, int *should_respond) {
   (void)action;
   *should_respond = 1;

   if (!value || value[0] == '\0')
      return strdup("Error: no document name provided.");

   /* Extract custom parameters */
   int start_chunk = 0;
   int count = DOC_READ_DEFAULT_COUNT;
   char tmp[32];

   if (tool_param_extract_custom(value, "start_chunk", tmp, sizeof(tmp)))
      start_chunk = atoi(tmp);
   if (tool_param_extract_custom(value, "count", tmp, sizeof(tmp))) {
      count = atoi(tmp);
      if (count <= 0)
         count = DOC_READ_DEFAULT_COUNT;
      if (count > DOC_READ_MAX_COUNT)
         count = DOC_READ_MAX_COUNT;
   }

   /* Strip custom params from value to get the document name */
   char *sep = strstr(value, "::");
   char doc_name[DOC_FILENAME_MAX];
   if (sep) {
      size_t len = (size_t)(sep - value);
      if (len >= sizeof(doc_name))
         len = sizeof(doc_name) - 1;
      memcpy(doc_name, value, len);
      doc_name[len] = '\0';
   } else {
      snprintf(doc_name, sizeof(doc_name), "%s", value);
   }

   /* Trim trailing whitespace from document name */
   size_t name_len = strlen(doc_name);
   while (name_len > 0 && (doc_name[name_len - 1] == ' ' || doc_name[name_len - 1] == '\t')) {
      doc_name[--name_len] = '\0';
   }

   if (doc_name[0] == '\0')
      return strdup("Error: no document name provided.");

   int user_id = tool_get_current_user_id();

   /* Find the document */
   document_t doc;
   if (document_db_find_by_name(user_id, doc_name, &doc) != 0) {
      char err_buf[DOC_FILENAME_MAX + 128];
      snprintf(err_buf, sizeof(err_buf),
               "Error: no document matching '%s' found. "
               "Use document_search to find available documents first.",
               doc_name);
      return strdup(err_buf);
   }

   /* Validate start_chunk */
   if (start_chunk < 0)
      start_chunk = 0;
   if (start_chunk >= doc.num_chunks) {
      char err_buf[DOC_FILENAME_MAX + 128];
      snprintf(err_buf, sizeof(err_buf),
               "Error: start_chunk %d is beyond end of document '%s' (%d chunks total).",
               start_chunk, doc.filename, doc.num_chunks);
      return strdup(err_buf);
   }

   /* Read chunks */
   document_chunk_t *chunks = calloc((size_t)count, sizeof(document_chunk_t));
   if (!chunks)
      return strdup("Error: memory allocation failed.");

   int read_count = 0;
   if (document_db_chunk_read(doc.id, chunks, count, start_chunk, &read_count) != SUCCESS ||
       read_count <= 0) {
      free(chunks);
      return strdup("Error: failed to read document chunks.");
   }

   /* Build result string */
   char *result = malloc(DOC_READ_RESULT_BUF_SIZE);
   if (!result) {
      free(chunks);
      return strdup("Error: memory allocation failed.");
   }

   int end_chunk = start_chunk + read_count - 1;
   int pos = snprintf(result, DOC_READ_RESULT_BUF_SIZE, "DOCUMENT: %s (chunks %d-%d of %d)\n",
                      doc.filename, start_chunk, end_chunk, doc.num_chunks);

   for (int i = 0; i < read_count && pos < DOC_READ_RESULT_BUF_SIZE - 256; i++) {
      pos += snprintf(result + pos, (size_t)(DOC_READ_RESULT_BUF_SIZE - pos),
                      "\n--- Chunk %d ---\n%s\n", chunks[i].chunk_index, chunks[i].text);
   }

   /* Pagination hint */
   int remaining = doc.num_chunks - (start_chunk + read_count);
   if (remaining > 0) {
      snprintf(result + pos, (size_t)(DOC_READ_RESULT_BUF_SIZE - pos),
               "\n[More content available: use document_read with start_chunk=%d to continue. "
               "%d chunk%s remaining.]",
               start_chunk + read_count, remaining, remaining == 1 ? "" : "s");
   }

   free(chunks);
   return result;
}
