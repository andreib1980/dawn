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
 * CalDAV protocol client — RFC 4791 calendar operations via libcurl + libxml2.
 * 3-step RFC-compliant discovery, REPORT queries, PUT/DELETE mutations.
 */

#ifdef DAWN_ENABLE_CALENDAR_TOOL

#include "tools/caldav_client.h"

#include <curl/curl.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/curl_buffer.h"
#include "logging.h"

#ifdef HAVE_LIBICAL
#include <libical/ical.h>
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

#define CALDAV_TIMEOUT_SEC 30
#define CALDAV_MAX_RESPONSE_SIZE (4 * 1024 * 1024) /* 4 MB */

/* XML namespace URIs */
#define NS_DAV "DAV:"
#define NS_CALDAV "urn:ietf:params:xml:ns:caldav"
#define NS_CS "http://calendarserver.org/ns/"
#define NS_APPLE "http://apple.com/ns/ical/"

/* ============================================================================
 * Internal: curl response buffer
 * ============================================================================ */

typedef struct {
   char *data;
   size_t size;
   size_t capacity;
} curl_buf_t;

/** Capture ETag from response headers */
typedef struct {
   char etag[128];
} etag_capture_t;

static size_t header_etag_cb(char *buf, size_t size, size_t nmemb, void *userdata) {
   size_t total = size * nmemb;
   etag_capture_t *cap = userdata;

   /* Look for "ETag:" header (case-insensitive) */
   if (total > 5 && strncasecmp(buf, "ETag:", 5) == 0) {
      const char *val = buf + 5;
      while (*val == ' ' || *val == '\t')
         val++;
      /* Strip surrounding quotes and trailing whitespace */
      size_t vlen = total - (size_t)(val - buf);
      while (vlen > 0 && (val[vlen - 1] == '\r' || val[vlen - 1] == '\n' || val[vlen - 1] == ' '))
         vlen--;
      if (vlen >= 2 && val[0] == '"' && val[vlen - 1] == '"') {
         val++;
         vlen -= 2;
      }
      if (vlen > 0 && vlen < sizeof(cap->etag))
         snprintf(cap->etag, sizeof(cap->etag), "%.*s", (int)vlen, val);
   }
   return total;
}

static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
   curl_buf_t *buf = userdata;
   size_t total = size * nmemb;

   if (buf->size + total > CALDAV_MAX_RESPONSE_SIZE) {
      return 0; /* abort */
   }

   if (buf->size + total >= buf->capacity) {
      size_t new_cap = buf->capacity * 2;
      if (new_cap < buf->size + total + 1)
         new_cap = buf->size + total + 1;
      char *tmp = realloc(buf->data, new_cap);
      if (!tmp)
         return 0;
      buf->data = tmp;
      buf->capacity = new_cap;
   }

   memcpy(buf->data + buf->size, ptr, total);
   buf->size += total;
   buf->data[buf->size] = '\0';
   return total;
}

static void curl_buf_init(curl_buf_t *buf) {
   buf->capacity = 4096;
   buf->data = malloc(buf->capacity);
   buf->size = 0;
   if (buf->data)
      buf->data[0] = '\0';
}

static void curl_buf_free(curl_buf_t *buf) {
   free(buf->data);
   buf->data = NULL;
   buf->size = 0;
   buf->capacity = 0;
}

/* ============================================================================
 * Internal: curl setup helpers
 * ============================================================================ */

static CURL *caldav_curl_init(const caldav_auth_t *auth, curl_buf_t *resp) {
   CURL *curl = curl_easy_init();
   if (!curl)
      return NULL;

   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
   curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)CALDAV_TIMEOUT_SEC);
   /* Disable redirects to prevent credential leakage to third-party hosts */
   curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
   /* SSRF protection: restrict to HTTPS only (CalDAV servers must use TLS) */
   DAWN_CURL_SET_PROTOCOLS(curl, "https");

   if (auth && auth->bearer_token) {
      curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BEARER);
      curl_easy_setopt(curl, CURLOPT_XOAUTH2_BEARER, auth->bearer_token);
   } else if (auth && auth->username && auth->password) {
      curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
      curl_easy_setopt(curl, CURLOPT_USERNAME, auth->username);
      curl_easy_setopt(curl, CURLOPT_PASSWORD, auth->password);
   }

   return curl;
}

static caldav_error_t http_status_to_error(long status) {
   if (status >= 200 && status <= 207)
      return CALDAV_OK;
   if (status == 401)
      return CALDAV_ERR_AUTH;
   if (status == 403)
      return CALDAV_ERR_FORBIDDEN;
   if (status == 404)
      return CALDAV_ERR_NOT_FOUND;
   if (status == 412)
      return CALDAV_ERR_CONFLICT;
   if (status >= 500)
      return CALDAV_ERR_SERVER;
   return CALDAV_ERR_NETWORK;
}

/* ============================================================================
 * Internal: PROPFIND helper
 * ============================================================================ */

static caldav_error_t do_propfind(CURL *curl,
                                  const char *url,
                                  const char *depth,
                                  const char *xml_body,
                                  curl_buf_t *resp) {
   resp->size = 0;
   if (resp->data)
      resp->data[0] = '\0';

   curl_easy_setopt(curl, CURLOPT_URL, url);
   curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PROPFIND");
   curl_easy_setopt(curl, CURLOPT_POSTFIELDS, xml_body);
   curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(xml_body));

   struct curl_slist *headers = NULL;
   headers = curl_slist_append(headers, "Content-Type: application/xml; charset=utf-8");
   char depth_hdr[32];
   snprintf(depth_hdr, sizeof(depth_hdr), "Depth: %s", depth);
   headers = curl_slist_append(headers, depth_hdr);
   curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

   CURLcode res = curl_easy_perform(curl);
   curl_slist_free_all(headers);

   if (res != CURLE_OK) {
      OLOG_ERROR("caldav: PROPFIND %s failed: %s", url, curl_easy_strerror(res));
      return CALDAV_ERR_NETWORK;
   }

   long status = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
   if (status >= 300) {
      OLOG_WARNING("caldav: PROPFIND %s Depth:%s -> HTTP %ld", url, depth, status);
      if (resp->data && resp->size > 0) {
         char snippet[201];
         size_t n = resp->size < 200 ? resp->size : 200;
         memcpy(snippet, resp->data, n);
         snippet[n] = '\0';
         OLOG_WARNING("caldav: response: %.200s", snippet);
      }
   }
   return http_status_to_error(status);
}

/* ============================================================================
 * Internal: URL resolution
 *
 * CalDAV servers return relative, absolute-path, or full URLs in href
 * responses. This handles all three cases.
 * ============================================================================ */

static int resolve_href(const char *base_url, const char *href, char *out, size_t out_len) {
   if (!href || !href[0]) {
      out[0] = '\0';
      return 1;
   }

   /* Case 1: Full URL (starts with http:// or https://) */
   if (strncmp(href, "http://", 7) == 0 || strncmp(href, "https://", 8) == 0) {
      snprintf(out, out_len, "%s", href);
      return 0;
   }

   /* Extract scheme + host from base_url */
   const char *scheme_end = strstr(base_url, "://");
   if (!scheme_end) {
      snprintf(out, out_len, "%s", href);
      return 1;
   }
   const char *host_start = scheme_end + 3;
   const char *host_end = strchr(host_start, '/');
   size_t base_prefix_len;
   if (host_end) {
      base_prefix_len = (size_t)(host_end - base_url);
   } else {
      base_prefix_len = strlen(base_url);
   }

   /* Case 2: Absolute path (starts with /) */
   if (href[0] == '/') {
      snprintf(out, out_len, "%.*s%s", (int)base_prefix_len, base_url, href);
      return 0;
   }

   /* Case 3: Relative path — append to base URL's directory */
   const char *last_slash = strrchr(base_url, '/');
   if (last_slash && last_slash > scheme_end + 2) {
      size_t dir_len = (size_t)(last_slash - base_url) + 1;
      snprintf(out, out_len, "%.*s%s", (int)dir_len, base_url, href);
   } else {
      snprintf(out, out_len, "%s/%s", base_url, href);
   }
   return 0;
}

/* ============================================================================
 * Internal: XML parsing helpers
 * ============================================================================ */

/** Register standard CalDAV namespaces for XPath */
static void register_namespaces(xmlXPathContextPtr ctx) {
   xmlXPathRegisterNs(ctx, (const xmlChar *)"d", (const xmlChar *)NS_DAV);
   xmlXPathRegisterNs(ctx, (const xmlChar *)"c", (const xmlChar *)NS_CALDAV);
   xmlXPathRegisterNs(ctx, (const xmlChar *)"cs", (const xmlChar *)NS_CS);
   xmlXPathRegisterNs(ctx, (const xmlChar *)"a", (const xmlChar *)NS_APPLE);
}

/** Extract text content from first XPath match, relative to a context node */
static int xpath_text(xmlXPathContextPtr ctx,
                      xmlNodePtr node,
                      const char *expr,
                      char *out,
                      size_t out_len) {
   xmlNodePtr saved = ctx->node;
   ctx->node = node;

   xmlXPathObjectPtr result = xmlXPathEvalExpression((const xmlChar *)expr, ctx);
   ctx->node = saved;

   if (!result)
      return 1;

   if (result->nodesetval && result->nodesetval->nodeNr > 0) {
      xmlNodePtr n = result->nodesetval->nodeTab[0];
      xmlChar *content = xmlNodeGetContent(n);
      if (content) {
         snprintf(out, out_len, "%s", (const char *)content);
         xmlFree(content);
         xmlXPathFreeObject(result);
         return 0;
      }
   }
   xmlXPathFreeObject(result);
   return 1;
}

/* ============================================================================
 * Discovery Step 1: Find current-user-principal (RFC 5397)
 * ============================================================================ */

static const char *PROPFIND_PRINCIPAL_XML = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                                            "<d:propfind xmlns:d=\"DAV:\">"
                                            "<d:prop>"
                                            "<d:current-user-principal/>"
                                            "<d:resourcetype/>"
                                            "</d:prop>"
                                            "</d:propfind>";

static caldav_error_t discover_principal(CURL *curl,
                                         const char *base_url,
                                         curl_buf_t *resp,
                                         char *principal_out,
                                         size_t principal_len) {
   principal_out[0] = '\0';

   caldav_error_t err = do_propfind(curl, base_url, "0", PROPFIND_PRINCIPAL_XML, resp);
   if (err != CALDAV_OK)
      return err;

   xmlDocPtr doc = xmlReadMemory(resp->data, (int)resp->size, NULL, NULL,
                                 XML_PARSE_NONET | XML_PARSE_NOBLANKS);
   if (!doc)
      return CALDAV_ERR_PARSE;

   xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
   register_namespaces(ctx);

   /* Look for current-user-principal/href */
   char href[512] = { 0 };
   xpath_text(ctx, xmlDocGetRootElement(doc), "//d:current-user-principal/d:href", href,
              sizeof(href));

   xmlXPathFreeContext(ctx);
   xmlFreeDoc(doc);

   if (href[0]) {
      resolve_href(base_url, href, principal_out, principal_len);
      OLOG_INFO("caldav: principal URL: %s", principal_out);
   }
   return CALDAV_OK;
}

/* ============================================================================
 * Discovery Step 2: Find calendar-home-set (RFC 4791 §6.2.1)
 * ============================================================================ */

static const char *PROPFIND_CALHOME_XML =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<d:propfind xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\">"
    "<d:prop>"
    "<c:calendar-home-set/>"
    "</d:prop>"
    "</d:propfind>";

static caldav_error_t discover_calendar_home(CURL *curl,
                                             const char *principal_url,
                                             curl_buf_t *resp,
                                             char *home_out,
                                             size_t home_len) {
   home_out[0] = '\0';

   caldav_error_t err = do_propfind(curl, principal_url, "0", PROPFIND_CALHOME_XML, resp);
   if (err != CALDAV_OK)
      return err;

   xmlDocPtr doc = xmlReadMemory(resp->data, (int)resp->size, NULL, NULL,
                                 XML_PARSE_NONET | XML_PARSE_NOBLANKS);
   if (!doc)
      return CALDAV_ERR_PARSE;

   xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
   register_namespaces(ctx);

   char href[512] = { 0 };
   xpath_text(ctx, xmlDocGetRootElement(doc), "//c:calendar-home-set/d:href", href, sizeof(href));

   xmlXPathFreeContext(ctx);
   xmlFreeDoc(doc);

   if (href[0]) {
      resolve_href(principal_url, href, home_out, home_len);
      OLOG_INFO("caldav: calendar-home-set: %s", home_out);
      return CALDAV_OK;
   }

   return CALDAV_ERR_NO_CALENDARS;
}

/* ============================================================================
 * Discovery Step 3: Enumerate calendar collections (PROPFIND Depth:1)
 * ============================================================================ */

static const char *PROPFIND_COLLECTIONS_XML =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<d:propfind xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\""
    " xmlns:cs=\"http://calendarserver.org/ns/\""
    " xmlns:a=\"http://apple.com/ns/ical/\">"
    "<d:prop>"
    "<d:resourcetype/>"
    "<d:displayname/>"
    "<cs:getctag/>"
    "<d:current-user-privilege-set/>"
    "<a:calendar-color/>"
    "</d:prop>"
    "</d:propfind>";

static caldav_error_t discover_collections(CURL *curl,
                                           const char *home_url,
                                           curl_buf_t *resp,
                                           caldav_calendar_info_t **out,
                                           int *out_count) {
   *out = NULL;
   *out_count = 0;

   caldav_error_t err = do_propfind(curl, home_url, "1", PROPFIND_COLLECTIONS_XML, resp);
   if (err != CALDAV_OK)
      return err;

   xmlDocPtr doc = xmlReadMemory(resp->data, (int)resp->size, NULL, NULL,
                                 XML_PARSE_NONET | XML_PARSE_NOBLANKS);
   if (!doc)
      return CALDAV_ERR_PARSE;

   xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
   register_namespaces(ctx);

   /* Find all response elements */
   xmlXPathObjectPtr responses = xmlXPathEvalExpression((const xmlChar *)"//d:response", ctx);
   if (!responses || !responses->nodesetval) {
      xmlXPathFreeContext(ctx);
      xmlFreeDoc(doc);
      return CALDAV_ERR_NO_CALENDARS;
   }

   int cap = 16;
   caldav_calendar_info_t *cals = calloc((size_t)cap, sizeof(caldav_calendar_info_t));
   if (!cals) {
      xmlXPathFreeObject(responses);
      xmlXPathFreeContext(ctx);
      xmlFreeDoc(doc);
      return CALDAV_ERR_ALLOC;
   }

   int count = 0;
   for (int i = 0; i < responses->nodesetval->nodeNr; i++) {
      xmlNodePtr resp_node = responses->nodesetval->nodeTab[i];

      /* Check if this resource has <cal:calendar/> in resourcetype */
      ctx->node = resp_node;
      xmlXPathObjectPtr rt = xmlXPathEvalExpression((const xmlChar *)".//d:resourcetype/c:calendar",
                                                    ctx);
      bool is_calendar = (rt && rt->nodesetval && rt->nodesetval->nodeNr > 0);
      xmlXPathFreeObject(rt);

      if (!is_calendar)
         continue;

      if (count >= cap) {
         cap *= 2;
         caldav_calendar_info_t *tmp = realloc(cals, (size_t)cap * sizeof(caldav_calendar_info_t));
         if (!tmp)
            break;
         cals = tmp;
      }

      caldav_calendar_info_t *cal = &cals[count];
      memset(cal, 0, sizeof(*cal));

      /* Extract href */
      char href[512] = { 0 };
      xpath_text(ctx, resp_node, "d:href", href, sizeof(href));
      resolve_href(home_url, href, cal->path, sizeof(cal->path));

      /* Display name */
      xpath_text(ctx, resp_node, ".//d:displayname", cal->display_name, sizeof(cal->display_name));

      /* CTag */
      xpath_text(ctx, resp_node, ".//cs:getctag", cal->ctag, sizeof(cal->ctag));

      /* Calendar color: try standard first, then Apple */
      if (xpath_text(ctx, resp_node, ".//a:calendar-color", cal->color, sizeof(cal->color)) != 0) {
         /* No Apple color, try x:calendar-color if needed */
      }

      /* Check read-only */
      xmlXPathObjectPtr write_priv = xmlXPathEvalExpression(
          (const xmlChar *)".//d:current-user-privilege-set/d:privilege/d:write", ctx);
      cal->read_only = !(write_priv && write_priv->nodesetval &&
                         write_priv->nodesetval->nodeNr > 0);
      xmlXPathFreeObject(write_priv);

      count++;
   }

   xmlXPathFreeObject(responses);
   xmlXPathFreeContext(ctx);
   xmlFreeDoc(doc);

   *out = cals;
   *out_count = count;
   return count > 0 ? CALDAV_OK : CALDAV_ERR_NO_CALENDARS;
}

/* ============================================================================
 * Public: caldav_discover — 3-step RFC-compliant discovery
 * ============================================================================ */

caldav_error_t caldav_discover(const char *base_url,
                               const caldav_auth_t *auth,
                               caldav_discovery_result_t *result) {
   memset(result, 0, sizeof(*result));

   curl_buf_t resp;
   curl_buf_init(&resp);
   if (!resp.data)
      return CALDAV_ERR_ALLOC;

   CURL *curl = caldav_curl_init(auth, &resp);
   if (!curl) {
      curl_buf_free(&resp);
      return CALDAV_ERR_NETWORK;
   }

   caldav_error_t err;

   /* Step 1: Find current-user-principal */
   char principal_url[512] = { 0 };
   err = discover_principal(curl, base_url, &resp, principal_url, sizeof(principal_url));

   if (err != CALDAV_OK || !principal_url[0]) {
      /* Fallback: try base URL directly as calendar-home (Radicale-style) */
      OLOG_INFO("caldav: no principal found, trying base URL as calendar-home");
      err = discover_collections(curl, base_url, &resp, &result->calendars,
                                 &result->calendar_count);
      if (err == CALDAV_OK) {
         snprintf(result->calendar_home_url, sizeof(result->calendar_home_url), "%s", base_url);
      }
      curl_easy_cleanup(curl);
      curl_buf_free(&resp);
      return err;
   }

   snprintf(result->principal_url, sizeof(result->principal_url), "%s", principal_url);

   /* Step 2: Find calendar-home-set */
   char home_url[512] = { 0 };
   err = discover_calendar_home(curl, principal_url, &resp, home_url, sizeof(home_url));
   if (err != CALDAV_OK) {
      curl_easy_cleanup(curl);
      curl_buf_free(&resp);
      return err;
   }

   snprintf(result->calendar_home_url, sizeof(result->calendar_home_url), "%s", home_url);

   /* Step 3: Enumerate calendar collections */
   err = discover_collections(curl, home_url, &resp, &result->calendars, &result->calendar_count);

   curl_easy_cleanup(curl);
   curl_buf_free(&resp);
   return err;
}

void caldav_discovery_free(caldav_discovery_result_t *result) {
   free(result->calendars);
   result->calendars = NULL;
   result->calendar_count = 0;
}

/* ============================================================================
 * Public: caldav_get_ctag
 * ============================================================================ */

static const char *PROPFIND_CTAG_XML =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<d:propfind xmlns:d=\"DAV:\" xmlns:cs=\"http://calendarserver.org/ns/\">"
    "<d:prop><cs:getctag/></d:prop>"
    "</d:propfind>";

caldav_error_t caldav_get_ctag(const char *calendar_url,
                               const caldav_auth_t *auth,
                               char *ctag_out,
                               size_t ctag_len) {
   ctag_out[0] = '\0';

   curl_buf_t resp;
   curl_buf_init(&resp);
   if (!resp.data)
      return CALDAV_ERR_ALLOC;

   CURL *curl = caldav_curl_init(auth, &resp);
   if (!curl) {
      curl_buf_free(&resp);
      return CALDAV_ERR_NETWORK;
   }

   caldav_error_t err = do_propfind(curl, calendar_url, "0", PROPFIND_CTAG_XML, &resp);
   curl_easy_cleanup(curl);

   if (err != CALDAV_OK) {
      curl_buf_free(&resp);
      return err;
   }

   xmlDocPtr doc = xmlReadMemory(resp.data, (int)resp.size, NULL, NULL,
                                 XML_PARSE_NONET | XML_PARSE_NOBLANKS);
   curl_buf_free(&resp);
   if (!doc)
      return CALDAV_ERR_PARSE;

   xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
   register_namespaces(ctx);
   xpath_text(ctx, xmlDocGetRootElement(doc), "//cs:getctag", ctag_out, ctag_len);
   xmlXPathFreeContext(ctx);
   xmlFreeDoc(doc);

   return ctag_out[0] ? CALDAV_OK : CALDAV_ERR_PARSE;
}

/* ============================================================================
 * Public: caldav_fetch_events (REPORT with calendar-query)
 * ============================================================================ */

caldav_error_t caldav_fetch_events(const char *calendar_url,
                                   const caldav_auth_t *auth,
                                   time_t range_start,
                                   time_t range_end,
                                   caldav_event_list_t *result) {
   result->events = NULL;
   result->count = 0;

   /* Build time-range filter */
   struct tm ts, te;
   gmtime_r(&range_start, &ts);
   gmtime_r(&range_end, &te);

   char xml[1024];
   snprintf(xml, sizeof(xml),
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<c:calendar-query xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\">"
            "<d:prop><d:getetag/><c:calendar-data/></d:prop>"
            "<c:filter><c:comp-filter name=\"VCALENDAR\">"
            "<c:comp-filter name=\"VEVENT\">"
            "<c:time-range start=\"%04d%02d%02dT000000Z\" end=\"%04d%02d%02dT000000Z\"/>"
            "</c:comp-filter></c:comp-filter></c:filter>"
            "</c:calendar-query>",
            ts.tm_year + 1900, ts.tm_mon + 1, ts.tm_mday, te.tm_year + 1900, te.tm_mon + 1,
            te.tm_mday);

   curl_buf_t resp;
   curl_buf_init(&resp);
   if (!resp.data)
      return CALDAV_ERR_ALLOC;

   CURL *curl = caldav_curl_init(auth, &resp);
   if (!curl) {
      curl_buf_free(&resp);
      return CALDAV_ERR_NETWORK;
   }

   /* REPORT method */
   resp.size = 0;
   curl_easy_setopt(curl, CURLOPT_URL, calendar_url);
   curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "REPORT");
   curl_easy_setopt(curl, CURLOPT_POSTFIELDS, xml);
   curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(xml));

   struct curl_slist *headers = NULL;
   headers = curl_slist_append(headers, "Content-Type: application/xml; charset=utf-8");
   headers = curl_slist_append(headers, "Depth: 1");
   curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

   CURLcode cres = curl_easy_perform(curl);
   curl_slist_free_all(headers);

   if (cres != CURLE_OK) {
      OLOG_ERROR("caldav: REPORT %s failed: %s", calendar_url, curl_easy_strerror(cres));
      curl_easy_cleanup(curl);
      curl_buf_free(&resp);
      return CALDAV_ERR_NETWORK;
   }

   long status = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
   curl_easy_cleanup(curl);

   caldav_error_t err = http_status_to_error(status);
   if (err != CALDAV_OK) {
      curl_buf_free(&resp);
      return err;
   }

   /* Parse multistatus response */
   xmlDocPtr doc = xmlReadMemory(resp.data, (int)resp.size, NULL, NULL,
                                 XML_PARSE_NONET | XML_PARSE_NOBLANKS);
   curl_buf_free(&resp);
   if (!doc)
      return CALDAV_ERR_PARSE;

   xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
   register_namespaces(ctx);

   xmlXPathObjectPtr responses = xmlXPathEvalExpression((const xmlChar *)"//d:response", ctx);

   if (!responses || !responses->nodesetval || responses->nodesetval->nodeNr == 0) {
      xmlXPathFreeObject(responses);
      xmlXPathFreeContext(ctx);
      xmlFreeDoc(doc);
      return CALDAV_OK; /* no events in range */
   }

   int cap = responses->nodesetval->nodeNr;
   caldav_event_t *events = calloc((size_t)cap, sizeof(caldav_event_t));
   if (!events) {
      xmlXPathFreeObject(responses);
      xmlXPathFreeContext(ctx);
      xmlFreeDoc(doc);
      return CALDAV_ERR_ALLOC;
   }

   int count = 0;
   for (int i = 0; i < responses->nodesetval->nodeNr && count < cap; i++) {
      xmlNodePtr resp_node = responses->nodesetval->nodeTab[i];
      caldav_event_t *evt = &events[count];
      memset(evt, 0, sizeof(*evt));

      /* href */
      char href[512] = { 0 };
      xpath_text(ctx, resp_node, "d:href", href, sizeof(href));
      resolve_href(calendar_url, href, evt->href, sizeof(evt->href));

      /* etag */
      xpath_text(ctx, resp_node, ".//d:getetag", evt->etag, sizeof(evt->etag));
      /* Strip quotes from etag */
      size_t elen = strlen(evt->etag);
      if (elen >= 2 && evt->etag[0] == '"' && evt->etag[elen - 1] == '"') {
         memmove(evt->etag, evt->etag + 1, elen - 2);
         evt->etag[elen - 2] = '\0';
      }

      /* calendar-data (raw iCalendar) */
      char *ical_data = NULL;
      {
         ctx->node = resp_node;
         xmlXPathObjectPtr cd = xmlXPathEvalExpression((const xmlChar *)".//c:calendar-data", ctx);
         if (cd && cd->nodesetval && cd->nodesetval->nodeNr > 0) {
            xmlChar *content = xmlNodeGetContent(cd->nodesetval->nodeTab[0]);
            if (content) {
               ical_data = strdup((const char *)content);
               xmlFree(content);
            }
         }
         xmlXPathFreeObject(cd);
      }

      if (!ical_data)
         continue;

      evt->raw_ical = ical_data;

#ifdef HAVE_LIBICAL
      /* Parse iCalendar with libical */
      icalcomponent *comp = icalparser_parse_string(ical_data);
      if (comp) {
         icalcomponent *vevent = icalcomponent_get_first_component(comp, ICAL_VEVENT_COMPONENT);
         if (vevent) {
            /* UID */
            const char *uid = icalcomponent_get_uid(vevent);
            if (uid)
               snprintf(evt->uid, sizeof(evt->uid), "%s", uid);

            /* Summary */
            const char *summary = icalcomponent_get_summary(vevent);
            if (summary)
               snprintf(evt->summary, sizeof(evt->summary), "%s", summary);

            /* Description */
            const char *desc = icalcomponent_get_description(vevent);
            if (desc)
               snprintf(evt->description, sizeof(evt->description), "%s", desc);

            /* Location */
            const char *loc = icalcomponent_get_location(vevent);
            if (loc)
               snprintf(evt->location, sizeof(evt->location), "%s", loc);

            /* DTSTART — convert to UTC epoch for consistent storage.
             * We use icaltime_convert_to_zone() to get UTC components, then
             * timegm() to produce a correct epoch. This avoids mktime() which
             * is affected by the process-wide TZ setting. */
            icaltimezone *utc_zone = icaltimezone_get_utc_timezone();
            icaltimetype dtstart = icalcomponent_get_dtstart(vevent);
            if (!icaltime_is_null_time(dtstart)) {
               if (dtstart.is_date) {
                  evt->all_day = true;
                  snprintf(evt->dtstart_date, sizeof(evt->dtstart_date), "%04d-%02d-%02d",
                           dtstart.year, dtstart.month, dtstart.day);
                  struct tm tm_date = { 0 };
                  tm_date.tm_year = dtstart.year - 1900;
                  tm_date.tm_mon = dtstart.month - 1;
                  tm_date.tm_mday = dtstart.day;
                  evt->dtstart = timegm(&tm_date);
               } else {
                  icaltimetype utc_dt = icaltime_convert_to_zone(dtstart, utc_zone);
                  struct tm utc_tm = { 0 };
                  utc_tm.tm_year = utc_dt.year - 1900;
                  utc_tm.tm_mon = utc_dt.month - 1;
                  utc_tm.tm_mday = utc_dt.day;
                  utc_tm.tm_hour = utc_dt.hour;
                  utc_tm.tm_min = utc_dt.minute;
                  utc_tm.tm_sec = utc_dt.second;
                  evt->dtstart = timegm(&utc_tm);
               }
            }

            /* DTEND */
            icaltimetype dtend = icalcomponent_get_dtend(vevent);
            if (!icaltime_is_null_time(dtend)) {
               if (dtend.is_date) {
                  snprintf(evt->dtend_date, sizeof(evt->dtend_date), "%04d-%02d-%02d", dtend.year,
                           dtend.month, dtend.day);
                  struct tm tm_date = { 0 };
                  tm_date.tm_year = dtend.year - 1900;
                  tm_date.tm_mon = dtend.month - 1;
                  tm_date.tm_mday = dtend.day;
                  evt->dtend = timegm(&tm_date);
               } else {
                  icaltimetype utc_dt = icaltime_convert_to_zone(dtend, utc_zone);
                  struct tm utc_tm = { 0 };
                  utc_tm.tm_year = utc_dt.year - 1900;
                  utc_tm.tm_mon = utc_dt.month - 1;
                  utc_tm.tm_mday = utc_dt.day;
                  utc_tm.tm_hour = utc_dt.hour;
                  utc_tm.tm_min = utc_dt.minute;
                  utc_tm.tm_sec = utc_dt.second;
                  evt->dtend = timegm(&utc_tm);
               }
            }

            /* Duration */
            if (evt->dtstart && evt->dtend) {
               evt->duration_sec = (int)(evt->dtend - evt->dtstart);
            }

            /* RRULE */
            icalproperty *rrprop = icalcomponent_get_first_property(vevent, ICAL_RRULE_PROPERTY);
            if (rrprop) {
               struct icalrecurrencetype rrule = icalproperty_get_rrule(rrprop);
               char *rrstr = icalrecurrencetype_as_string(&rrule);
               if (rrstr)
                  snprintf(evt->rrule, sizeof(evt->rrule), "%s", rrstr);
            }
         }
         icalcomponent_free(comp);
      }
#endif /* HAVE_LIBICAL */

      count++;
   }

   xmlXPathFreeObject(responses);
   xmlXPathFreeContext(ctx);
   xmlFreeDoc(doc);

   result->events = events;
   result->count = count;
   return CALDAV_OK;
}

void caldav_event_list_free(caldav_event_list_t *list) {
   if (list->events) {
      for (int i = 0; i < list->count; i++) {
         free(list->events[i].raw_ical);
      }
      free(list->events);
      list->events = NULL;
   }
   list->count = 0;
}

/* ============================================================================
 * Public: caldav_create_event (PUT with If-None-Match: *)
 * ============================================================================ */

caldav_error_t caldav_create_event(const char *calendar_url,
                                   const caldav_auth_t *auth,
                                   const char *uid,
                                   const char *ical_data,
                                   char *etag_out,
                                   size_t etag_len) {
   if (etag_out && etag_len > 0)
      etag_out[0] = '\0';

   /* Build resource URL: calendar_url + uid + ".ics" */
   char url[1024];
   size_t cal_len = strlen(calendar_url);
   const char *sep = (cal_len > 0 && calendar_url[cal_len - 1] == '/') ? "" : "/";
   snprintf(url, sizeof(url), "%s%s%s.ics", calendar_url, sep, uid);

   curl_buf_t resp;
   curl_buf_init(&resp);
   if (!resp.data)
      return CALDAV_ERR_ALLOC;

   CURL *curl = caldav_curl_init(auth, &resp);
   if (!curl) {
      curl_buf_free(&resp);
      return CALDAV_ERR_NETWORK;
   }

   etag_capture_t etag_cap = { { 0 } };
   curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_etag_cb);
   curl_easy_setopt(curl, CURLOPT_HEADERDATA, &etag_cap);

   curl_easy_setopt(curl, CURLOPT_URL, url);
   curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
   curl_easy_setopt(curl, CURLOPT_POSTFIELDS, ical_data);
   curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(ical_data));

   struct curl_slist *headers = NULL;
   headers = curl_slist_append(headers, "Content-Type: text/calendar; charset=utf-8");
   headers = curl_slist_append(headers, "If-None-Match: *");
   curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

   CURLcode cres = curl_easy_perform(curl);
   curl_slist_free_all(headers);

   if (cres != CURLE_OK) {
      OLOG_ERROR("caldav: PUT create %s failed: %s", url, curl_easy_strerror(cres));
      curl_easy_cleanup(curl);
      curl_buf_free(&resp);
      return CALDAV_ERR_NETWORK;
   }

   long status = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

   /* Capture ETag from response headers */
   if (etag_out && etag_len > 0 && etag_cap.etag[0])
      snprintf(etag_out, etag_len, "%s", etag_cap.etag);

   curl_easy_cleanup(curl);
   curl_buf_free(&resp);
   return http_status_to_error(status);
}

/* ============================================================================
 * Public: caldav_update_event (PUT with If-Match: etag)
 * ============================================================================ */

caldav_error_t caldav_update_event(const char *href,
                                   const caldav_auth_t *auth,
                                   const char *etag,
                                   const char *ical_data,
                                   char *etag_out,
                                   size_t etag_len) {
   if (etag_out && etag_len > 0)
      etag_out[0] = '\0';

   curl_buf_t resp;
   curl_buf_init(&resp);
   if (!resp.data)
      return CALDAV_ERR_ALLOC;

   CURL *curl = caldav_curl_init(auth, &resp);
   if (!curl) {
      curl_buf_free(&resp);
      return CALDAV_ERR_NETWORK;
   }

   etag_capture_t etag_cap = { { 0 } };
   curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_etag_cb);
   curl_easy_setopt(curl, CURLOPT_HEADERDATA, &etag_cap);

   curl_easy_setopt(curl, CURLOPT_URL, href);
   curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
   curl_easy_setopt(curl, CURLOPT_POSTFIELDS, ical_data);
   curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(ical_data));

   struct curl_slist *headers = NULL;
   headers = curl_slist_append(headers, "Content-Type: text/calendar; charset=utf-8");
   if (etag && etag[0]) {
      char if_match[256];
      snprintf(if_match, sizeof(if_match), "If-Match: \"%s\"", etag);
      headers = curl_slist_append(headers, if_match);
   }
   curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

   CURLcode cres = curl_easy_perform(curl);
   curl_slist_free_all(headers);

   if (cres != CURLE_OK) {
      OLOG_ERROR("caldav: PUT update %s failed: %s", href, curl_easy_strerror(cres));
      curl_easy_cleanup(curl);
      curl_buf_free(&resp);
      return CALDAV_ERR_NETWORK;
   }

   long status = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

   /* Capture new ETag from response */
   if (etag_out && etag_len > 0 && etag_cap.etag[0])
      snprintf(etag_out, etag_len, "%s", etag_cap.etag);

   curl_easy_cleanup(curl);
   curl_buf_free(&resp);
   return http_status_to_error(status);
}

/* ============================================================================
 * Public: caldav_delete_event (DELETE with If-Match: etag)
 * ============================================================================ */

caldav_error_t caldav_delete_event(const char *href, const caldav_auth_t *auth, const char *etag) {
   curl_buf_t resp;
   curl_buf_init(&resp);
   if (!resp.data)
      return CALDAV_ERR_ALLOC;

   CURL *curl = caldav_curl_init(auth, &resp);
   if (!curl) {
      curl_buf_free(&resp);
      return CALDAV_ERR_NETWORK;
   }

   curl_easy_setopt(curl, CURLOPT_URL, href);
   curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");

   struct curl_slist *headers = NULL;
   if (etag && etag[0]) {
      char if_match[256];
      snprintf(if_match, sizeof(if_match), "If-Match: \"%s\"", etag);
      headers = curl_slist_append(headers, if_match);
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
   }

   CURLcode cres = curl_easy_perform(curl);
   curl_slist_free_all(headers);

   if (cres != CURLE_OK) {
      OLOG_ERROR("caldav: DELETE %s failed: %s", href, curl_easy_strerror(cres));
      curl_easy_cleanup(curl);
      curl_buf_free(&resp);
      return CALDAV_ERR_NETWORK;
   }

   long status = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
   curl_easy_cleanup(curl);
   curl_buf_free(&resp);
   return http_status_to_error(status);
}

/* ============================================================================
 * Public: caldav_strerror
 * ============================================================================ */

const char *caldav_strerror(caldav_error_t err) {
   switch (err) {
      case CALDAV_OK:
         return "success";
      case CALDAV_ERR_NETWORK:
         return "network error";
      case CALDAV_ERR_AUTH:
         return "authentication failed (401)";
      case CALDAV_ERR_FORBIDDEN:
         return "forbidden (403)";
      case CALDAV_ERR_NOT_FOUND:
         return "not found (404)";
      case CALDAV_ERR_CONFLICT:
         return "conflict/ETag mismatch (412)";
      case CALDAV_ERR_SERVER:
         return "server error (5xx)";
      case CALDAV_ERR_PARSE:
         return "XML/iCalendar parse error";
      case CALDAV_ERR_NO_CALENDARS:
         return "no calendar collections found";
      case CALDAV_ERR_ALLOC:
         return "memory allocation failure";
   }
   return "unknown error";
}

#endif /* DAWN_ENABLE_CALENDAR_TOOL */
