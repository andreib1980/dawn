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
 * Phone Tool — LLM tool interface for phone calls and SMS.
 */

#ifndef PHONE_TOOL_H
#define PHONE_TOOL_H

#include "tools/phone_audio_config.h"

/**
 * @brief Register the phone tool with the tool registry.
 * @return 0 on success, non-zero on failure.
 */
int phone_tool_register(void);

/**
 * @brief Apply a new call-audio config at runtime (from the WebUI settings save).
 *
 * Updates the module state and forwards to phone_service, which persists it for
 * the next call and live-applies to a call in progress.  Thread-safe.
 */
void phone_tool_update_config(const phone_audio_config_t *audio);

/**
 * @brief Snapshot the current call-audio config (for the WebUI GET). Thread-safe.
 */
void phone_tool_get_audio_config(phone_audio_config_t *out);

#endif /* PHONE_TOOL_H */
