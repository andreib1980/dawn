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
 * WebUI Contacts Management — per-user contact CRUD via WebSocket
 */

#ifndef WEBUI_CONTACTS_H
#define WEBUI_CONTACTS_H

#include "webui/webui_internal.h"

/** List all contacts for the current user */
void handle_contacts_list(ws_connection_t *conn, json_object *payload);

/** Search contacts by name */
void handle_contacts_search(ws_connection_t *conn, json_object *payload);

/** Add a new contact */
void handle_contacts_add(ws_connection_t *conn, json_object *payload);

/** Update an existing contact */
void handle_contacts_update(ws_connection_t *conn, json_object *payload);

/** Delete a contact */
void handle_contacts_delete(ws_connection_t *conn, json_object *payload);

/** Search entities by name for typeahead (returns person entities matching query) */
void handle_contacts_search_entities(ws_connection_t *conn, json_object *payload);

/** Set or clear an entity's contact photo */
void handle_entity_set_photo(ws_connection_t *conn, json_object *payload);

/** Find or create a person entity by name (lightweight upsert for photo-only flow) */
void handle_entity_ensure(ws_connection_t *conn, json_object *payload);

#endif /* WEBUI_CONTACTS_H */
