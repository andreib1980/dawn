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
 * WebUI Image Handlers
 *
 * Handles HTTP endpoints for image upload/download:
 *   POST /api/images    - Upload an image
 *   GET  /api/images/:id - Download an image
 *
 * Authentication required for all endpoints.
 */

#ifndef WEBUI_IMAGES_H
#define WEBUI_IMAGES_H

#include <libwebsockets.h>
#include <stdbool.h>

/* Forward declaration */
struct http_image_session;

/* =============================================================================
 * Image Session Data
 *
 * Extended session data for handling image uploads (larger than normal POST).
 * Allocated separately due to size (up to 4MB for image data).
 * ============================================================================= */

/* Image upload max size is now configurable via vision.max_image_size_kb in dawn_config.h.
 * Use g_config.vision.max_image_size_kb * 1024 at runtime. */

typedef struct http_image_session {
   int user_id;           /* Authenticated user */
   char path[256];        /* Request path */
   char mime_type[64];    /* Content-Type from header */
   char boundary[128];    /* Multipart boundary */
   unsigned char *data;   /* Image data buffer */
   size_t data_len;       /* Current data length */
   size_t data_cap;       /* Allocated capacity */
   bool header_parsed;    /* Multipart header parsed */
   bool is_multipart;     /* Is multipart/form-data */
   size_t content_length; /* Expected total length */
   size_t max_image_size; /* Config snapshot at upload_start (TOCTOU prevention) */
} http_image_session_t;

/* =============================================================================
 * HTTP Handlers
 * ============================================================================= */

/**
 * @brief Handle image upload request start (POST /api/images)
 *
 * Validates authentication, allocates upload buffer.
 * Returns 0 to continue to body callbacks, LWS_CLOSE_CONNECTION on error.
 *
 * @param wsi WebSocket/HTTP connection
 * @param session Output: allocated image session (caller must free)
 * @param user_id Authenticated user ID
 * @return 0 on success, LWS_CLOSE_CONNECTION on error
 */
int webui_images_handle_upload_start(struct lws *wsi, http_image_session_t **session, int user_id);

/**
 * @brief Handle image upload body data
 *
 * Accumulates uploaded data in session buffer.
 *
 * @param wsi WebSocket/HTTP connection
 * @param session Image session
 * @param data Incoming data chunk
 * @param len Length of data chunk
 * @return 0 on success, LWS_CLOSE_CONNECTION on error
 */
int webui_images_handle_upload_body(struct lws *wsi,
                                    http_image_session_t *session,
                                    const void *data,
                                    size_t len);

/**
 * @brief Handle image upload completion
 *
 * Parses multipart data, saves image, sends response.
 * Frees session on completion.
 *
 * @param wsi WebSocket/HTTP connection
 * @param session Image session (will be freed)
 * @return LWS_CLOSE_CONNECTION to close connection (response sent)
 */
int webui_images_handle_upload_complete(struct lws *wsi, http_image_session_t *session);

/**
 * @brief Handle image download request (GET /api/images/:id)
 *
 * Validates authentication and access, sends image data.
 *
 * @param wsi WebSocket/HTTP connection
 * @param image_id Image ID from URL path
 * @param user_id Authenticated user ID (0 for admin bypass)
 * @return LWS_CLOSE_CONNECTION to close connection (response sent)
 */
int webui_images_handle_download(struct lws *wsi, const char *image_id, int user_id);

/**
 * @brief Free image session resources
 *
 * Safe to call with NULL.
 *
 * @param session Image session to free
 */
void webui_images_session_free(http_image_session_t *session);

#endif /* WEBUI_IMAGES_H */
