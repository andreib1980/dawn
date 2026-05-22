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
 * Messaging LLM tool — exposes messaging_engine to the LLM.  See
 * docs/MESSAGING_CHANNELS_DESIGN.md §3.
 */
#ifndef MESSAGING_TOOL_H
#define MESSAGING_TOOL_H

#ifdef __cplusplus
extern "C" {
#endif

int messaging_tool_register(void);

#ifdef __cplusplus
}
#endif

#endif /* MESSAGING_TOOL_H */
