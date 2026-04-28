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
 * WebUI Document Upload Handlers
 *
 * Handles HTTP endpoint for plain text document upload:
 *   POST /api/documents - Upload a text document, returns extracted content
 *
 * Phase 1: Plain text files (.txt, .md, .csv, .json, source code)
 * Phase 2: PDF extraction (MuPDF), DOCX extraction (libzip + libxml2)
 * Phase 3: HTML-to-markdown (html_parser.c), token budget + auto-summarize
 * Text is extracted server-side and returned in the JSON response.
 * No persistent storage — client holds content in JS state until sent.
 *
 * Authentication required.
 */

#ifndef WEBUI_DOCUMENTS_H
#define WEBUI_DOCUMENTS_H

#include <libwebsockets.h>

/* Forward declaration */
typedef struct document_upload_session document_upload_session_t;

/* Document upload limits are now configurable via documents_config_t (dawn_config.h).
 * Use g_config.documents.max_file_size_kb * 1024 etc. at runtime.
 * Text extraction moved to tools/document_extract.h (shared with document_index tool). */

/* =============================================================================
 * HTTP Handlers
 * ============================================================================= */

/**
 * @brief Handle document upload request start (POST /api/documents)
 *
 * Validates Content-Type, extracts multipart boundary, allocates session.
 * Returns 0 to continue to body callbacks, LWS_CLOSE_CONNECTION on error.
 *
 * @param wsi WebSocket/HTTP connection
 * @param session_out Output: allocated document session (caller must free)
 * @return 0 on success, LWS_CLOSE_CONNECTION on error
 */
int webui_documents_handle_upload_start(struct lws *wsi, document_upload_session_t **session_out);

/**
 * @brief Handle document upload body data
 *
 * Accumulates uploaded data in session buffer.
 *
 * @param wsi WebSocket/HTTP connection
 * @param session Document session
 * @param data Incoming data chunk
 * @param len Length of data chunk
 * @return 0 on success, LWS_CLOSE_CONNECTION on error
 */
int webui_documents_handle_upload_body(struct lws *wsi,
                                       document_upload_session_t *session,
                                       const char *data,
                                       size_t len);

/**
 * @brief Handle document upload completion
 *
 * Parses multipart data, extracts text, sends JSON response with content.
 * Frees session on completion.
 *
 * @param wsi WebSocket/HTTP connection
 * @param session Document session (will be freed)
 * @return LWS_CLOSE_CONNECTION to close connection (response sent)
 */
int webui_documents_handle_upload_complete(struct lws *wsi, document_upload_session_t *session);

/**
 * @brief Handle POST /api/documents/summarize
 *
 * TF-IDF summarizes document text to fit within a token budget.
 * Expects JSON body: {"content": "...", "target_tokens": N}
 *
 * @param wsi WebSocket/HTTP connection
 * @param body POST body (JSON)
 * @param body_len Length of POST body
 * @return LWS_CLOSE_CONNECTION to close connection (response sent)
 */
int webui_documents_handle_summarize(struct lws *wsi, const char *body, size_t body_len);

/**
 * @brief Free document session resources
 *
 * Safe to call with NULL.
 *
 * @param session Document session to free
 */
void webui_documents_session_free(document_upload_session_t *session);

#endif /* WEBUI_DOCUMENTS_H */
