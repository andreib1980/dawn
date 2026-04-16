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
 * Phone Service — call state machine, event handling, contact resolution,
 * TTS announcements, HUD MQTT dispatch.
 */

#ifndef PHONE_SERVICE_H
#define PHONE_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Phone states */
typedef enum {
   PHONE_STATE_IDLE = 0,
   PHONE_STATE_DIALING,
   PHONE_STATE_RINGING_IN,
   PHONE_STATE_ACTIVE,
   PHONE_STATE_HANGING_UP,
} phone_state_t;

/* Phone service configuration */
typedef struct {
   bool enabled;
   bool confirm_outbound;
   char audio_device[64];
   int user_id; /* owner of the modem — contacts/logs scoped to this user */
   int sms_retention_days;
   int call_log_retention_days;
   int rate_limit_sms_per_min;
   int rate_limit_calls_per_min;
   int rate_limit_sms_per_day;
} phone_service_config_t;

/**
 * @brief Initialize the phone service.
 *
 * Subscribes to echo/events and echo/response via DAWN's MQTT connection.
 *
 * @return 0 on success, 1 on failure.
 */
int phone_service_init(void);

/**
 * @brief Shutdown the phone service.
 */
void phone_service_shutdown(void);

/**
 * @brief Check if the phone service is available (ECHO daemon online).
 */
bool phone_service_available(void);

/**
 * @brief Initiate an outbound call.
 *
 * Resolves contact name to phone number, publishes dial to echo/cmd.
 *
 * @param user_id        User ID for contact lookup.
 * @param name_or_number Contact name or phone number.
 * @param result_buf     Output: human-readable result for the LLM.
 * @param buf_size       Size of result buffer.
 * @return 0 on success, 1 on error.
 */
int phone_service_call(int user_id, const char *name_or_number, char *result_buf, size_t buf_size);

/**
 * @brief Answer an incoming call.
 */
int phone_service_answer(int user_id, char *result_buf, size_t buf_size);

/**
 * @brief Hang up the current call.
 */
int phone_service_hangup(int user_id, char *result_buf, size_t buf_size);

/**
 * @brief Send an SMS.
 *
 * Resolves contact name, publishes send_sms to echo/cmd.
 */
int phone_service_send_sms(int user_id,
                           const char *name_or_number,
                           const char *body,
                           char *result_buf,
                           size_t buf_size);

/**
 * @brief Get the current phone state.
 */
phone_state_t phone_service_get_state(void);

/**
 * @brief Handle an event from echo/events (called from MQTT on_message).
 *
 * Parses the event JSON and updates call state, inserts DB records,
 * sends TTS announcements and HUD MQTT notifications.
 *
 * @param payload     JSON payload from echo/events.
 * @param payload_len Length of payload.
 */
void phone_service_handle_event(const char *payload, int payload_len);

/**
 * @brief Handle a response from echo/response (called from command_router).
 *
 * This is for responses that need state machine updates beyond what
 * command_router_wait already provides (e.g., call_connected arriving
 * before dial response).
 */
void phone_service_handle_response(const char *payload, int payload_len);

/**
 * @brief Get the phone service configuration (for tool layer).
 */
const phone_service_config_t *phone_service_get_config(void);

#endif /* PHONE_SERVICE_H */
