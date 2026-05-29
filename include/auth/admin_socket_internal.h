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
 * Admin socket internal API — shared between admin_socket.c and its
 * per-feature sibling files (admin_socket_memory_entity.c, etc.).  Not part
 * of the public admin_socket.h surface; do not include from outside the
 * src/auth/admin_socket*.c family.
 */

#ifndef DAWN_AUTH_ADMIN_SOCKET_INTERNAL_H
#define DAWN_AUTH_ADMIN_SOCKET_INTERNAL_H

/* Security guard — only admin_socket modules should include this. */
#ifndef ADMIN_SOCKET_INTERNAL_ALLOWED
#error "admin_socket_internal.h is an internal header - include auth/admin_socket.h instead"
#endif

#include <stdint.h>

#include "auth/admin_socket.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Wire-level reply helpers shared across admin handler modules.  Defined in
 * admin_socket.c. */
int send_response(int client_fd, admin_resp_code_t code);
int send_text_response(int client_fd, admin_resp_code_t code, const char *text);

/* Phase 6.5 entity-merge handlers (admin_socket_memory_entity.c).  Dispatched
 * from handle_client() in admin_socket.c against ADMIN_MSG_MEMORY_ENTITY_*
 * opcodes 0x83-0x88. */
int handle_memory_entity_merge(int client_fd, const char *payload, uint16_t payload_len);
int handle_memory_entity_split(int client_fd, const char *payload, uint16_t payload_len);
int handle_memory_entity_aliases(int client_fd, const char *payload, uint16_t payload_len);
int handle_memory_entity_history(int client_fd, const char *payload, uint16_t payload_len);
int handle_memory_entity_list(int client_fd, const char *payload, uint16_t payload_len);
int handle_memory_entity_link_user_self(int client_fd, const char *payload, uint16_t payload_len);

/* Memory-maintenance handlers (admin_socket_memory.c).  Dispatched from
 * handle_client() in admin_socket.c against ADMIN_MSG_MEMORY_* opcodes. */
int handle_memory_recategorize(int client_fd, const char *payload, uint16_t payload_len);
int handle_memory_cleanup_meta_facts(int client_fd, const char *payload, uint16_t payload_len);
int handle_memory_summarize_missing(int client_fd, const char *payload, uint16_t payload_len);
int handle_memory_reextract(int client_fd, const char *payload, uint16_t payload_len);
int handle_memory_reextract_status(int client_fd, const char *payload, uint16_t payload_len);

/* Messaging-channels handlers (admin_socket_messaging.c).  Dispatched from
 * handle_client() in admin_socket.c against ADMIN_MSG_MESSAGING_* opcodes. */
int handle_messaging_generate_link_code(int client_fd, const char *payload, uint16_t payload_len);
int handle_messaging_list_channels(int client_fd, const char *payload, uint16_t payload_len);
int handle_messaging_unlink_channel(int client_fd, const char *payload, uint16_t payload_len);
int handle_messaging_link_attempts(int client_fd, const char *payload, uint16_t payload_len);
int handle_messaging_reenable_channel(int client_fd, const char *payload, uint16_t payload_len);

#ifdef __cplusplus
}
#endif

#endif /* DAWN_AUTH_ADMIN_SOCKET_INTERNAL_H */
