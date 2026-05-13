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
 * Tavily /extract adapter implementation.
 */

#include "tools/url_fetch_tavily.h"

#include <curl/curl.h>
#include <json-c/json.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "logging.h"
#include "tools/curl_buffer.h"
#include "tools/url_fetcher.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

#define TAVILY_EXTRACT_ENDPOINT "https://api.tavily.com/extract"
#define TAVILY_EXTRACT_CONNECT_TIMEOUT_SEC 5
#define TAVILY_EXTRACT_MAX_REDIRS 3L

/* Response buffer cap — Tavily can return large raw_content blobs; allow
 * 2 MB inbound so the post-extraction size check has headroom. The output is
 * still trimmed to the configured per-fetch max_response_bytes. */
#define TAVILY_EXTRACT_RESPONSE_MAX_BYTES (2 * 1024 * 1024)

#define TAVILY_RAW_CONTENT_DEFAULT_MAX (1 * 1024 * 1024) /* matches dawn_config.h default */

/* =============================================================================
 * Helpers
 * ============================================================================= */

static int tavily_extract_timeout_sec(void) {
   int t = g_config.url_fetcher.tavily.timeout_sec;
   return (t > 0) ? t : 30;
}

static size_t tavily_raw_content_max(void) {
   size_t m = g_config.url_fetcher.tavily.max_response_bytes;
   return m ? m : TAVILY_RAW_CONTENT_DEFAULT_MAX;
}

/**
 * @brief Build the Tavily /extract JSON request body.
 *
 * Body shape: {"api_key": "...", "urls": ["<url>"], "extract_depth": "basic"}
 * Caller frees the heap buffer AND must memset(0) it before free to clear
 * the embedded API key. Length returned via *out_len so caller can memset.
 */
static char *build_extract_body(const char *api_key, const char *url, size_t *out_len) {
   if (!api_key || !url || !out_len) {
      return NULL;
   }
   struct json_object *root = json_object_new_object();
   if (!root) {
      return NULL;
   }
   struct json_object *urls_arr = json_object_new_array();
   if (!urls_arr) {
      json_object_put(root);
      return NULL;
   }
   json_object_array_add(urls_arr, json_object_new_string(url));

   /* Read depth once for the request; empty config defaults to "advanced"
    * since heavy-chrome sites (TipRanks etc.) dominate the first 8 KB
    * under "basic" and the article body never reaches the LLM. */
   const char *depth = g_config.url_fetcher.tavily.extract_depth;
   if (!depth || !depth[0]) {
      depth = "advanced";
   }

   json_object_object_add(root, "api_key", json_object_new_string(api_key));
   json_object_object_add(root, "urls", urls_arr);
   json_object_object_add(root, "extract_depth", json_object_new_string(depth));

   size_t serialized_len = 0;
   const char *serialized = json_object_to_json_string_length(root, JSON_C_TO_STRING_PLAIN,
                                                              &serialized_len);
   if (!serialized) {
      json_object_put(root);
      return NULL;
   }
   char *body = malloc(serialized_len + 1);
   if (!body) {
      json_object_put(root);
      return NULL;
   }
   memcpy(body, serialized, serialized_len);
   body[serialized_len] = '\0';
   *out_len = serialized_len;

   json_object_put(root);
   return body;
}

/**
 * @brief Parse Tavily /extract response. Populates *out_content with the
 *        first results[].raw_content (or NULL on failure). Logs first
 *        failed_results[].error at WARNING; never propagates Tavily's error
 *        string verbatim to the LLM (SEC-L3).
 */
static int parse_extract_response(const char *json_text, char **out_content, size_t *out_size) {
   if (!out_content) {
      return URL_FETCH_ERROR_INVALID_URL;
   }
   *out_content = NULL;
   if (out_size) {
      *out_size = 0;
   }

   struct json_object *root = json_tokener_parse(json_text);
   if (!root) {
      OLOG_WARNING("tavily_extract: failed to parse JSON response");
      return URL_FETCH_ERROR_NETWORK;
   }

   struct json_object *results_arr = NULL;
   struct json_object *failed_arr = NULL;
   json_object_object_get_ex(root, "results", &results_arr);
   json_object_object_get_ex(root, "failed_results", &failed_arr);

   int has_results = (results_arr && json_object_is_type(results_arr, json_type_array) &&
                      json_object_array_length(results_arr) > 0);
   int has_failed = (failed_arr && json_object_is_type(failed_arr, json_type_array) &&
                     json_object_array_length(failed_arr) > 0);

   if (!has_results) {
      /* SEC-L3: log failed_results[0].error for diagnostics — never returned
       * to the LLM (it could itself be an injection vector). The error
       * string is attacker-influenceable (user-supplied URL flows into
       * Tavily's error message); sanitize CR/LF + cap length before
       * logging to prevent syslog/log-file injection. */
      if (has_failed) {
         struct json_object *first = json_object_array_get_idx(failed_arr, 0);
         struct json_object *jerr = NULL;
         const char *err_s = NULL;
         if (first && json_object_object_get_ex(first, "error", &jerr) && jerr) {
            err_s = json_object_get_string(jerr);
         }
         char sanitized[256];
         size_t out = 0;
         if (err_s) {
            for (size_t i = 0; err_s[i] && out + 1 < sizeof(sanitized); i++) {
               unsigned char c = (unsigned char)err_s[i];
               sanitized[out++] = (c < 0x20 || c == 0x7f) ? ' ' : (char)c;
            }
         }
         sanitized[out] = '\0';
         OLOG_WARNING("tavily_extract: upstream refused (first error: %s)",
                      out ? sanitized : "(empty)");
      } else {
         OLOG_WARNING("tavily_extract: empty results and no failure detail");
      }
      json_object_put(root);
      return URL_FETCH_ERROR_NETWORK;
   }

   struct json_object *first = json_object_array_get_idx(results_arr, 0);
   if (!first || !json_object_is_type(first, json_type_object)) {
      json_object_put(root);
      return URL_FETCH_ERROR_EMPTY;
   }
   struct json_object *jraw = NULL;
   if (!json_object_object_get_ex(first, "raw_content", &jraw) || !jraw) {
      json_object_put(root);
      return URL_FETCH_ERROR_EMPTY;
   }
   const char *raw_s = json_object_get_string(jraw);
   if (!raw_s || !raw_s[0]) {
      json_object_put(root);
      return URL_FETCH_ERROR_EMPTY;
   }

   /* SEC-M2: enforce hard inbound size cap (configurable). */
   size_t raw_len = strlen(raw_s);
   size_t max_bytes = tavily_raw_content_max();
   int truncated = 0;
   if (raw_len > max_bytes) {
      OLOG_WARNING("tavily_extract: raw_content %zu bytes exceeds cap %zu — truncating", raw_len,
                   max_bytes);
      raw_len = max_bytes;
      truncated = 1;
   }

   char *content = malloc(raw_len + 1);
   if (!content) {
      json_object_put(root);
      return URL_FETCH_ERROR_ALLOC;
   }
   memcpy(content, raw_s, raw_len);
   content[raw_len] = '\0';

   json_object_put(root); /* releases the Tavily raw_content string */

   /* Note on prompt-injection filtering: an earlier revision ran
    * raw_content through memory_filter_check + [DATA] wrap. That filter's
    * blocklist was authored for short factual text destined for persistent
    * memory storage; it false-positives on article-length natural language
    * (markdown link tails "](http", everyday phrases "you should", etc.).
    * The wrap also fed into the downstream summarizer which preserved the
    * markers but stripped article body in favor of legal/chrome chunks.
    *
    * Tavily's /extract is a commercial article extractor — its output is
    * already curated body text. We pass it through unmodified except for
    * the size cap; the summarizer is also bypassed via the sentinel below.
    * Memory-ingestion paths (memory_db_fact_insert etc.) still gate on
    * memory_filter_check at their storage sites. */
   (void)0;

   if (truncated) {
      /* Append a visible notice that the content was truncated, similar to
       * what url_fetcher.c does for direct fetches. */
      static const char NOTICE[] =
          "\n\n---\n**Note: This page was truncated due to size limits.**\n";
      size_t notice_len = sizeof(NOTICE) - 1;
      char *grown = realloc(content, raw_len + notice_len + 1);
      if (grown) {
         memcpy(grown + raw_len, NOTICE, notice_len + 1);
         content = grown;
         raw_len += notice_len;
      }
   }

   *out_content = content;
   if (out_size) {
      *out_size = raw_len;
   }
   return URL_FETCH_SUCCESS;
}

/* =============================================================================
 * Public API
 * ============================================================================= */

int url_fetch_tavily_is_configured(void) {
   if (strcmp(g_config.url_fetcher.fallback, "tavily") != 0) {
      return 0;
   }
   if (g_secrets.tavily_api_key[0] == '\0') {
      return 0;
   }
   return 1;
}

int url_fetch_tavily(const char *url, char **out_content, size_t *out_size) {
   if (!url || !out_content) {
      return URL_FETCH_ERROR_INVALID_URL;
   }
   *out_content = NULL;
   if (out_size) {
      *out_size = 0;
   }

   /* SEC-H1: SSRF pre-validation. DAWN does not delegate internal-IP /
    * RFC1918 / cloud-metadata decisions to upstream — same allowlist that
    * gates direct libcurl fetch applies before Tavily handoff. */
   if (!url_is_valid(url)) {
      OLOG_WARNING("tavily_extract: invalid URL rejected: %s", url);
      return URL_FETCH_ERROR_INVALID_URL;
   }
   if (url_is_blocked(url)) {
      OLOG_WARNING("tavily_extract: URL blocked by SSRF allowlist (not forwarded to Tavily): %s",
                   url);
      return URL_FETCH_ERROR_BLOCKED_URL;
   }

   const char *api_key = g_secrets.tavily_api_key;
   if (api_key[0] == '\0') {
      OLOG_WARNING("tavily_extract: no API key set");
      return URL_FETCH_ERROR_NETWORK;
   }

   size_t body_len = 0;
   char *body = build_extract_body(api_key, url, &body_len);
   if (!body) {
      return URL_FETCH_ERROR_ALLOC;
   }

   CURL *curl = curl_easy_init();
   if (!curl) {
      /* SEC-L1: zero secret-containing buffer before free.
       * sodium_memzero survives dead-store elimination. */
      sodium_memzero(body, body_len);
      free(body);
      return URL_FETCH_ERROR_ALLOC;
   }

   struct curl_slist *headers = NULL;
   headers = curl_slist_append(headers, "Content-Type: application/json");
   headers = curl_slist_append(headers, "Accept: application/json");

   curl_buffer_t buf;
   curl_buffer_init_with_max(&buf, TAVILY_EXTRACT_RESPONSE_MAX_BYTES);
   /* EMB-M3: /extract responses are typically large (article text). Skip the
    * 4 KB → 8 KB → ... → 2 MB growth ladder by preallocating 64 KB up front,
    * cutting cumulative realloc-memcpy work by ~75% for full-page extracts. */
   buf.data = malloc(64 * 1024);
   if (buf.data) {
      buf.data[0] = '\0';
      buf.capacity = 64 * 1024;
   }

   int timeout = tavily_extract_timeout_sec();
   curl_easy_setopt(curl, CURLOPT_URL, TAVILY_EXTRACT_ENDPOINT);
   curl_easy_setopt(curl, CURLOPT_POST, 1L);
   curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
   curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
   curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_buffer_write_callback);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
   curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout);
   curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (long)TAVILY_EXTRACT_CONNECT_TIMEOUT_SEC);
   curl_easy_setopt(curl, CURLOPT_USERAGENT, "DAWN/1.0");
   /* SEC-M1: explicit TLS + protocol pinning. */
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
   DAWN_CURL_SET_PROTOCOLS(curl, "https");
#if LIBCURL_VERSION_NUM >= 0x075500
   curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
   curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, (long)CURLPROTO_HTTPS);
#endif
   curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
   curl_easy_setopt(curl, CURLOPT_MAXREDIRS, TAVILY_EXTRACT_MAX_REDIRS);

   OLOG_INFO("tavily_extract: fetching %s via Tavily", url);

   CURLcode res = curl_easy_perform(curl);
   long http_code = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

   curl_slist_free_all(headers);
   curl_easy_cleanup(curl);

   /* SEC-L1: zero secret-containing buffer before free.
    * sodium_memzero survives dead-store elimination. */
   sodium_memzero(body, body_len);
   free(body);

   if (res != CURLE_OK) {
      OLOG_WARNING("tavily_extract: request failed: %s", curl_easy_strerror(res));
      curl_buffer_free(&buf);
      return URL_FETCH_ERROR_NETWORK;
   }

   if (http_code == 401) {
      OLOG_WARNING("tavily_extract: HTTP 401 — invalid API key");
      curl_buffer_free(&buf);
      return URL_FETCH_ERROR_HTTP;
   }
   if (http_code == 429) {
      OLOG_WARNING("tavily_extract: HTTP 429 — rate limited by Tavily");
      curl_buffer_free(&buf);
      return URL_FETCH_ERROR_HTTP;
   }
   if (http_code < 200 || http_code >= 300) {
      OLOG_WARNING("tavily_extract: HTTP error %ld", http_code);
      curl_buffer_free(&buf);
      return URL_FETCH_ERROR_HTTP;
   }

   if (buf.truncated) {
      OLOG_WARNING("tavily_extract: response truncated at %zu bytes (max %d)", buf.size,
                   TAVILY_EXTRACT_RESPONSE_MAX_BYTES);
   }

   if (!buf.data || buf.size == 0) {
      OLOG_WARNING("tavily_extract: empty response");
      curl_buffer_free(&buf);
      return URL_FETCH_ERROR_EMPTY;
   }

   int result = parse_extract_response(buf.data, out_content, out_size);
   curl_buffer_free(&buf);

   if (result == URL_FETCH_SUCCESS && out_size) {
      OLOG_INFO("tavily_extract: success — %zu bytes of markdown for %s", *out_size, url);
   }
   return result;
}
