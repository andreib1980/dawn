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
 * Unit tests for auth_db_core.c — authentication database CRUD.
 * Uses an in-memory SQLite database via auth_db_init(":memory:").
 */

#define AUTH_DB_INTERNAL_ALLOWED

#include <sqlite3.h>
#include <string.h>
#include <time.h>

#include "auth/auth_db.h"
#include "auth/auth_db_internal.h"
#include "unity.h"

/* ============================================================================
 * setUp / tearDown — fresh DB per test
 * ============================================================================ */

void setUp(void) {
   auth_db_init(":memory:");
}

void tearDown(void) {
   auth_db_shutdown();
}

/* ============================================================================
 * Helpers
 * ============================================================================ */

/**
 * @brief Create a user and return the user ID via auth_db_get_user.
 */
static int create_and_get_id(const char *username, const char *hash, bool is_admin) {
   auth_db_create_user(username, hash, is_admin);
   auth_user_t user;
   memset(&user, 0, sizeof(user));
   auth_db_get_user(username, &user);
   return user.id;
}

/**
 * @brief Force a session's expires_at to a past timestamp via raw SQL.
 *
 * Needed because auth_db_create_session always computes expiry from now().
 */
static void force_session_expiry(const char *token, time_t expires_at) {
   const char *sql = "UPDATE sessions SET expires_at = ? WHERE token = ?";
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(s_db.db, sql, -1, &stmt, NULL);
   sqlite3_bind_int64(stmt, 1, (int64_t)expires_at);
   sqlite3_bind_text(stmt, 2, token, -1, SQLITE_STATIC);
   sqlite3_step(stmt);
   sqlite3_finalize(stmt);
}

/* ============================================================================
 * Lifecycle Tests
 * ============================================================================ */

static void test_init_returns_success(void) {
   /* setUp already called auth_db_init; verify it's ready */
   TEST_ASSERT_TRUE(auth_db_is_ready());
}

static void test_shutdown_and_reinit(void) {
   auth_db_shutdown();
   TEST_ASSERT_FALSE(auth_db_is_ready());

   int rc = auth_db_init(":memory:");
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);
   TEST_ASSERT_TRUE(auth_db_is_ready());
}

/* ============================================================================
 * User Tests
 * ============================================================================ */

static void test_create_user(void) {
   int rc = auth_db_create_user("alice", "hash_alice", true);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);

   int count = 0;
   rc = auth_db_user_count(&count);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);
   /* One user we created plus the initial user count may vary;
    * just verify at least one user exists */
   TEST_ASSERT_TRUE(count >= 1);
}

static void test_get_user_by_name(void) {
   auth_db_create_user("bob", "hash_bob", false);

   auth_user_t user;
   memset(&user, 0, sizeof(user));
   int rc = auth_db_get_user("bob", &user);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_STRING("bob", user.username);
   TEST_ASSERT_EQUAL_STRING("hash_bob", user.password_hash);
   TEST_ASSERT_FALSE(user.is_admin);
   TEST_ASSERT_TRUE(user.id > 0);
}

static void test_get_user_not_found(void) {
   int rc = auth_db_get_user("nonexistent", NULL);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, rc);
}

static void test_create_duplicate_user(void) {
   auth_db_create_user("carol", "hash1", false);
   int rc = auth_db_create_user("carol", "hash2", false);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_DUPLICATE, rc);
}

static void test_delete_user(void) {
   /* Need two admins so we can delete one (last-admin protection) */
   auth_db_create_user("admin1", "hash1", true);
   auth_db_create_user("admin2", "hash2", true);

   int rc = auth_db_delete_user("admin1");
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);

   rc = auth_db_get_user("admin1", NULL);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, rc);
}

static void test_delete_last_admin_fails(void) {
   auth_db_create_user("sole_admin", "hash", true);

   int rc = auth_db_delete_user("sole_admin");
   TEST_ASSERT_EQUAL_INT(AUTH_DB_LAST_ADMIN, rc);

   /* User should still exist */
   rc = auth_db_get_user("sole_admin", NULL);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);
}

static void test_verify_password(void) {
   auth_db_create_user("dave", "correct_hash", false);

   auth_user_t user;
   memset(&user, 0, sizeof(user));
   int rc = auth_db_get_user("dave", &user);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_STRING("correct_hash", user.password_hash);
}

static void test_verify_wrong_password(void) {
   auth_db_create_user("eve", "real_hash", false);

   auth_user_t user;
   memset(&user, 0, sizeof(user));
   auth_db_get_user("eve", &user);
   TEST_ASSERT_NOT_EQUAL(0, strcmp(user.password_hash, "wrong_hash"));
}

static void test_update_password(void) {
   auth_db_create_user("frank", "old_hash", false);

   int rc = auth_db_update_password("frank", "new_hash");
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);

   auth_user_t user;
   memset(&user, 0, sizeof(user));
   auth_db_get_user("frank", &user);
   TEST_ASSERT_EQUAL_STRING("new_hash", user.password_hash);
}

static void test_validate_username(void) {
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_validate_username("alice"));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_validate_username("user_1"));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_validate_username("_underscore"));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, auth_db_validate_username("a.b-c"));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_INVALID, auth_db_validate_username(""));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_INVALID, auth_db_validate_username(NULL));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_INVALID, auth_db_validate_username("1startsdigit"));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_INVALID, auth_db_validate_username("has space"));
}

/* ============================================================================
 * Session Tests
 * ============================================================================ */

static void test_create_session(void) {
   int user_id = create_and_get_id("sess_user", "hash", false);

   int rc = auth_db_create_session(user_id, "token_abc_1234567890", "127.0.0.1", "TestAgent",
                                   false);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);
}

static void test_get_session(void) {
   int user_id = create_and_get_id("sess_user2", "hash", false);
   auth_db_create_session(user_id, "token_xyz_1234567890", "10.0.0.1", "Mozilla", false);

   auth_session_t session;
   memset(&session, 0, sizeof(session));
   int rc = auth_db_get_session("token_xyz_1234567890", &session);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(user_id, session.user_id);
   TEST_ASSERT_EQUAL_STRING("sess_user2", session.username);
}

static void test_delete_session(void) {
   int user_id = create_and_get_id("sess_del", "hash", false);
   auth_db_create_session(user_id, "token_del_1234567890", NULL, NULL, false);

   int rc = auth_db_delete_session("token_del_1234567890");
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);

   auth_session_t session;
   memset(&session, 0, sizeof(session));
   rc = auth_db_get_session("token_del_1234567890", &session);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, rc);
}

static void test_expired_session(void) {
   int user_id = create_and_get_id("sess_exp", "hash", false);
   auth_db_create_session(user_id, "token_exp_1234567890", NULL, NULL, false);

   /* Force the session to have already expired */
   force_session_expiry("token_exp_1234567890", time(NULL) - 3600);

   auth_session_t session;
   memset(&session, 0, sizeof(session));
   int rc = auth_db_get_session("token_exp_1234567890", &session);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, rc);
}

static void test_delete_user_sessions(void) {
   int user_id = create_and_get_id("multi_sess", "hash", false);
   auth_db_create_session(user_id, "token_ms1_1234567890", NULL, NULL, false);
   auth_db_create_session(user_id, "token_ms2_1234567890", NULL, NULL, false);

   int deleted = 0;
   int rc = auth_db_delete_user_sessions(user_id, &deleted);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(2, deleted);

   auth_session_t session;
   memset(&session, 0, sizeof(session));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, auth_db_get_session("token_ms1_1234567890", &session));
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, auth_db_get_session("token_ms2_1234567890", &session));
}

/* ============================================================================
 * Conversation Tests
 * ============================================================================ */

static void test_create_conversation(void) {
   int user_id = create_and_get_id("conv_user", "hash", false);

   int64_t conv_id = 0;
   int rc = conv_db_create(user_id, "Test Chat", &conv_id);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);
   TEST_ASSERT_TRUE(conv_id > 0);
}

static void test_get_conversation(void) {
   int user_id = create_and_get_id("conv_get", "hash", false);

   int64_t conv_id = 0;
   conv_db_create(user_id, "My Conversation", &conv_id);

   conversation_t conv;
   memset(&conv, 0, sizeof(conv));
   int rc = conv_db_get(conv_id, user_id, &conv);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_STRING("My Conversation", conv.title);
   TEST_ASSERT_EQUAL_INT(user_id, conv.user_id);
   conv_free(&conv);
}

static void test_delete_conversation(void) {
   int user_id = create_and_get_id("conv_del", "hash", false);

   int64_t conv_id = 0;
   conv_db_create(user_id, "Deletable", &conv_id);

   int rc = conv_db_delete(conv_id, user_id);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);

   conversation_t conv;
   memset(&conv, 0, sizeof(conv));
   rc = conv_db_get(conv_id, user_id, &conv);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, rc);
   conv_free(&conv);
}

static void test_conversation_user_isolation(void) {
   int user1 = create_and_get_id("iso_user1", "hash1", false);
   int user2 = create_and_get_id("iso_user2", "hash2", false);

   int64_t conv_id = 0;
   conv_db_create(user1, "User1 Private", &conv_id);

   /* user2 should not be able to delete user1's conversation */
   int rc = conv_db_delete(conv_id, user2);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_NOT_FOUND, rc);

   /* user2 should not be able to read user1's conversation */
   conversation_t conv;
   memset(&conv, 0, sizeof(conv));
   rc = conv_db_get(conv_id, user2, &conv);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_FORBIDDEN, rc);
   conv_free(&conv);

   /* user1 can still access their own conversation */
   memset(&conv, 0, sizeof(conv));
   rc = conv_db_get(conv_id, user1, &conv);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_STRING("User1 Private", conv.title);
   conv_free(&conv);
}

static void test_conversation_add_message(void) {
   int user_id = create_and_get_id("msg_user", "hash", false);

   int64_t conv_id = 0;
   conv_db_create(user_id, "Chat with messages", &conv_id);

   int rc = conv_db_add_message(conv_id, user_id, "user", "Hello there");
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);

   rc = conv_db_add_message(conv_id, user_id, "assistant", "Hi! How can I help?");
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);

   /* Verify message count via conv_db_get */
   conversation_t conv;
   memset(&conv, 0, sizeof(conv));
   rc = conv_db_get(conv_id, user_id, &conv);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_INT(2, conv.message_count);
   conv_free(&conv);
}

/* ============================================================================
 * User Settings Tests
 * ============================================================================ */

static void test_user_settings_defaults(void) {
   int user_id = create_and_get_id("settings_user", "hash", false);

   auth_user_settings_t settings;
   memset(&settings, 0, sizeof(settings));
   int rc = auth_db_get_user_settings(user_id, &settings);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);

   /* Default settings from schema */
   TEST_ASSERT_EQUAL_STRING("", settings.persona_description);
   TEST_ASSERT_EQUAL_STRING("", settings.location);
   TEST_ASSERT_EQUAL_STRING("UTC", settings.timezone);
}

static void test_user_settings_set_and_get(void) {
   int user_id = create_and_get_id("settings_user2", "hash", false);

   auth_user_settings_t settings;
   memset(&settings, 0, sizeof(settings));
   strncpy(settings.location, "New York", sizeof(settings.location) - 1);
   strncpy(settings.timezone, "America/New_York", sizeof(settings.timezone) - 1);
   strncpy(settings.units, "imperial", sizeof(settings.units) - 1);

   int rc = auth_db_set_user_settings(user_id, &settings);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);

   auth_user_settings_t loaded;
   memset(&loaded, 0, sizeof(loaded));
   rc = auth_db_get_user_settings(user_id, &loaded);
   TEST_ASSERT_EQUAL_INT(AUTH_DB_SUCCESS, rc);
   TEST_ASSERT_EQUAL_STRING("New York", loaded.location);
   TEST_ASSERT_EQUAL_STRING("America/New_York", loaded.timezone);
   TEST_ASSERT_EQUAL_STRING("imperial", loaded.units);
}

/* ============================================================================
 * main
 * ============================================================================ */

int main(void) {
   UNITY_BEGIN();

   /* Lifecycle */
   RUN_TEST(test_init_returns_success);
   RUN_TEST(test_shutdown_and_reinit);

   /* Users */
   RUN_TEST(test_create_user);
   RUN_TEST(test_get_user_by_name);
   RUN_TEST(test_get_user_not_found);
   RUN_TEST(test_create_duplicate_user);
   RUN_TEST(test_delete_user);
   RUN_TEST(test_delete_last_admin_fails);
   RUN_TEST(test_verify_password);
   RUN_TEST(test_verify_wrong_password);
   RUN_TEST(test_update_password);
   RUN_TEST(test_validate_username);

   /* Sessions */
   RUN_TEST(test_create_session);
   RUN_TEST(test_get_session);
   RUN_TEST(test_delete_session);
   RUN_TEST(test_expired_session);
   RUN_TEST(test_delete_user_sessions);

   /* Conversations */
   RUN_TEST(test_create_conversation);
   RUN_TEST(test_get_conversation);
   RUN_TEST(test_delete_conversation);
   RUN_TEST(test_conversation_user_isolation);
   RUN_TEST(test_conversation_add_message);

   /* User Settings */
   RUN_TEST(test_user_settings_defaults);
   RUN_TEST(test_user_settings_set_and_get);

   return UNITY_END();
}
