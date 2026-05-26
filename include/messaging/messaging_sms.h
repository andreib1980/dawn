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
 * SMS driver — public registration entry point.
 *
 * Unlike Telegram/Discord/Slack drivers, the SMS driver does NOT own a
 * listener thread.  Inbound SMS flows through the existing ECHO MQTT
 * pipeline (phone_service.c) which calls
 * messaging_engine_handle_sms_inbound() on every received SMS.  This
 * driver provides the OUTBOUND path (send_text → phone_service_send_sms)
 * plus the address-validation hook used by /link.
 */
#ifndef MESSAGING_SMS_H
#define MESSAGING_SMS_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the SMS driver with the messaging engine.
 *
 * Always succeeds (no token required — modem connection lifecycle
 * belongs to ECHO).  If the modem is offline at send time,
 * phone_service_send_sms() returns failure and that propagates back
 * through the engine.
 *
 * Must be called AFTER messaging_engine_init().
 *
 * @return SUCCESS / FAILURE.
 */
int messaging_sms_register(void);

/**
 * @brief Tear the driver down.  No-op for the SMS path — nothing to
 *        join / close.
 */
void messaging_sms_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* MESSAGING_SMS_H */
