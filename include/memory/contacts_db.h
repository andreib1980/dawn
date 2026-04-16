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
 * Contacts database — links contact info (email, phone) to memory entities.
 */

#ifndef CONTACTS_DB_H
#define CONTACTS_DB_H

#include <stdint.h>

typedef struct {
   int64_t contact_id;
   int64_t entity_id;
   char entity_name[64];
   char canonical_name[64];
   char field_type[16]; /* "email", "phone", "address" */
   char value[256];
   char label[32];    /* "work", "personal", "mobile" */
   char photo_id[32]; /* image store ID or empty */
} contact_result_t;

/**
 * @brief Find contacts by entity name and optional field type.
 *
 * Performs case-insensitive LIKE search on entity canonical_name.
 * Escapes LIKE metacharacters (%, _, \) in the name parameter.
 *
 * @param user_id     User ID for isolation
 * @param name        Entity name to search for (fuzzy match)
 * @param field_type  Filter by type ("email", "phone", etc.) or NULL for all
 * @param out         Output array
 * @param max_results Maximum results to return
 * @return Number of results found, or -1 on error
 */
int contacts_find(int user_id,
                  const char *name,
                  const char *field_type,
                  contact_result_t *out,
                  int max_results);

/**
 * @brief Add a contact info record for an entity.
 *
 * @param user_id    User ID
 * @param entity_id  Memory entity ID
 * @param field_type Type: "email", "phone", "address"
 * @param value      The contact value (e.g., email address)
 * @param label      Optional label: "work", "personal", etc.
 * @return 0 on success, 1 on failure
 */
int contacts_add(int user_id,
                 int64_t entity_id,
                 const char *field_type,
                 const char *value,
                 const char *label);

/**
 * @brief Delete a contact record by ID.
 *
 * @param user_id    User ID (ownership check)
 * @param contact_id Contact record ID
 * @return 0 on success, 1 on failure
 */
int contacts_delete(int user_id, int64_t contact_id);

/**
 * @brief List all contacts for a user, optionally filtered by type.
 *
 * @param user_id    User ID
 * @param field_type Filter by exact type ("email", "phone", "address") or NULL for all
 * @param out        Output array
 * @param max_results Maximum results
 * @param offset     Number of rows to skip (for pagination)
 * @return Number of results, or -1 on error
 */
int contacts_list(int user_id,
                  const char *field_type,
                  contact_result_t *out,
                  int max_results,
                  int offset);

/**
 * @brief Count all contacts for a user.
 *
 * @param user_id User ID
 * @return Contact count, or -1 on error
 */
int contacts_count(int user_id);

/**
 * @brief Update an existing contact record.
 *
 * @param user_id    User ID (ownership check)
 * @param contact_id Contact record ID
 * @param field_type New field type ("email", "phone", "address")
 * @param value      New contact value
 * @param label      New label ("work", "personal", etc.) or NULL
 * @return 0 on success, 1 on failure
 */
int contacts_update(int user_id,
                    int64_t contact_id,
                    const char *field_type,
                    const char *value,
                    const char *label);

#endif /* CONTACTS_DB_H */
