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
 * Suit-telemetry tool — LLM-facing descriptor ("suit_status") that reports live
 * helmet environment/orientation/position (AURA) and armor status (SPARK) from
 * the suit_service cache.  Registered in src/tools/tools_init.c.
 */

#ifndef SUIT_TOOL_H
#define SUIT_TOOL_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Register the suit_status tool with the tool registry. */
int suit_tool_register(void);

#ifdef __cplusplus
}
#endif

#endif /* SUIT_TOOL_H */
