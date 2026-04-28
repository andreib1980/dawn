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
 * Email LLM tool — voice-controlled IMAP/SMTP email access.
 * Actions: recent, read, search, folders, send, confirm_send, trash, confirm_trash, archive,
 * accounts
 */

#include "tools/email_tool.h"

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "core/session_manager.h"
#include "logging.h"
#include "memory/contacts_db.h"
#include "tools/email_service.h"
#include "tools/toml.h"
#include "tools/tool_registry.h"

/* =============================================================================
 * Constants
 * ============================================================================= */

#define RESULT_BUF_SIZE 16384
#define MAX_EMAIL_RESULTS 50

/* =============================================================================
 * Config (TOOL_CAP_DANGEROUS requires enabled = true as first field)
 * ============================================================================= */

typedef struct {
   bool enabled;
} email_tool_config_t;

static email_tool_config_t s_config;

/* =============================================================================
 * Forward Declarations
 * ============================================================================= */

static char *email_tool_callback(const char *action, char *value, int *should_respond);
static int email_tool_init(void);
static void email_tool_cleanup(void);
static bool email_tool_available(void);

/* =============================================================================
 * JSON Helpers (same as calendar_tool.c)
 * ============================================================================= */

static const char *json_get_str(struct json_object *obj, const char *key) {
   struct json_object *val = NULL;
   if (!json_object_object_get_ex(obj, key, &val))
      return NULL;
   return json_object_get_string(val);
}

static int json_get_int(struct json_object *obj, const char *key, int def) {
   struct json_object *val = NULL;
   if (!json_object_object_get_ex(obj, key, &val))
      return def;
   return json_object_get_int(val);
}

/* =============================================================================
 * Access Summary Footer (tells LLM which accounts are read-only)
 * ============================================================================= */

static int append_access_summary(char *buf, int pos, size_t buf_len, int user_id) {
   char writable[512] = { 0 }, ro[512] = { 0 };
   if (email_service_get_access_summary(user_id, writable, sizeof(writable), ro, sizeof(ro)) > 0) {
      pos += snprintf(buf + pos, buf_len - pos,
                      "\n\nWritable accounts: %s\nRead-only accounts (no sending): %s",
                      writable[0] ? writable : "(none)", ro);
   }
   return pos;
}

/* =============================================================================
 * Action Handlers
 * ============================================================================= */

static char *handle_accounts(int user_id) {
   email_account_t accounts[EMAIL_MAX_ACCOUNTS];
   int count = email_service_list_accounts(user_id, accounts, EMAIL_MAX_ACCOUNTS);

   char *buf = malloc(RESULT_BUF_SIZE);
   if (!buf)
      return strdup("Error: memory allocation failed");

   int pos = 0;
   if (count <= 0) {
      pos += snprintf(buf, RESULT_BUF_SIZE, "No email accounts configured.");
      return buf;
   }

   pos += snprintf(buf, RESULT_BUF_SIZE, "Email accounts (%d):\n", count);

   for (int i = 0; i < count && pos < RESULT_BUF_SIZE - 256; i++) {
      const char *status = accounts[i].enabled ? "" : " [DISABLED]";
      const char *ro = accounts[i].read_only ? " [READ-ONLY]" : "";
      const char *auth = strcmp(accounts[i].auth_type, "oauth") == 0 ? " (Google OAuth)" : "";

      pos += snprintf(buf + pos, RESULT_BUF_SIZE - pos, "- %s%s%s%s (%s)\n", accounts[i].name, ro,
                      status, auth, accounts[i].username);
   }

   pos = append_access_summary(buf, pos, RESULT_BUF_SIZE, user_id);
   return buf;
}

static bool json_get_bool(struct json_object *obj, const char *key, bool def) {
   struct json_object *val = NULL;
   if (!json_object_object_get_ex(obj, key, &val))
      return def;
   return json_object_get_boolean(val);
}

static char *handle_recent(struct json_object *details, int user_id) {
   int count = json_get_int(details, "count", 10);
   const char *account = json_get_str(details, "account");
   const char *folder = json_get_str(details, "folder");
   bool unread_only = json_get_bool(details, "unread_only", false);
   const char *page_token = json_get_str(details, "page_token");

   email_summary_t emails[MAX_EMAIL_RESULTS];
   int out_count = 0;
   char next_page_token[256] = { 0 };
   int rc = email_service_recent(user_id, account, folder, count, unread_only, page_token, emails,
                                 MAX_EMAIL_RESULTS, &out_count, next_page_token,
                                 sizeof(next_page_token));

   if (rc != 0)
      return strdup("Error: failed to fetch recent emails. Check account configuration.");

   char *buf = malloc(RESULT_BUF_SIZE);
   if (!buf)
      return strdup("Error: memory allocation failed");

   int pos = 0;
   if (out_count == 0) {
      pos += snprintf(buf, RESULT_BUF_SIZE, "No recent emails found.");
   } else {
      pos += snprintf(buf, RESULT_BUF_SIZE, "Recent emails (%d):\n", out_count);
      for (int i = 0; i < out_count && pos < RESULT_BUF_SIZE - 512; i++) {
         pos += snprintf(buf + pos, RESULT_BUF_SIZE - pos,
                         "\n%d. From: %s%s%s\n   Subject: %s\n   Date: %s\n   [ID: %s]\n", i + 1,
                         emails[i].from_name, emails[i].from_name[0] ? " " : "",
                         emails[i].from_addr, emails[i].subject, emails[i].date_str,
                         emails[i].message_id);
      }
   }

   if (next_page_token[0])
      pos += snprintf(buf + pos, RESULT_BUF_SIZE - pos,
                      "\n[More results available. Use page_token: \"%s\" to fetch next page]",
                      next_page_token);

   return buf;
}

static char *handle_read(struct json_object *details, int user_id) {
   /* Accept message_id (string) or fall back to uid (int) for backward compat */
   const char *mid = json_get_str(details, "message_id");
   char message_id[192] = "";
   if (mid && mid[0]) {
      snprintf(message_id, sizeof(message_id), "%s", mid);
   } else {
      int uid_val = json_get_int(details, "uid", 0);
      if (uid_val <= 0)
         return strdup(
             "Error: 'message_id' is required (get IDs from 'recent' or 'search' results)");
      snprintf(message_id, sizeof(message_id), "%u", (uint32_t)uid_val);
   }

   const char *account = json_get_str(details, "account");

   email_message_t msg = { 0 };
   int rc = email_service_read(user_id, account, message_id, &msg);
   if (rc != 0)
      return strdup("Error: failed to read email");

   char *buf = malloc(RESULT_BUF_SIZE);
   if (!buf) {
      email_message_free(&msg);
      return strdup("Error: memory allocation failed");
   }

   int pos = 0;
   pos += snprintf(buf, RESULT_BUF_SIZE, "From: %s%s%s%s\nTo: %s\nSubject: %s\nDate: %s\n",
                   msg.from_name, msg.from_name[0] ? " <" : "", msg.from_addr,
                   msg.from_name[0] ? ">" : "", msg.to, msg.subject, msg.date_str);

   if (msg.attachment_count > 0)
      pos += snprintf(buf + pos, RESULT_BUF_SIZE - pos, "Attachments: %d\n", msg.attachment_count);

   pos += snprintf(buf + pos, RESULT_BUF_SIZE - pos, "\n%s", msg.body ? msg.body : "(No body)");

   if (msg.truncated)
      pos += snprintf(buf + pos, RESULT_BUF_SIZE - pos, "\n[Message truncated]");

   email_message_free(&msg);
   return buf;
}

static char *handle_search(struct json_object *details, int user_id) {
   const char *account = json_get_str(details, "account");

   email_search_params_t params = { 0 };

   const char *folder = json_get_str(details, "folder");
   if (folder)
      snprintf(params.folder, sizeof(params.folder), "%s", folder);

   const char *from = json_get_str(details, "from");
   if (from)
      snprintf(params.from, sizeof(params.from), "%s", from);

   const char *subject = json_get_str(details, "subject");
   if (subject)
      snprintf(params.subject, sizeof(params.subject), "%s", subject);

   const char *text = json_get_str(details, "text");
   if (text)
      snprintf(params.text, sizeof(params.text), "%s", text);

   const char *since = json_get_str(details, "since");
   if (since)
      snprintf(params.since, sizeof(params.since), "%s", since);

   const char *before = json_get_str(details, "before");
   if (before)
      snprintf(params.before, sizeof(params.before), "%s", before);

   const char *page_token = json_get_str(details, "page_token");
   if (page_token)
      snprintf(params.page_token, sizeof(params.page_token), "%s", page_token);

   params.unread_only = json_get_bool(details, "unread_only", false);

   email_summary_t emails[MAX_EMAIL_RESULTS];
   int out_count = 0;
   char next_page_token[256] = { 0 };
   int rc = email_service_search(user_id, account, &params, emails, MAX_EMAIL_RESULTS, &out_count,
                                 next_page_token, sizeof(next_page_token));

   if (rc != 0)
      return strdup("Error: email search failed");

   char *buf = malloc(RESULT_BUF_SIZE);
   if (!buf)
      return strdup("Error: memory allocation failed");

   int pos = 0;
   if (out_count == 0) {
      pos += snprintf(buf, RESULT_BUF_SIZE, "No emails matching your search criteria.");
   } else {
      pos += snprintf(buf, RESULT_BUF_SIZE, "Search results (%d):\n", out_count);
      for (int i = 0; i < out_count && pos < RESULT_BUF_SIZE - 512; i++) {
         pos += snprintf(buf + pos, RESULT_BUF_SIZE - pos,
                         "\n%d. From: %s%s%s\n   Subject: %s\n   Date: %s\n   [ID: %s]\n", i + 1,
                         emails[i].from_name, emails[i].from_name[0] ? " " : "",
                         emails[i].from_addr, emails[i].subject, emails[i].date_str,
                         emails[i].message_id);
      }
   }

   if (next_page_token[0])
      pos += snprintf(buf + pos, RESULT_BUF_SIZE - pos,
                      "\n[More results available. Use page_token: \"%s\" to fetch next page]",
                      next_page_token);

   return buf;
}

static char *handle_send(struct json_object *details, int user_id) {
   const char *to = json_get_str(details, "to");
   const char *subject = json_get_str(details, "subject");
   const char *body = json_get_str(details, "body");

   if (!to || !to[0])
      return strdup("Error: 'to' is required (email address or contact name)");
   if (!subject || !subject[0])
      return strdup("Error: 'subject' is required");
   if (!body || !body[0])
      return strdup("Error: 'body' is required");

   /* Validate field lengths */
   if (strlen(body) > EMAIL_MAX_BODY_LEN) {
      char err[128];
      snprintf(err, sizeof(err), "Error: email body too long (%zu chars, max %d)", strlen(body),
               EMAIL_MAX_BODY_LEN);
      return strdup(err);
   }
   if (strlen(subject) > EMAIL_MAX_SUBJECT_LEN) {
      return strdup("Error: subject too long (max 250 characters)");
   }

   /* Contact resolution: if no @, try looking up as a contact name */
   char resolved_addr[256] = "";
   char resolved_name[64] = "";

   if (strchr(to, '@')) {
      snprintf(resolved_addr, sizeof(resolved_addr), "%s", to);
   } else {
      contact_result_t contacts[5];
      int found = 0;
      contacts_find(user_id, to, "email", contacts, 5, &found);

      if (found == 0) {
         char err[256];
         snprintf(err, sizeof(err),
                  "Error: no email address found for '%s'. Ask the user for the email address "
                  "or use save_contact to store it.",
                  to);
         return strdup(err);
      } else if (found == 1) {
         snprintf(resolved_addr, sizeof(resolved_addr), "%s", contacts[0].value);
         snprintf(resolved_name, sizeof(resolved_name), "%s", contacts[0].entity_name);
      } else {
         /* Multiple matches — ask LLM to disambiguate */
         char *buf = malloc(1024);
         if (!buf)
            return strdup("Error: memory allocation failed");
         int pos = snprintf(buf, 1024, "Multiple email addresses found for '%s':\n", to);
         if (pos > 1024)
            pos = 1024;
         for (int i = 0; i < found && pos < 900; i++) {
            pos += snprintf(buf + pos, 1024 - pos, "- %s: %s%s%s\n", contacts[i].entity_name,
                            contacts[i].value, contacts[i].label[0] ? " (" : "",
                            contacts[i].label[0] ? contacts[i].label : "");
            if (pos > 1024)
               pos = 1024;
            if (contacts[i].label[0] && pos < 1024) {
               pos += snprintf(buf + pos, 1024 - pos, ")");
               if (pos > 1024)
                  pos = 1024;
            }
         }
         if (pos < 1024)
            snprintf(buf + pos, 1024 - pos, "\nPlease specify which email address to use.");
         return buf;
      }
   }

   /* Create draft (two-step send) */
   char draft_id[16];
   int rc = email_service_create_draft(user_id, resolved_addr, resolved_name, subject, body,
                                       draft_id, sizeof(draft_id));
   if (rc == 2)
      return strdup("Error: all email accounts are read-only. Cannot send emails.");
   if (rc != 0)
      return strdup("Error: failed to create email draft");

   char *buf = malloc(RESULT_BUF_SIZE);
   if (!buf)
      return strdup("Error: memory allocation failed");

   snprintf(buf, RESULT_BUF_SIZE,
            "Draft email prepared:\n"
            "  To: %s%s%s%s\n"
            "  Subject: %s\n"
            "  Body: %s\n\n"
            "Read this back to the user and ask for confirmation. "
            "If confirmed, call confirm_send with draft_id '%s'.",
            resolved_name[0] ? resolved_name : "", resolved_name[0] ? " <" : "", resolved_addr,
            resolved_name[0] ? ">" : "", subject, body, draft_id);

   return buf;
}

static char *handle_confirm_send(struct json_object *details, int user_id) {
   const char *draft_id = json_get_str(details, "draft_id");
   if (!draft_id || !draft_id[0])
      return strdup("Error: 'draft_id' is required");

   int rc = email_service_confirm_send(user_id, draft_id);
   switch (rc) {
      case 0:
         return strdup("Email sent successfully.");
      case 2:
         return strdup("Error: draft not found or expired. The draft may have timed out "
                       "(5-minute limit). Please use 'send' to create a new draft.");
      case 3:
         return strdup("Error: too many failed confirmation attempts. Please wait 60 seconds "
                       "before trying again.");
      default:
         return strdup("Error: failed to send email. Check account configuration.");
   }
}

static char *handle_folders(struct json_object *details, int user_id) {
   const char *account = json_get_str(details, "account");

   char buf[4096];
   int rc = email_service_list_folders(user_id, account, buf, sizeof(buf));
   if (rc != 0)
      return strdup("Error: failed to list folders. Check account configuration.");

   if (!buf[0])
      return strdup("No folders found.");

   char *result = malloc(RESULT_BUF_SIZE);
   if (!result)
      return strdup("Error: memory allocation failed");

   if (account && account[0])
      snprintf(result, RESULT_BUF_SIZE, "Folders for '%s':\n%s", account, buf);
   else
      snprintf(result, RESULT_BUF_SIZE, "Available folders/labels:\n%s", buf);
   return result;
}

/* =============================================================================
 * Trash / Archive Handlers
 * ============================================================================= */

static char *handle_trash(struct json_object *details, int user_id) {
   const char *mid = json_get_str(details, "message_id");
   if (!mid || !mid[0])
      return strdup("Error: 'message_id' is required (get IDs from 'recent' or 'search' results)");

   const char *account = json_get_str(details, "account");

   char pending_id[16];
   char subject[256] = { 0 };
   char from[128] = { 0 };
   int rc = email_service_create_pending_trash(user_id, account, mid, pending_id,
                                               sizeof(pending_id), subject, sizeof(subject), from,
                                               sizeof(from));
   if (rc == 2)
      return strdup("Error: email account is read-only. Cannot trash emails.");
   if (rc != 0)
      return strdup("Error: failed to prepare trash action. Check account configuration.");

   char *buf = malloc(RESULT_BUF_SIZE);
   if (!buf)
      return strdup("Error: memory allocation failed");

   snprintf(buf, RESULT_BUF_SIZE,
            "Pending trash:\n"
            "  From: %s\n"
            "  Subject: %s\n\n"
            "Confirm with the user before proceeding. "
            "If confirmed, call confirm_trash with pending_id '%s'.",
            from, subject, pending_id);

   return buf;
}

static char *handle_confirm_trash(struct json_object *details, int user_id) {
   const char *pending_id = json_get_str(details, "pending_id");
   if (!pending_id || !pending_id[0])
      return strdup("Error: 'pending_id' is required");

   int rc = email_service_confirm_trash(user_id, pending_id);
   switch (rc) {
      case 0:
         return strdup("Email moved to Trash successfully.");
      case 2:
         return strdup("Error: pending trash not found or expired. The request may have timed out "
                       "(5-minute limit). Please use 'trash' to create a new request.");
      case 3:
         return strdup("Error: too many failed confirmation attempts. Please wait 60 seconds "
                       "before trying again.");
      default:
         return strdup("Error: failed to trash email. Check account configuration.");
   }
}

static char *handle_archive(struct json_object *details, int user_id) {
   const char *mid = json_get_str(details, "message_id");
   if (!mid || !mid[0])
      return strdup("Error: 'message_id' is required (get IDs from 'recent' or 'search' results)");

   const char *account = json_get_str(details, "account");

   int rc = email_service_archive(user_id, account, mid);
   switch (rc) {
      case 0:
         return strdup("Email archived successfully (removed from Inbox, kept in All Mail).");
      case 2:
         return strdup("Error: email account is read-only. Cannot archive emails.");
      default:
         return strdup("Error: failed to archive email. Check account configuration.");
   }
}

/* =============================================================================
 * Tool Callback
 * ============================================================================= */

static char *email_tool_callback(const char *action, char *value, int *should_respond) {
   *should_respond = 1;

   if (!action || !action[0])
      return strdup("Error: action is required");

   struct json_object *details = NULL;
   if (value && value[0]) {
      details = json_tokener_parse(value);
      if (!details)
         return strdup("Error: invalid JSON in details parameter");
   } else {
      details = json_object_new_object();
   }

   int user_id = tool_get_current_user_id();

   char *result = NULL;

   if (strcmp(action, "accounts") == 0) {
      result = handle_accounts(user_id);
   } else if (strcmp(action, "recent") == 0) {
      result = handle_recent(details, user_id);
   } else if (strcmp(action, "read") == 0) {
      result = handle_read(details, user_id);
   } else if (strcmp(action, "search") == 0) {
      result = handle_search(details, user_id);
   } else if (strcmp(action, "folders") == 0) {
      result = handle_folders(details, user_id);
   } else if (strcmp(action, "send") == 0) {
      result = handle_send(details, user_id);
   } else if (strcmp(action, "confirm_send") == 0) {
      result = handle_confirm_send(details, user_id);
   } else if (strcmp(action, "trash") == 0) {
      result = handle_trash(details, user_id);
   } else if (strcmp(action, "confirm_trash") == 0) {
      result = handle_confirm_trash(details, user_id);
   } else if (strcmp(action, "archive") == 0) {
      result = handle_archive(details, user_id);
   } else {
      char buf[256];
      snprintf(buf, sizeof(buf),
               "Error: unknown action '%s'. Valid: accounts, recent, read, search, folders, "
               "send, confirm_send, trash, confirm_trash, archive",
               action);
      result = strdup(buf);
   }

   json_object_put(details);
   return result;
}

/* =============================================================================
 * Lifecycle
 * ============================================================================= */

static int email_tool_init(void) {
   return email_service_init();
}

static void email_tool_cleanup(void) {
   email_service_shutdown();
}

static bool email_tool_available(void) {
   return s_config.enabled && email_service_available();
}

/* =============================================================================
 * Config Parser
 * ============================================================================= */

static void email_tool_config_parse(toml_table_t *table, void *config) {
   email_tool_config_t *cfg = (email_tool_config_t *)config;

   if (!table)
      return;

   toml_datum_t enabled = toml_bool_in(table, "enabled");
   if (enabled.ok)
      cfg->enabled = enabled.u.b;
}

/* =============================================================================
 * Tool Registration
 * ============================================================================= */

static const treg_param_t email_params[] = {
   {
       .name = "action",
       .description = "Email action: 'accounts' (list configured accounts), "
                      "'recent' (fetch recent emails), 'read' (read full email by message_id), "
                      "'search' (search by from/subject/text/date), "
                      "'folders' (list available folders/labels), "
                      "'send' (compose draft for user confirmation), "
                      "'confirm_send' (send confirmed draft), "
                      "'trash' (move email to trash — requires confirmation), "
                      "'confirm_trash' (confirm trash action), "
                      "'archive' (remove from inbox, keep in All Mail)",
       .type = TOOL_PARAM_TYPE_ENUM,
       .required = true,
       .maps_to = TOOL_MAPS_TO_ACTION,
       .enum_values = { "recent", "read", "search", "folders", "send", "confirm_send", "accounts",
                        "trash", "confirm_trash", "archive" },
       .enum_count = 10,
   },
   {
       .name = "details",
       .description =
           "JSON: "
           "recent {count? (up to 50), folder?, unread_only?, account?, page_token?}, "
           "read {message_id, account?}, "
           "search {from?, subject?, text?, since?, before?, folder?, unread_only?, "
           "account?, page_token?} (dates: YYYY-MM-DD), "
           "folders {account?}, "
           "send {to, subject, body} (to: email or contact name), "
           "confirm_send {draft_id}, "
           "trash {message_id, account?} (creates pending — ask user to confirm), "
           "confirm_trash {pending_id}, "
           "archive {message_id, account?} (removes from inbox, no confirmation needed). "
           "Results include a page_token when more results are available — "
           "pass it in the next call to fetch the next page.",
       .type = TOOL_PARAM_TYPE_STRING,
       .required = false,
       .maps_to = TOOL_MAPS_TO_VALUE,
   },
};

static const tool_metadata_t email_metadata = {
   .name = "email",
   .device_string = "email",
   .topic = "dawn",
   .aliases = { "mail", "inbox", "gmail" },
   .alias_count = 3,

   .description = "Read, send, trash, and archive emails. "
                  "Use 'accounts' to see configured email accounts. "
                  "Use 'folders' to list available folders/labels for an account. "
                  "Use 'recent' to fetch recent emails sorted newest-first "
                  "(set unread_only:true for unread only, folder to specify folder/label). "
                  "Use 'read' to read a full email by message_id (from recent/search results). "
                  "Use 'search' to find emails by from, subject, text, or date range "
                  "(results sorted newest-first, set unread_only:true to filter unread). "
                  "The 'folder' parameter (on recent/search) selects which folder/label: "
                  "inbox, sent, trash, spam, drafts, starred, important, all, "
                  "or a custom label name. Default is inbox. "
                  "Use 'send' to compose a draft (reads back to user for confirmation). "
                  "Use 'confirm_send' with the draft_id to actually send after user confirms. "
                  "Use 'trash' to move an email to trash (two-step: creates pending action, "
                  "then 'confirm_trash' executes after user confirms). "
                  "Use 'archive' to remove an email from inbox (keeps in All Mail, no "
                  "confirmation needed). "
                  "Emails are sent from the first writable account. "
                  "For 'send', the 'to' field can be a contact name (resolved via contacts) "
                  "or a direct email address.",
   .params = email_params,
   .param_count = 2,

   .device_type = TOOL_DEVICE_TYPE_TRIGGER,
   .capabilities = TOOL_CAP_NETWORK | TOOL_CAP_DANGEROUS | TOOL_CAP_SCHEDULABLE,
   .is_getter = false,
   .skip_followup = false,
   .default_local = true,
   .default_remote = true,

   .config = &s_config,
   .config_size = sizeof(s_config),
   .config_parser = email_tool_config_parse,
   .config_section = "email",

   .is_available = email_tool_available,
   .init = email_tool_init,
   .cleanup = email_tool_cleanup,
   .callback = email_tool_callback,
};

int email_tool_register(void) {
   return tool_registry_register(&email_metadata);
}
